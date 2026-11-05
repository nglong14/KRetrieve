// Scatter-gather coordinator: fan a query out to every shard and merge top-K.
#pragma once

#include "index.hpp"
#include "net.hpp"
#include "protocol.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kretrieve {

struct ShardAddress {
  std::string host;
  uint16_t port = 0;
};

// Parse "host:port,host:port". Throws std::invalid_argument on bad input.
std::vector<ShardAddress> parse_shard_list(std::string_view list);

// Best k hits across all shard lists, ordered by ranks_before.
std::vector<SearchHit> merge_top_k(std::span<const std::vector<SearchHit>> shard_hits, size_t k);

class Coordinator {
 public:
  // pool_threads should be at least shards x concurrent queries, so fan-out never queues.
  Coordinator(std::vector<ShardAddress> shards, size_t pool_threads,
              std::vector<ShardAddress> replicas = {});

  // ponytail: fixed constants; make them flags when someone needs to tune them.
  static constexpr std::chrono::microseconds kDefaultBudget{1'000'000};  // client sent 0
  static constexpr std::chrono::microseconds kMaxBudget{60'000'000};

  // Thread-safe. Same contract as handle_request: nullopt closes the client connection.
  // The deadline is received + budget_us. Shards that fail or miss it are left out
  // and listed in missing_shards (by index in the shard list); the reply is still Ok.
  // A request whose deadline passed before fan-out (it waited in the queue) gets Overloaded.
  std::optional<Frame> handle(const Frame& request, Clock::time_point received = Clock::now());

 private:
  std::vector<ShardAddress> shards_;
  std::vector<ShardAddress> replicas_;
  std::atomic<uint64_t> next_req_id_{1};
  ThreadPool pool_;  // last: its destructor joins tasks that read shards_
};

}  // namespace kretrieve
