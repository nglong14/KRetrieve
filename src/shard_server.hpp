// Linux TCP server: one epoll I/O loop and a fixed pool of query workers.
#pragma once

#include "index.hpp"
#include "net.hpp"
#include "protocol.hpp"

#include <cstddef>
#include <functional>
#include <optional>

namespace kretrieve {

// Answer one request frame. nullopt means "protocol violation, close the
// connection" (only QueryRequest is accepted). A malformed body yields BadRequest.
std::optional<Frame> handle_request(const InvertedIndex& index, const Frame& request);

// Called concurrently from worker threads; same nullopt contract as handle_request.
using RequestHandler = std::function<std::optional<Frame>(const Frame&)>;

// Frame server used by both shards and the coordinator.
class ShardServer {
 public:
  // workers=0 selects min(connection cap, CPU count).
  ShardServer(RequestHandler handler, Fd listener, size_t max_connections, size_t workers = 0);
  // index must outlive the server.
  ShardServer(const InvertedIndex& index, Fd listener, size_t max_connections, size_t workers = 0);
  ShardServer(const ShardServer&) = delete;
  ShardServer& operator=(const ShardServer&) = delete;

  // Call once. Shutdown drops queued work, closes sockets, and joins active searches.
  void run();
  // Thread-safe and async-signal-safe: only writes one byte to a pipe.
  void stop();

 private:
  RequestHandler handler_;
  Fd listener_;
  Fd stop_read_;
  Fd stop_write_;
  size_t max_connections_;
  size_t worker_count_;
};

}  // namespace kretrieve
