#include "net.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace kretrieve {
namespace {

// Milliseconds left, rounded up so we never poll(0) before the deadline; -1 = no deadline.
int remaining_ms(Deadline deadline) {
  if (!deadline) return -1;
  const auto left = *deadline - Clock::now();
  if (left <= Clock::duration::zero()) return 0;
  const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
  return static_cast<int>(std::min<decltype(ms)>(ms, std::numeric_limits<int>::max()));
}

// Try each resolved address until one binds+listens (server) or connects (client).
Fd open_tcp(const std::string& host, uint16_t port, bool server, Deadline deadline) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV | (server ? AI_PASSIVE : 0);
  addrinfo* results = nullptr;
  const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results);
  if (rc != 0) throw std::runtime_error("resolve " + host + ": " + gai_strerror(rc));

  int error = 0;
  Fd fd;
  for (const addrinfo* ai = results; ai != nullptr; ai = ai->ai_next) {
    Fd candidate(socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol));
    if (candidate.get() < 0) {
      error = errno;
      continue;
    }
    const int one = 1;
    bool ok = false;
    if (server) {
      setsockopt(candidate.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
      ok = bind(candidate.get(), ai->ai_addr, ai->ai_addrlen) == 0 &&
           listen(candidate.get(), SOMAXCONN) == 0;
    } else {
      if (deadline) {
        // Linux applies SO_SNDTIMEO to connect(); it fails with EINPROGRESS on timeout.
        const auto left = std::chrono::ceil<std::chrono::microseconds>(*deadline - Clock::now());
        if (left.count() <= 0) {
          error = ETIMEDOUT;
          break;
        }
        const timeval tv{static_cast<time_t>(left.count() / 1'000'000),
                         static_cast<suseconds_t>(left.count() % 1'000'000)};
        setsockopt(candidate.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
      }
      ok = connect(candidate.get(), ai->ai_addr, ai->ai_addrlen) == 0;
      if (ok) setsockopt(candidate.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    if (ok) {
      fd = std::move(candidate);
      break;
    }
    error = errno;
  }
  freeaddrinfo(results);
  if (fd.get() < 0) {
    throw std::runtime_error((server ? "listen on " : "connect to ") + host + ":" +
                             std::to_string(port) + ": " + std::strerror(error));
  }
  return fd;
}

bool recv_exact(int fd, uint8_t* p, size_t n, Deadline deadline) {
  while (n > 0) {
    if (deadline) {
      pollfd pfd{fd, POLLIN, 0};
      const int ready = poll(&pfd, 1, remaining_ms(deadline));
      if (ready < 0 && errno == EINTR) continue;
      if (ready <= 0) return false;  // error or deadline passed
    }
    const ssize_t got = recv(fd, p, n, 0);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) return false;
    p += got;
    n -= static_cast<size_t>(got);
  }
  return true;
}

}  // namespace

void Fd::reset() {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

Fd listen_tcp(const std::string& host, uint16_t port) { return open_tcp(host, port, true, {}); }
Fd connect_tcp(const std::string& host, uint16_t port, Deadline deadline) {
  return open_tcp(host, port, false, deadline);
}

uint16_t local_port(int fd) {
  sockaddr_storage addr{};
  socklen_t len = sizeof addr;
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    throw std::runtime_error(std::string("getsockname: ") + std::strerror(errno));
  }
  if (addr.ss_family == AF_INET6) return ntohs(reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
  return ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
}

bool send_all(int fd, std::span<const uint8_t> data) {
  while (!data.empty()) {
    const ssize_t sent = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent < 0 && errno == EINTR) continue;
    if (sent <= 0) return false;
    data = data.subspan(static_cast<size_t>(sent));
  }
  return true;
}

std::optional<Frame> read_frame(int fd, Deadline deadline) {
  std::vector<uint8_t> buffer(kFramePrefixSize);
  if (!recv_exact(fd, buffer.data(), kFramePrefixSize, deadline)) return std::nullopt;
  // decode_frame rejects a bad length from the prefix alone, before we allocate.
  if (decode_frame(buffer).status == DecodeStatus::Invalid) return std::nullopt;
  const uint32_t len = uint32_t{buffer[0]} | (uint32_t{buffer[1]} << 8) |
                       (uint32_t{buffer[2]} << 16) | (uint32_t{buffer[3]} << 24);
  buffer.resize(kFramePrefixSize + len);
  if (!recv_exact(fd, buffer.data() + kFramePrefixSize, len, deadline)) return std::nullopt;
  auto decoded = decode_frame(buffer);
  if (decoded.status != DecodeStatus::Ok) return std::nullopt;
  return std::move(decoded.frame);
}

}  // namespace kretrieve
