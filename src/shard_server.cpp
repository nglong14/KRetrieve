#include "shard_server.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <list>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace kretrieve {

std::optional<Frame> handle_request(const InvertedIndex& index, const Frame& request) {
  if (request.type != MsgType::QueryRequest) return std::nullopt;
  QueryResponse response;
  if (const auto query = decode_request(request.body)) {
    try {
      response.hits = index.search(query->query, query->k);
    } catch (const std::exception&) {
      response.status = ResponseStatus::InternalError;
    }
  } else {
    response.status = ResponseStatus::BadRequest;
  }
  return Frame{MsgType::QueryResponse, request.req_id, encode_response(response)};
}

namespace {

Fd checked_fd(int fd, const char* operation) {
  if (fd < 0) throw std::system_error(errno, std::generic_category(), operation);
  return Fd(fd);
}

// Runtime state is local to run(): unwinding also stops and joins a partially started pool.
class Reactor {
 public:
  Reactor(const InvertedIndex& index, Fd listener, int stop_fd, size_t cap)
      : index_(index),
        listener_(std::move(listener)),
        epoll_(checked_fd(epoll_create1(EPOLL_CLOEXEC), "epoll_create1")),
        completed_fd_(checked_fd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), "eventfd")),
        cap_(cap) {
    const int flags = fcntl(listener_.get(), F_GETFL);
    if (flags < 0 || fcntl(listener_.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
      throw std::system_error(errno, std::generic_category(), "nonblocking listener");
    }
    watch(EPOLL_CTL_ADD, listener_.get(), EPOLLIN, kListener);
    watch(EPOLL_CTL_ADD, stop_fd, EPOLLIN, kStop);
    watch(EPOLL_CTL_ADD, completed_fd_.get(), EPOLLIN, kCompleted);
  }

  ~Reactor() {
    listener_ = Fd();
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
      jobs_.clear();
    }
    ready_.notify_all();
    connections_.clear();
    pending_.clear();
    for (auto& worker : workers_) worker.join();
  }

  void run(size_t worker_count) {
    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) workers_.emplace_back([this] { work(); });

    std::array<epoll_event, 64> events{};
    while (true) {
      int timeout = -1;
      if (!accepting_) {
        const auto remaining = retry_accept_ - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
          watch(EPOLL_CTL_ADD, listener_.get(), EPOLLIN, kListener);
          accepting_ = true;
        } else {
          timeout = static_cast<int>(
              std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count() + 1);
        }
      }
      const int count = epoll_wait(epoll_.get(), events.data(), events.size(), timeout);
      if (count < 0) {
        if (errno == EINTR) continue;
        throw std::system_error(errno, std::generic_category(), "epoll_wait");
      }
      // Prioritize shutdown even when sockets and completions are also ready.
      for (int i = 0; i < count; ++i) {
        if (events[i].data.u64 == kStop) return;
      }
      for (int i = 0; i < count; ++i) {
        const auto id = events[i].data.u64;
        if (id == kListener) {
          accept_clients();
        } else if (id == kCompleted) {
          complete();
        } else if (auto found = connections_.find(id); found != connections_.end()) {
          auto& connection = found->second;
          bool keep = true;
          if (events[i].events & EPOLLERR) {
            keep = false;
          } else if (connection.phase == Phase::Reading) {
            keep = read_request(id, connection);
          } else if (connection.phase == Phase::Writing) {
            keep = write_response(id, connection);
          }
          if (!keep) close_client(id);
        }
      }
      admit();
    }
  }

 private:
  static constexpr uint64_t kListener = 0;
  static constexpr uint64_t kStop = 1;
  static constexpr uint64_t kCompleted = 2;
  enum class Phase { Reading, Busy, Writing };
  struct Connection {
    Fd fd;
    Phase phase = Phase::Reading;
    std::vector<uint8_t> buffer = std::vector<uint8_t>(kFramePrefixSize);
    size_t offset = 0;
  };
  struct Job {
    uint64_t connection;
    Frame request;
    std::vector<uint8_t> response;  // empty means close the connection
  };

  void watch(int operation, int fd, uint32_t events, uint64_t id) {
    epoll_event event{};
    event.events = events;
    event.data.u64 = id;
    if (epoll_ctl(epoll_.get(), operation, fd, &event) < 0) {
      throw std::system_error(errno, std::generic_category(), "epoll_ctl");
    }
  }

  void close_client(uint64_t id) {
    const auto found = connections_.find(id);
    if (found == connections_.end()) return;
    // Busy sockets are already unregistered. close() also removes epoll registrations.
    ::shutdown(found->second.fd.get(), SHUT_RDWR);
    connections_.erase(found);
  }

  void accept_clients() {
    // Bound each batch so an accept flood cannot starve existing clients or stop().
    for (int i = 0; i < 64; ++i) {
      Fd client(accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
      if (client.get() < 0) {
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
          watch(EPOLL_CTL_DEL, listener_.get(), 0, kListener);
          accepting_ = false;
          retry_accept_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
          return;
        }
        throw std::system_error(errno, std::generic_category(), "accept4");
      }
      if (connections_.size() >= cap_) continue;
      const int one = 1;
      setsockopt(client.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      // Never use fd numbers as identities: stale events/completions cannot target a reused fd.
      const uint64_t id = next_id_++;
      auto& connection = connections_.emplace(id, Connection{std::move(client)}).first->second;
      watch(EPOLL_CTL_ADD, connection.fd.get(), EPOLLIN, id);
    }
  }

  bool read_request(uint64_t id, Connection& connection) {
    auto& buffer = connection.buffer;
    while (true) {
      const ssize_t got = recv(connection.fd.get(), buffer.data() + connection.offset,
                               buffer.size() - connection.offset, 0);
      if (got < 0 && errno == EINTR) continue;
      if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
      if (got <= 0) return false;
      connection.offset += static_cast<size_t>(got);
      if (connection.offset != buffer.size()) continue;

      if (buffer.size() == kFramePrefixSize) {
        if (decode_frame(buffer).status == DecodeStatus::Invalid) return false;
        const uint32_t len = uint32_t{buffer[0]} | (uint32_t{buffer[1]} << 8) |
                             (uint32_t{buffer[2]} << 16) | (uint32_t{buffer[3]} << 24);
        buffer.resize(kFramePrefixSize + len);
        continue;
      }
      auto decoded = decode_frame(buffer);
      if (decoded.status != DecodeStatus::Ok) return false;
      pending_.push_back({id, std::move(decoded.frame), {}});
      std::vector<uint8_t>().swap(buffer);  // do not retain large request buffers
      connection.offset = 0;
      connection.phase = Phase::Busy;
      // ponytail: one in-flight request per connection; multiplex only if needed.
      // Unregister to avoid repeated HUP events while a job is waiting/running.
      watch(EPOLL_CTL_DEL, connection.fd.get(), 0, id);
      return true;
    }
  }

  bool write_response(uint64_t id, Connection& connection) {
    auto& buffer = connection.buffer;
    while (connection.offset < buffer.size()) {
      const ssize_t sent = send(connection.fd.get(), buffer.data() + connection.offset,
                                buffer.size() - connection.offset, MSG_NOSIGNAL);
      if (sent < 0 && errno == EINTR) continue;
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
      if (sent <= 0) return false;
      connection.offset += static_cast<size_t>(sent);
    }
    buffer = std::vector<uint8_t>(kFramePrefixSize);
    connection.offset = 0;
    connection.phase = Phase::Reading;
    watch(EPOLL_CTL_MOD, connection.fd.get(), EPOLLIN, id);
    return true;
  }

  void admit() {
    if (pending_.empty()) return;
    std::lock_guard lock(mutex_);
    // Includes running jobs and completions not yet consumed, even after a disconnect.
    while (!pending_.empty() && outstanding_ < cap_) {
      jobs_.splice(jobs_.end(), pending_, pending_.begin());
      ++outstanding_;
      ready_.notify_one();
    }
  }

  void work() {
    std::list<Job> current;
    while (true) {
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (stopping_) return;
        current.splice(current.end(), jobs_, jobs_.begin());
      }
      auto& job = current.front();
      try {
        if (const auto response = handle_request(index_, job.request)) {
          job.response = encode_frame(*response);
        }
      } catch (...) {
        // Allocation/encoding failures also stay inside the worker boundary.
        job.response.clear();
      }
      job.request = Frame();
      {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        // Reuse the job node: publishing a completion cannot fail an allocation.
        completed_.splice(completed_.end(), current);
      }
      const uint64_t one = 1;
      while (::write(completed_fd_.get(), &one, sizeof one) < 0 && errno == EINTR) {
      }
      // EAGAIN means a wakeup is already pending; the queue, not the counter, owns results.
    }
  }

  void complete() {
    uint64_t notifications;
    while (::read(completed_fd_.get(), &notifications, sizeof notifications) < 0 &&
           errno == EINTR) {
    }
    std::list<Job> results;
    {
      std::lock_guard lock(mutex_);
      results.splice(results.end(), completed_);
    }
    for (auto& job : results) {
      --outstanding_;
      const auto found = connections_.find(job.connection);
      if (found == connections_.end()) continue;
      if (job.response.empty()) {
        close_client(job.connection);
      } else {
        auto& connection = found->second;
        connection.buffer = std::move(job.response);
        connection.phase = Phase::Writing;
        watch(EPOLL_CTL_ADD, connection.fd.get(), EPOLLOUT, job.connection);
      }
    }
  }

  const InvertedIndex& index_;
  Fd listener_;
  Fd epoll_;
  Fd completed_fd_;
  size_t cap_;
  uint64_t next_id_ = 3;
  std::unordered_map<uint64_t, Connection> connections_;
  std::list<Job> pending_;
  size_t outstanding_ = 0;  // event-loop owned
  bool accepting_ = true;
  std::chrono::steady_clock::time_point retry_accept_;

  std::mutex mutex_;
  std::condition_variable ready_;
  bool stopping_ = false;  // mutex protected, as are jobs_ and completed_
  std::list<Job> jobs_;
  std::list<Job> completed_;
  std::vector<std::thread> workers_;
};

}  // namespace

ShardServer::ShardServer(const InvertedIndex& index, Fd listener, size_t max_connections,
                         size_t workers)
    : index_(index),
      listener_(std::move(listener)),
      max_connections_(max_connections),
      worker_count_(
          workers == 0
              ? std::min(max_connections, size_t{std::max(1u, std::thread::hardware_concurrency())})
              : workers) {
  if (max_connections == 0) throw std::invalid_argument("max_connections must be positive");
  int fds[2];
  if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
    throw std::runtime_error(std::string("pipe2: ") + std::strerror(errno));
  }
  stop_read_ = Fd(fds[0]);
  stop_write_ = Fd(fds[1]);
}

void ShardServer::stop() {
  const int saved_errno = errno;
  const char byte = 1;
  while (::write(stop_write_.get(), &byte, 1) < 0 && errno == EINTR) {
  }
  errno = saved_errno;
}

void ShardServer::run() {
  Reactor reactor(index_, std::move(listener_), stop_read_.get(), max_connections_);
  reactor.run(worker_count_);
}

}  // namespace kretrieve
