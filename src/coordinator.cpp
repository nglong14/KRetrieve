#include "coordinator.hpp"

#include "net.hpp"

#include <poll.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <exception>
#include <future>
#include <queue>
#include <stdexcept>
#include <utility>

namespace kretrieve {
namespace {

// Connect and send `query` with budget_us = time left, so the shard can drop work we
// stopped waiting for. An empty Fd means it failed.
Fd send_query(const ShardAddress& shard, uint64_t req_id, QueryRequest query,
              Clock::time_point deadline) {
  const auto left = std::chrono::ceil<std::chrono::microseconds>(deadline - Clock::now());
  if (left.count() <= 0) return {};
  query.budget_us = static_cast<uint64_t>(left.count());
  try {
    auto fd = connect_tcp(shard.host, shard.port, deadline);
    const auto frame = encode_frame({MsgType::QueryRequest, req_id, encode_request(query)});
    if (send_all(fd.get(), frame, deadline)) return fd;
  } catch (const std::exception&) {
  }
  return {};
}

// nullopt unless the next frame is an Ok QueryResponse for req_id.
std::optional<QueryResponse> read_ok_reply(int fd, uint64_t req_id, Clock::time_point deadline) {
  try {
    const auto reply = read_frame(fd, deadline);
    if (!reply || reply->type != MsgType::QueryResponse || reply->req_id != req_id) {
      return std::nullopt;
    }
    auto response = decode_response(reply->body);
    if (!response || response->status != ResponseStatus::Ok) return std::nullopt;
    return response;
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

Coordinator::Coordinator(std::vector<ShardAddress> shards, size_t pool_threads,
                         std::vector<ShardAddress> replicas, std::chrono::microseconds hedge_after)
    : shards_(std::move(shards)),
      replicas_(std::move(replicas)),
      hedge_after_(hedge_after),
      pool_(pool_threads) {
  if (shards_.empty()) throw std::invalid_argument("coordinator needs at least one shard");
  if (!replicas_.empty() && replicas_.size() != shards_.size()) {
    throw std::invalid_argument("--replicas must have one address per --shards entry");
  }
  if (hedge_after_.count() > 0 && replicas_.empty()) {
    throw std::invalid_argument("--hedge-ms needs --replicas");
  }
}

std::optional<QueryResponse> Coordinator::ask_shard(size_t i, uint64_t req_id,
                                                    const QueryRequest& query,
                                                    Clock::time_point deadline) const {
  // ponytail: fixed hedge delay and no cap on hedge rate. Use an adaptive p95 or a hedge
  // budget if hedges under overload start to slow the replicas.
  const bool hedging = hedge_after_.count() > 0;
  const ShardAddress* replica = replicas_.empty() ? nullptr : &replicas_[i];
  std::vector<Fd> live;  // copies still waiting for a reply; destroying one cancels it
  live.push_back(send_query(shards_[i], req_id, query, deadline));
  const auto hedge_at = Clock::now() + hedge_after_;
  std::optional<Clock::time_point> failed_at;  // set when the replica replaces a failed primary

  auto done = [&](std::optional<QueryResponse> response) {
    if (failed_at) {
      const auto us =
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - *failed_at);
      std::fprintf(stderr, "failover shard=%zu result=%s failover_ms=%.3f\n", i,
                   response ? "success" : "failure", static_cast<double>(us.count()) / 1000.0);
    }
    return response;
  };

  while (true) {
    std::erase_if(live, [](const Fd& fd) { return fd.get() < 0; });
    if (Clock::now() >= deadline) return done(std::nullopt);
    // Ask the replica once: right away if the primary failed, or when the primary is slow.
    if (replica && (live.empty() || (hedging && Clock::now() >= hedge_at))) {
      if (live.empty()) failed_at = Clock::now();
      live.push_back(send_query(*replica, req_id, query, deadline));
      replica = nullptr;
      continue;
    }
    if (live.empty()) return done(std::nullopt);

    std::array<pollfd, 2> fds{};
    for (size_t k = 0; k < live.size(); ++k) fds[k] = {live[k].get(), POLLIN, 0};
    const auto wake = replica && hedging ? std::min(hedge_at, deadline) : deadline;
    const int ready = poll(fds.data(), live.size(), remaining_ms(wake));
    if (ready < 0 && errno != EINTR) return done(std::nullopt);
    for (size_t k = 0; ready > 0 && k < live.size(); ++k) {
      if (fds[k].revents == 0) continue;
      // ponytail: reads the whole frame from the ready socket, so a half-sent reply holds up
      // the other copy until the deadline. Fine for small loopback replies; assemble frames
      // non-blocking if shards move off-host.
      if (auto response = read_ok_reply(live[k].get(), req_id, deadline)) {
        return done(std::move(response));
      }
      live[k] = Fd();  // bad reply or closed: this copy is out
    }
  }
}

std::optional<Frame> Coordinator::handle(const Frame& request, Clock::time_point received) {
  using std::chrono::microseconds;
  if (request.type != MsgType::QueryRequest) return std::nullopt;
  auto query = decode_request(request.body);
  if (!query) return reply(request.req_id, {ResponseStatus::BadRequest, {}, {}});

  const auto budget = query->budget_us == 0
                          ? kDefaultBudget
                          : microseconds(std::min<uint64_t>(query->budget_us, kMaxBudget.count()));
  const auto deadline = received + budget;

  // No budget left means the request waited it out in our queue: shed it.
  if (Clock::now() >= deadline) return reply(request.req_id, {ResponseStatus::Overloaded, {}, {}});

  // Our own id on every shard request; replies carrying another id are rejected.
  const uint64_t req_id = next_req_id_.fetch_add(1, std::memory_order_relaxed);

  std::vector<std::future<std::optional<QueryResponse>>> pending;
  pending.reserve(shards_.size());
  for (size_t i = 0; i < shards_.size(); ++i) {
    pending.push_back(pool_.submit(
        [this, i, req_id, &query, deadline] { return ask_shard(i, req_id, *query, deadline); }));
  }

  // Every task returns by the deadline, so waiting on all of them is bounded.
  // It is also required: the tasks reference query.
  std::vector<std::vector<SearchHit>> shard_hits;
  QueryResponse out;
  for (size_t i = 0; i < pending.size(); ++i) {
    auto response = pending[i].get();
    if (!response) {
      out.missing_shards.push_back(static_cast<uint32_t>(i));
    } else {
      shard_hits.push_back(std::move(response->hits));
    }
  }
  out.hits = merge_top_k(shard_hits, query->k);
  return reply(request.req_id, out);
}

}  // namespace kretrieve
