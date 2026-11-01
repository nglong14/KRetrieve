// Scatter-gather coordinator: fan a query out to every shard and merge top-K.
#pragma once

#include "index.hpp"
#include "protocol.hpp"
#include "thread_pool.hpp"

#include <atomic>
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
  Coordinator(std::vector<ShardAddress> shards, size_t pool_threads);

  // Thread-safe. Same contract as handle_request: nullopt closes the client connection.
  // Any shard failure yields InternalError (day 4 turns this into partial results).
  std::optional<Frame> handle(const Frame& request);

 private:
  std::vector<ShardAddress> shards_;
  std::atomic<uint64_t> next_req_id_{1};
  ThreadPool pool_;  // last: its destructor joins tasks that read shards_
};

}  // namespace kretrieve
