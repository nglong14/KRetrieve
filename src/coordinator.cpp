#include "coordinator.hpp"

#include "net.hpp"

#include <algorithm>
#include <charconv>
#include <exception>
#include <future>
#include <queue>
#include <stdexcept>
#include <utility>

namespace kretrieve {
namespace {

// One blocking round trip; nullopt on any failure (connect, I/O, wrong reply).
std::optional<QueryResponse> query_shard(const ShardAddress& shard, uint64_t req_id,
                                         const std::vector<uint8_t>& request) {
  try {
    const auto fd = connect_tcp(shard.host, shard.port);
    if (!send_all(fd.get(), request)) return std::nullopt;
    const auto reply = read_frame(fd.get());
    if (!reply || reply->type != MsgType::QueryResponse || reply->req_id != req_id) {
      return std::nullopt;
    }
    return decode_response(reply->body);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

Frame reply(uint64_t req_id, const QueryResponse& response) {
  return {MsgType::QueryResponse, req_id, encode_response(response)};
}

}  // namespace

std::vector<ShardAddress> parse_shard_list(std::string_view list) {
  std::vector<ShardAddress> shards;
  while (true) {
    const auto comma = list.find(',');
    const auto item = list.substr(0, comma);
    const auto colon = item.rfind(':');
    uint16_t port = 0;
    const char* end = item.data() + item.size();
    if (colon == 0 || colon == std::string_view::npos ||
        std::from_chars(item.data() + colon + 1, end, port).ptr != end || port == 0) {
      throw std::invalid_argument("bad shard address (want host:port): " + std::string(item));
    }
    shards.push_back({std::string(item.substr(0, colon)), port});
    if (comma == std::string_view::npos) return shards;
    list.remove_prefix(comma + 1);
  }
}

std::vector<SearchHit> merge_top_k(std::span<const std::vector<SearchHit>> shard_hits, size_t k) {
  // Min-heap of the best k so far: the top is the worst kept hit.
  std::priority_queue<SearchHit, std::vector<SearchHit>, decltype(&ranks_before)> heap(
      ranks_before);
  if (k == 0) return {};
  for (const auto& hits : shard_hits) {
    for (const auto& hit : hits) {
      if (heap.size() < k) {
        heap.push(hit);
      } else if (ranks_before(hit, heap.top())) {
        heap.pop();
        heap.push(hit);
      }
    }
  }
  std::vector<SearchHit> out(heap.size());
  for (auto it = out.rbegin(); it != out.rend(); ++it) {
    *it = heap.top();
    heap.pop();
  }
  return out;
}

Coordinator::Coordinator(std::vector<ShardAddress> shards, size_t pool_threads)
    : shards_(std::move(shards)), pool_(pool_threads) {
  if (shards_.empty()) throw std::invalid_argument("coordinator needs at least one shard");
}

std::optional<Frame> Coordinator::handle(const Frame& request) {
  if (request.type != MsgType::QueryRequest) return std::nullopt;
  const auto query = decode_request(request.body);
  if (!query) return reply(request.req_id, {ResponseStatus::BadRequest, {}});

  // Our own id on every shard request; replies carrying another id are rejected.
  const uint64_t req_id = next_req_id_.fetch_add(1, std::memory_order_relaxed);
  const auto shard_request = encode_frame({MsgType::QueryRequest, req_id, request.body});

  std::vector<std::future<std::optional<QueryResponse>>> pending;
  pending.reserve(shards_.size());
  for (const auto& shard : shards_) {
    pending.push_back(pool_.submit(
        [&shard, req_id, &shard_request] { return query_shard(shard, req_id, shard_request); }));
  }

  // Wait for every task before returning: they reference shard_request.
  std::vector<std::vector<SearchHit>> shard_hits;
  bool failed = false;
  for (auto& future : pending) {
    auto response = future.get();
    if (!response || response->status != ResponseStatus::Ok) {
      failed = true;
    } else {
      shard_hits.push_back(std::move(response->hits));
    }
  }
  if (failed) return reply(request.req_id, {ResponseStatus::InternalError, {}});
  return reply(request.req_id, {ResponseStatus::Ok, merge_top_k(shard_hits, query->k)});
}

}  // namespace kretrieve
