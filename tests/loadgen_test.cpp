#include "loadgen.hpp"
#include "server.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace kretrieve;

namespace {

const std::vector<Document> kCorpus = {{1, "cat sat"}, {2, "dog ran"}, {3, "cat and dog"}};

TEST(Percentile, NearestRank) {
  const std::vector<int64_t> values = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  EXPECT_EQ(percentile(values, 0.5), 5);
  EXPECT_EQ(percentile(values, 0.9), 9);
  EXPECT_EQ(percentile(values, 0.99), 10);
  EXPECT_EQ(percentile(values, 1.0), 10);
  EXPECT_EQ(percentile(values, 0.001), 1);
  EXPECT_EQ(percentile({}, 0.5), 0);
}

TEST(RunLoad, SendsOnScheduleAndMeasuresFromDueTime) {
  const InvertedIndex index(kCorpus, compute_global_stats(kCorpus));
  auto listener = listen_tcp("127.0.0.1", 0);
  LoadConfig config;
  config.port = local_port(listener.get());
  FrameServer server(index, std::move(listener), 64, 2);
  std::thread serving([&server] { server.run(); });

  config.qps = 200;
  config.duration = std::chrono::milliseconds(500);
  config.queries = {"cat", "dog"};
  config.connections = 4;
  const auto samples = run_load(config);
  server.stop();
  serving.join();

  ASSERT_EQ(samples.size(), 100u);
  for (size_t i = 0; i < samples.size(); ++i) {
    EXPECT_EQ(samples[i].status, static_cast<int>(ResponseStatus::Ok)) << i;
    EXPECT_EQ(samples[i].hits, 2u) << i;  // "cat" and "dog" each match two documents
    EXPECT_NEAR(samples[i].due_us, static_cast<int64_t>(i) * 5000, 1) << i;
    EXPECT_GT(samples[i].latency_us, 0) << i;
  }
}

TEST(RunLoad, RecordsTransportErrorsWhenNothingListens) {
  LoadConfig config;
  {
    auto closed = listen_tcp("127.0.0.1", 0);  // grab a free port, then close it
    config.port = local_port(closed.get());
  }
  config.qps = 50;
  config.duration = std::chrono::milliseconds(100);
  config.queries = {"cat"};
  config.connections = 2;
  const auto samples = run_load(config);
  ASSERT_EQ(samples.size(), 5u);
  for (const auto& sample : samples) EXPECT_EQ(sample.status, kTransportError);
}

}  // namespace
