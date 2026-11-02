#include "coordinator.hpp"
#include "net.hpp"
#include "shard_server.hpp"

#include <gtest/gtest.h>

#include <sys/socket.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace kretrieve;

namespace {

const std::vector<Document> kCorpus = {{1, "cat sat on the mat"}, {2, "dog chased the cat"},
                                       {3, "bird sang"},          {4, "cat cat cat"},
                                       {5, "dog dog bird"},       {6, "the quick brown fox"},
                                       {7, "fox and dog"},        {8, "cat and bird friends"},
                                       {9, "lazy dog sleeps"},    {10, "cat dog bird fox"}};

// One shard server on an ephemeral port, serving a slice of a shared-stats corpus.
class RunningShard {
 public:
  RunningShard(std::vector<Document> documents, const GlobalStats& stats)
      : index_(documents, stats) {
    auto listener = listen_tcp("127.0.0.1", 0);
    port = local_port(listener.get());
    server_ = std::make_unique<ShardServer>(index_, std::move(listener), 64, 2);
    thread_ = std::thread([this] { server_->run(); });
  }
  ~RunningShard() {
    server_->stop();
    thread_.join();
  }

  uint16_t port = 0;

 private:
  const InvertedIndex index_;
  std::unique_ptr<ShardServer> server_;
  std::thread thread_;
};

// Three shards partitioned by id % 3, the same as prepare_corpus.
struct Cluster {
  Cluster() {
    for (size_t s = 0; s < 3; ++s) {
      std::vector<Document> part;
      for (const auto& doc : kCorpus) {
        if (doc.id % 3 == s) part.push_back(doc);
      }
      shards.push_back(std::make_unique<RunningShard>(part, stats));
      addresses.push_back({"127.0.0.1", shards.back()->port});
    }
  }
  GlobalStats stats = compute_global_stats(kCorpus);
  std::vector<std::unique_ptr<RunningShard>> shards;
  std::vector<ShardAddress> addresses;
};

std::optional<QueryResponse> ask(Coordinator& coordinator, const std::string& query, uint32_t k,
                                 uint64_t client_id = 42) {
  const auto reply =
      coordinator.handle({MsgType::QueryRequest, client_id, encode_request({k, 0, query})});
  if (!reply) return std::nullopt;
  EXPECT_EQ(reply->type, MsgType::QueryResponse);
  EXPECT_EQ(reply->req_id, client_id);  // the client's id is echoed, not the internal one
  return decode_response(reply->body);
}

void expect_same_hits(const std::vector<SearchHit>& actual,
                      const std::vector<SearchHit>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); ++i) {
    EXPECT_EQ(actual[i].document_id, expected[i].document_id) << "rank " << i;
    EXPECT_DOUBLE_EQ(actual[i].score, expected[i].score) << "rank " << i;
  }
}

}  // namespace

TEST(MergeTopK, OrdersByScoreThenDocIdAndKeepsK) {
  const std::vector<std::vector<SearchHit>> shards = {
      {{5, 2.0}, {1, 1.0}}, {}, {{3, 2.0}, {2, 1.0}, {9, 0.5}}};
  expect_same_hits(merge_top_k(shards, 10), {{3, 2.0}, {5, 2.0}, {1, 1.0}, {2, 1.0}, {9, 0.5}});
  expect_same_hits(merge_top_k(shards, 3), {{3, 2.0}, {5, 2.0}, {1, 1.0}});
  EXPECT_TRUE(merge_top_k(shards, 0).empty());
  EXPECT_TRUE(merge_top_k({}, 5).empty());
}

TEST(ParseShardList, AcceptsHostPortListsAndRejectsJunk) {
  const auto shards = parse_shard_list("127.0.0.1:9000,localhost:9001");
  ASSERT_EQ(shards.size(), 2u);
  EXPECT_EQ(shards[1].host, "localhost");
  EXPECT_EQ(shards[1].port, 9001);
  for (const char* bad : {"", "host", ":9000", "host:", "host:0", "host:70000", "a:1,", "a:1x"}) {
    EXPECT_THROW(parse_shard_list(bad), std::invalid_argument) << bad;
  }
}

TEST(Coordinator, ThreeShardsMatchUnshardedSearch) {
  Cluster cluster;
  Coordinator coordinator(cluster.addresses, 6);
  const InvertedIndex full(kCorpus, cluster.stats);
  for (const std::string query : {"cat", "dog bird", "fox cat dog", "missing"}) {
    for (const uint32_t k : {0u, 1u, 3u, 100u}) {
      const auto response = ask(coordinator, query, k);
      ASSERT_TRUE(response) << query;
      EXPECT_EQ(response->status, ResponseStatus::Ok);
      expect_same_hits(response->hits, full.search(query, k));
    }
  }
}

TEST(Coordinator, BadRequestBodyAndWrongFrameType) {
  Cluster cluster;
  Coordinator coordinator(cluster.addresses, 3);
  const auto bad = coordinator.handle({MsgType::QueryRequest, 7, {1, 2, 3}});
  ASSERT_TRUE(bad);
  EXPECT_EQ(decode_response(bad->body)->status, ResponseStatus::BadRequest);
  EXPECT_FALSE(coordinator.handle({MsgType::QueryResponse, 7, {}}));
}

TEST(Coordinator, DeadShardFailsTheQuery) {
  Cluster cluster;
  auto addresses = cluster.addresses;
  {
    auto closed = listen_tcp("127.0.0.1", 0);  // grab a free port, then close it
    addresses.push_back({"127.0.0.1", local_port(closed.get())});
  }
  Coordinator coordinator(addresses, 4);
  const auto response = ask(coordinator, "cat", 10);
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, ResponseStatus::InternalError);
}

TEST(Coordinator, MismatchedShardReqIdFailsTheQuery) {
  // A fake shard that answers with the wrong request id.
  auto listener = listen_tcp("127.0.0.1", 0);
  const uint16_t port = local_port(listener.get());
  std::thread fake([&listener] {
    const Fd client(accept(listener.get(), nullptr, nullptr));
    const auto request = read_frame(client.get());
    ASSERT_TRUE(request);
    const Frame wrong{MsgType::QueryResponse, request->req_id + 1, encode_response({})};
    send_all(client.get(), encode_frame(wrong));
  });
  Coordinator coordinator({{"127.0.0.1", port}}, 1);
  const auto response = ask(coordinator, "cat", 10);
  fake.join();
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, ResponseStatus::InternalError);
}

TEST(Coordinator, ConcurrentQueriesThroughTheCoordinatorServer) {
  Cluster cluster;
  Coordinator coordinator(cluster.addresses, 3 * 4);
  auto listener = listen_tcp("127.0.0.1", 0);
  const uint16_t port = local_port(listener.get());
  ShardServer server([&coordinator](const Frame& f) { return coordinator.handle(f); },
                     std::move(listener), 64, 4);
  std::thread serving([&server] { server.run(); });

  const InvertedIndex full(kCorpus, cluster.stats);
  const auto expected = full.search("cat dog", 5);
  std::vector<std::thread> clients;
  for (uint64_t c = 0; c < 8; ++c) {
    clients.emplace_back([&, c] {
      const auto fd = connect_tcp("127.0.0.1", port);
      for (uint64_t i = 0; i < 20; ++i) {
        const uint64_t id = c * 1000 + i;
        ASSERT_TRUE(send_all(fd.get(), encode_frame({MsgType::QueryRequest, id,
                                                     encode_request({5, 0, "cat dog"})})));
        const auto reply = read_frame(fd.get());
        ASSERT_TRUE(reply);
        EXPECT_EQ(reply->req_id, id);
        const auto response = decode_response(reply->body);
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, ResponseStatus::Ok);
        expect_same_hits(response->hits, expected);
      }
    });
  }
  for (auto& client : clients) client.join();
  server.stop();
  serving.join();
}
