#include "server.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace kretrieve;

namespace {

const std::vector<Document> kCorpus = {
    {10, "cat cat dog"}, {20, "dog bird"}, {30, "cat bird bird"}};

void expect_readme_hits(const std::vector<SearchHit>& hits) {
  ASSERT_EQ(hits.size(), 3u);
  EXPECT_EQ(hits[0].document_id, 10u);
  EXPECT_NEAR(hits[0].score, 1.071445295349, 1e-12);
  EXPECT_EQ(hits[1].document_id, 20u);
  EXPECT_NEAR(hits[1].score, 0.523548346502, 1e-12);
  EXPECT_EQ(hits[2].document_id, 30u);
  EXPECT_NEAR(hits[2].score, 0.447138587823, 1e-12);
}

Frame request_frame(uint64_t req_id, const std::string& query, uint32_t k = 10) {
  return {MsgType::QueryRequest, req_id, encode_request({k, 0, query})};
}

// Send one request and read its response; nullopt if the server closed the connection.
std::optional<QueryResponse> query(int fd, uint64_t req_id, const std::string& text) {
  if (!send_all(fd, encode_frame(request_frame(req_id, text)))) return std::nullopt;
  const auto reply = read_frame(fd);
  if (!reply) return std::nullopt;
  EXPECT_EQ(reply->type, MsgType::QueryResponse);
  EXPECT_EQ(reply->req_id, req_id);
  return decode_response(reply->body);
}

class RunningShard {
 public:
  explicit RunningShard(size_t max_connections = 64, size_t workers = 2,
                        std::span<const Document> corpus = kCorpus, int send_buffer = 0)
      : index_(corpus, compute_global_stats(corpus)) {
    auto listener = listen_tcp("127.0.0.1", 0);
    if (send_buffer != 0) {
      setsockopt(listener.get(), SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof send_buffer);
    }
    port = local_port(listener.get());
    server_ = std::make_unique<FrameServer>(index_, std::move(listener), max_connections, workers);
    thread_ = std::thread([this] { server_->run(); });
  }
  ~RunningShard() { stop(); }
  void stop() {
    server_->stop();
    if (thread_.joinable()) thread_.join();
  }
  Fd connect() const {
    auto fd = connect_tcp("127.0.0.1", port);
    const timeval timeout{5, 0};
    setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    return fd;
  }

  uint16_t port = 0;

 private:
  const InvertedIndex index_;
  std::unique_ptr<FrameServer> server_;
  std::thread thread_;
};

}  // namespace

TEST(HandleRequest, AnswersQueriesAndFlagsBadInput) {
  const InvertedIndex index(kCorpus, compute_global_stats(kCorpus));

  const auto reply = handle_request(index, request_frame(77, "cat dog"));
  ASSERT_TRUE(reply);
  EXPECT_EQ(reply->type, MsgType::QueryResponse);
  EXPECT_EQ(reply->req_id, 77u);
  const auto response = decode_response(reply->body);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, ResponseStatus::Ok);
  expect_readme_hits(response->hits);

  const auto bad = handle_request(index, {MsgType::QueryRequest, 5, {1, 2, 3}});
  ASSERT_TRUE(bad);
  EXPECT_EQ(decode_response(bad->body)->status, ResponseStatus::BadRequest);

  EXPECT_FALSE(handle_request(index, {MsgType::QueryResponse, 5, {}}));
}

TEST(HandleRequest, DropsRequestsWhoseBudgetExpired) {
  const InvertedIndex index(kCorpus, compute_global_stats(kCorpus));
  const auto long_ago = Clock::now() - std::chrono::seconds(1);
  const Frame expired{MsgType::QueryRequest, 1, encode_request({10, 1000, "cat"})};
  EXPECT_FALSE(handle_request(index, expired, long_ago));
  const Frame fresh{MsgType::QueryRequest, 1, encode_request({10, 60'000'000, "cat"})};
  EXPECT_TRUE(handle_request(index, fresh, long_ago));
  // budget_us == 0 means no deadline; a huge budget must not overflow.
  EXPECT_TRUE(handle_request(index, request_frame(1, "cat"), long_ago));
  const Frame huge{MsgType::QueryRequest, 1, encode_request({10, UINT64_MAX, "cat"})};
  EXPECT_TRUE(handle_request(index, huge, long_ago));
}

TEST(ReadFrame, GivesUpAtTheDeadline) {
  int pair[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
  const Fd reader(pair[0]);
  const Fd writer(pair[1]);
  const auto start = Clock::now();
  EXPECT_FALSE(read_frame(reader.get(), start + std::chrono::milliseconds(50)));
  const auto waited = Clock::now() - start;
  EXPECT_GE(waited, std::chrono::milliseconds(50));
  EXPECT_LT(waited, std::chrono::seconds(2));
}

TEST(ReadFrame, ReassemblesSplitWritesAndRejectsOversizedPrefix) {
  int pair[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
  const Fd reader(pair[0]);
  Fd writer(pair[1]);

  const auto wire = encode_frame(request_frame(9, "cat"));
  std::thread slow_writer([&] {
    for (const uint8_t byte : wire) ASSERT_EQ(::write(writer.get(), &byte, 1), 1);
    const uint8_t huge[4] = {0xff, 0xff, 0xff, 0x7f};
    ASSERT_EQ(::write(writer.get(), huge, 4), 4);  // rejected without reading a body
  });
  const auto frame = read_frame(reader.get());
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->req_id, 9u);
  EXPECT_FALSE(read_frame(reader.get()));
  slow_writer.join();

  writer = Fd();
  EXPECT_FALSE(read_frame(reader.get()));  // clean EOF
}

TEST(FrameServer, ServesSequentialAndConcurrentClients) {
  RunningShard shard;
  const auto connection = shard.connect();
  for (uint64_t id = 1; id <= 3; ++id) {
    const auto response = query(connection.get(), id, "cat dog");
    ASSERT_TRUE(response);
    expect_readme_hits(response->hits);
  }

  std::barrier start(8);
  std::vector<std::thread> clients;
  for (uint64_t id = 0; id < 8; ++id) {
    clients.emplace_back([&shard, &start, id] {
      const auto fd = shard.connect();
      start.arrive_and_wait();
      const auto response = query(fd.get(), 100 + id, "cat dog");
      ASSERT_TRUE(response);
      expect_readme_hits(response->hits);
    });
  }
  for (auto& client : clients) client.join();
}

TEST(FrameServer, BadBodyKeepsConnectionButBadFrameClosesIt) {
  RunningShard shard;
  const auto connection = shard.connect();
  ASSERT_TRUE(send_all(connection.get(), encode_frame({MsgType::QueryRequest, 1, {0xff}})));
  const auto reply = read_frame(connection.get());
  ASSERT_TRUE(reply);
  EXPECT_EQ(decode_response(reply->body)->status, ResponseStatus::BadRequest);
  ASSERT_TRUE(query(connection.get(), 2, "cat dog"));  // still usable

  auto wire = encode_frame(request_frame(3, "cat"));
  wire[4] = 0xff;  // bad protocol version
  ASSERT_TRUE(send_all(connection.get(), wire));
  EXPECT_FALSE(read_frame(connection.get()));

  const auto second = shard.connect();
  ASSERT_TRUE(send_all(second.get(), encode_frame({MsgType::QueryResponse, 4, {}})));
  EXPECT_FALSE(read_frame(second.get()));

  const auto third = shard.connect();
  const std::vector<uint8_t> huge = {0xff, 0xff, 0xff, 0x7f};
  ASSERT_TRUE(send_all(third.get(), huge));
  EXPECT_FALSE(read_frame(third.get()));  // invalid length needs no body
}

TEST(FrameServer, RejectsConnectionsOverCapUntilASlotFrees) {
  RunningShard shard(1);
  auto first = shard.connect();
  ASSERT_TRUE(query(first.get(), 1, "cat"));  // first is now registered as a connection

  const auto rejected = shard.connect();
  EXPECT_FALSE(query(rejected.get(), 2, "cat"));

  first = Fd();
  bool served = false;  // the event loop observes EOF asynchronously; retry briefly
  for (int attempt = 0; attempt < 200 && !served; ++attempt) {
    const auto retry = shard.connect();
    served = query(retry.get(), 3, "cat").has_value();
    if (!served) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(served);
}

TEST(FrameServer, StopReturnsWithIdleClientsConnected) {
  RunningShard shard;
  const auto idle_a = shard.connect();
  const auto idle_b = shard.connect();
  ASSERT_TRUE(query(idle_a.get(), 1, "cat"));
  shard.stop();
  EXPECT_FALSE(read_frame(idle_a.get()));
}

TEST(FrameServer, IdleConnectionsDoNotCreateThreads) {
  const auto thread_count = [] {
    const auto tasks = std::filesystem::directory_iterator("/proc/self/task");
    return std::distance(begin(tasks), end(tasks));
  };
  RunningShard shard(64, 2);
  const auto first = shard.connect();
  ASSERT_TRUE(query(first.get(), 1, "cat"));
  // Measure after startup: sanitizers may lazily create their own background thread.
  const auto baseline = thread_count();
  std::vector<Fd> idle;
  for (int i = 0; i < 32; ++i) idle.push_back(shard.connect());
  ASSERT_TRUE(query(idle.back().get(), 1, "cat"));
  EXPECT_EQ(thread_count(), baseline);
}

TEST(FrameServer, PartialRequestDoesNotBlockOtherClients) {
  RunningShard shard(64, 1);
  const auto slow = shard.connect();
  const auto wire = encode_frame(request_frame(11, "cat dog"));
  ASSERT_TRUE(send_all(slow.get(), std::span(wire).first(3)));
  const auto other = shard.connect();
  ASSERT_TRUE(query(other.get(), 12, "cat"));
  // Complete the prefix, but leave the body incomplete across another query.
  ASSERT_TRUE(send_all(slow.get(), std::span(wire).subspan(3, 4)));
  ASSERT_TRUE(query(other.get(), 13, "dog"));
  ASSERT_TRUE(send_all(slow.get(), std::span(wire).subspan(7)));
  const auto response = read_frame(slow.get());
  ASSERT_TRUE(response);
  EXPECT_EQ(response->req_id, 11u);
  expect_readme_hits(decode_response(response->body)->hits);
}

TEST(FrameServer, PipelinedRequestsStayOrderedAndSurviveWriteHalfClose) {
  RunningShard shard(64, 1);
  const auto fd = shard.connect();
  std::vector<uint8_t> pipeline;
  for (uint64_t id = 1; id <= 8; ++id) {
    const auto wire = encode_frame(request_frame(id, "cat dog"));
    pipeline.insert(pipeline.end(), wire.begin(), wire.end());
  }
  ASSERT_TRUE(send_all(fd.get(), pipeline));
  ASSERT_EQ(shutdown(fd.get(), SHUT_WR), 0);
  for (uint64_t id = 1; id <= 8; ++id) {
    const auto reply = read_frame(fd.get());
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->req_id, id);
    const auto response = decode_response(reply->body);
    ASSERT_TRUE(response);
    expect_readme_hits(response->hits);
  }
  EXPECT_FALSE(read_frame(fd.get()));
}

TEST(FrameServer, DisconnectChurnDoesNotMixResponses) {
  RunningShard shard(16, 1);
  for (uint64_t id = 1; id <= 100; ++id) {
    {
      const auto abandoned = shard.connect();
      ASSERT_TRUE(send_all(abandoned.get(), encode_frame(request_frame(id, "cat"))));
    }
    const auto fresh = shard.connect();
    const auto response = query(fresh.get(), 1000 + id, "cat dog");
    ASSERT_TRUE(response);
    expect_readme_hits(response->hits);
  }
}

TEST(FrameServer, SlowReaderDoesNotBlockQueriesAndResponsesAreComplete) {
  std::vector<Document> corpus;
  for (uint64_t id = 0; id < 20'000; ++id) corpus.push_back({id, "cat"});
  RunningShard shard(64, 1, corpus, 4096);
  const auto slow = shard.connect();
  // Accepted sockets inherit the small send buffer, forcing partial writes/EAGAIN.
  for (uint64_t id = 1; id <= 4; ++id) {
    ASSERT_TRUE(send_all(slow.get(), encode_frame(request_frame(id, "cat", 20'000))));
  }
  pollfd readable{slow.get(), POLLIN, 0};
  ASSERT_EQ(poll(&readable, 1, 5000), 1);  // response has started, but we are not draining it
  const auto other = shard.connect();
  ASSERT_TRUE(query(other.get(), 100, "cat"));
  for (uint64_t id = 1; id <= 4; ++id) {
    const auto reply = read_frame(slow.get());
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->req_id, id);
    const auto response = decode_response(reply->body);
    ASSERT_TRUE(response);
    ASSERT_EQ(response->hits.size(), corpus.size());
    EXPECT_EQ(response->hits.front().document_id, 0u);
    EXPECT_EQ(response->hits.back().document_id, 19'999u);
  }
}

TEST(FrameServer, StopWithPartialAndQueuedRequests) {
  RunningShard shard(64, 1);
  std::vector<Fd> clients;
  for (uint64_t id = 0; id < 32; ++id) {
    clients.push_back(shard.connect());
    const auto wire = encode_frame(request_frame(id, "cat dog"));
    ASSERT_TRUE(send_all(clients.back().get(), std::span(wire).first(id % 2 ? wire.size() : 3)));
  }
  shard.stop();
  const auto observer = clients.front().get();  // incomplete prefix cannot have a response
  EXPECT_FALSE(read_frame(observer));
}

TEST(FrameServer, RecoversAfterDescriptorExhaustion) {
  RunningShard shard(64, 1);
  const auto active = shard.connect();
  ASSERT_TRUE(query(active.get(), 1, "cat"));  // pool and reactor are fully started
  const Fd waiting(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  ASSERT_GE(waiting.get(), 0);
  const timeval timeout{5, 0};
  ASSERT_EQ(setsockopt(waiting.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout), 0);

  rlimit original{};
  ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &original), 0);
  struct LimitGuard {
    rlimit original;
    ~LimitGuard() { setrlimit(RLIMIT_NOFILE, &original); }
  } limit{original};
  auto reduced = limit.original;
  reduced.rlim_cur = std::min<rlim_t>(limit.original.rlim_cur, 128);
  ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &reduced), 0);
  std::vector<Fd> fillers;
  while (true) {
    const int fd = dup(active.get());
    if (fd < 0) break;
    fillers.emplace_back(fd);
  }
  ASSERT_EQ(errno, EMFILE);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(shard.port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(connect(waiting.get(), reinterpret_cast<sockaddr*>(&address), sizeof address), 0);
  ASSERT_TRUE(send_all(waiting.get(), encode_frame(request_frame(2, "cat"))));
  ASSERT_TRUE(query(active.get(), 3, "dog"));  // existing sockets still make progress
  fillers.clear();
  const auto reply = read_frame(waiting.get());
  ASSERT_TRUE(reply);  // listener retries after descriptors become available
  EXPECT_EQ(reply->req_id, 2u);
}

TEST(FrameServer, FullQueueRepliesOverloadedAndKeepsConnection) {
  const InvertedIndex index(kCorpus, compute_global_stats(kCorpus));
  std::promise<void> entered;
  std::promise<void> release;
  const std::shared_future<void> released = release.get_future().share();
  std::atomic<int> calls{0};
  auto listener = listen_tcp("127.0.0.1", 0);
  const uint16_t port = local_port(listener.get());
  // One worker, one queue slot: the third concurrent request must be shed.
  FrameServer server(
      [&](const Frame& request, Clock::time_point received) {
        if (calls.fetch_add(1) == 0) entered.set_value();
        released.wait();
        return handle_request(index, request, received);
      },
      std::move(listener), 64, /*workers=*/1, /*max_queue=*/1);
  std::thread serving([&server] { server.run(); });
  // Unblock the worker and join even if an assertion below fails.
  struct Cleanup {
    std::promise<void>& release;
    FrameServer& server;
    std::thread& serving;
    ~Cleanup() {
      try {
        release.set_value();
      } catch (const std::future_error&) {
      }
      server.stop();
      serving.join();
    }
  } cleanup{release, server, serving};

  const auto ask = [](const Fd& fd, uint64_t id) -> std::optional<QueryResponse> {
    if (!send_all(fd.get(), encode_frame(request_frame(id, "cat")))) return std::nullopt;
    const auto reply = read_frame(fd.get(), Clock::now() + std::chrono::seconds(5));
    if (!reply || reply->req_id != id) return std::nullopt;
    return decode_response(reply->body);
  };
  const auto running = connect_tcp("127.0.0.1", port);
  const auto queued = connect_tcp("127.0.0.1", port);
  const auto shed = connect_tcp("127.0.0.1", port);
  ASSERT_TRUE(send_all(running.get(), encode_frame(request_frame(1, "cat"))));
  entered.get_future().wait();
  ASSERT_TRUE(send_all(queued.get(), encode_frame(request_frame(2, "cat"))));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let the server queue it

  const auto busy = ask(shed, 3);  // answered while the worker is still blocked
  ASSERT_TRUE(busy);
  EXPECT_EQ(busy->status, ResponseStatus::Overloaded);
  EXPECT_TRUE(busy->hits.empty());

  release.set_value();
  for (const auto* fd : {&running, &queued}) {
    const auto reply = read_frame(fd->get(), Clock::now() + std::chrono::seconds(5));
    ASSERT_TRUE(reply);
    EXPECT_EQ(decode_response(reply->body)->status, ResponseStatus::Ok);
  }
  const auto later = ask(shed, 4);  // the shed connection still works
  ASSERT_TRUE(later);
  EXPECT_EQ(later->status, ResponseStatus::Ok);
}
