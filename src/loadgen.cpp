#include "loadgen.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <ostream>
#include <stdexcept>
#include <thread>

namespace kretrieve {
namespace {

using std::chrono::microseconds;

constexpr auto kReplyTimeout = std::chrono::seconds(5);

// One request on fd (connecting first if needed). Leaves fd closed after a failure.
void send_one(const LoadConfig& config, Fd& fd, uint64_t req_id, const std::string& query,
              Sample& sample) {
  try {
    const auto deadline = Clock::now() + kReplyTimeout;
    if (fd.get() < 0) fd = connect_tcp(config.host, config.port, deadline);
    const Frame frame{MsgType::QueryRequest, req_id,
                      encode_request({config.k, config.budget_us, query})};
    if (send_all(fd.get(), encode_frame(frame), deadline)) {
      const auto reply = read_frame(fd.get(), deadline);
      if (reply && reply->type == MsgType::QueryResponse && reply->req_id == req_id) {
        if (const auto response = decode_response(reply->body)) {
          sample.status = static_cast<int>(response->status);
          sample.missing = static_cast<uint32_t>(response->missing_shards.size());
          sample.hits = static_cast<uint32_t>(response->hits.size());
        }
      }
    }
  } catch (const std::exception&) {
    // Connect failure: recorded as a transport error below.
  }
  if (sample.status == kTransportError) fd = Fd();
}

}  // namespace

std::vector<Sample> run_load(const LoadConfig& config) {
  if (!(config.qps > 0) || config.duration.count() <= 0 || config.queries.empty() ||
      config.connections == 0) {
    throw std::invalid_argument("loadgen needs qps > 0, duration > 0, queries, connections > 0");
  }
  const double seconds = std::chrono::duration<double>(config.duration).count();
  const auto total = static_cast<size_t>(std::max(1.0, std::round(config.qps * seconds)));
  std::vector<Sample> samples(total);
  std::atomic<size_t> next{0};
  // A short lead time so request 0 is not already late while threads start.
  const auto start = Clock::now() + std::chrono::milliseconds(50);

  auto sender = [&] {
    Fd fd;
    for (size_t i = next.fetch_add(1); i < total; i = next.fetch_add(1)) {
      const auto due =
          start + std::chrono::duration_cast<Clock::duration>(
                      std::chrono::duration<double>(static_cast<double>(i) / config.qps));
      std::this_thread::sleep_until(due);
      auto& sample = samples[i];
      sample.due_us = std::chrono::duration_cast<microseconds>(due - start).count();
      send_one(config, fd, i + 1, config.queries[i % config.queries.size()], sample);
      sample.latency_us = std::chrono::duration_cast<microseconds>(Clock::now() - due).count();
    }
  };
  std::vector<std::thread> threads;
  threads.reserve(config.connections);
  for (size_t t = 0; t < config.connections; ++t) threads.emplace_back(sender);
  for (auto& thread : threads) thread.join();
  return samples;
}

int64_t percentile(std::span<const int64_t> sorted, double q) {
  if (sorted.empty()) return 0;
  const auto rank = static_cast<size_t>(std::ceil(q * static_cast<double>(sorted.size())));
  return sorted[std::clamp<size_t>(rank, 1, sorted.size()) - 1];
}

void write_csv(std::ostream& out, std::span<const Sample> samples) {
  out << "seq,due_us,latency_us,status,missing,hits\n";
  for (size_t i = 0; i < samples.size(); ++i) {
    const auto& s = samples[i];
    out << i << ',' << s.due_us << ',' << s.latency_us << ',' << s.status << ',' << s.missing << ','
        << s.hits << '\n';
  }
}

std::string summarize(std::span<const Sample> samples) {
  size_t ok = 0, incomplete = 0, overloaded = 0, errors = 0;
  std::vector<int64_t> latencies;
  latencies.reserve(samples.size());
  for (const auto& s : samples) {
    if (s.status == static_cast<int>(ResponseStatus::Ok)) {
      ++ok;
      latencies.push_back(s.latency_us);  // shed replies are fast and would hide the tail
      if (s.missing != 0) ++incomplete;
    } else if (s.status == static_cast<int>(ResponseStatus::Overloaded)) {
      ++overloaded;
    } else {
      ++errors;
    }
  }
  std::sort(latencies.begin(), latencies.end());
  const auto ms = [&](double q) { return static_cast<double>(percentile(latencies, q)) / 1000; };
  char line[256];
  std::snprintf(line, sizeof line,
                "sent=%zu ok=%zu incomplete=%zu overloaded=%zu errors=%zu "
                "ok p50=%.2fms p95=%.2fms p99=%.2fms max=%.2fms",
                samples.size(), ok, incomplete, overloaded, errors, ms(0.50), ms(0.95), ms(0.99),
                ms(1.0));
  return line;
}

}  // namespace kretrieve
