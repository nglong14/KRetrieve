// Open-loop load generator: sends queries on a fixed schedule and records latency.
#pragma once

#include "net.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace kretrieve {

struct LoadConfig {
  std::string host = "127.0.0.1";
  uint16_t port = 0;
  double qps = 0;
  std::chrono::microseconds duration{0};
  std::vector<std::string> queries;  // cycled in order
  uint32_t k = 10;
  uint64_t budget_us = 0;
  size_t connections = 32;  // sender threads, one connection each
};

constexpr int kTransportError = -1;  // Sample::status when no valid reply arrived

struct Sample {
  int64_t due_us = 0;            // scheduled send time, relative to the run start
  int64_t latency_us = 0;        // reply time minus *scheduled* time (no coordinated omission)
  int status = kTransportError;  // ResponseStatus value, or kTransportError
  uint32_t missing = 0;          // missing shard count
  uint32_t hits = 0;
};

// Request i is due at start + i/qps. Each sender takes the next index when it is free,
// so when all senders are busy a request goes out late and the delay counts against it.
std::vector<Sample> run_load(const LoadConfig& config);

// Nearest-rank percentile (q in (0, 1]) of an ascending list; 0 when empty.
int64_t percentile(std::span<const int64_t> sorted, double q);

void write_csv(std::ostream& out, std::span<const Sample> samples);
// One line: counts by outcome plus p50/p95/p99/max latency (ms) of Ok replies.
std::string summarize(std::span<const Sample> samples);

}  // namespace kretrieve
