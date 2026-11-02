// Blocking POSIX TCP helpers shared by the shard server and its clients.
#pragma once

#include "protocol.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace kretrieve {

// Owns one file descriptor; closes it on destruction.
class Fd {
 public:
  Fd() = default;
  explicit Fd(int fd) : fd_(fd) {}
  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  ~Fd() { reset(); }

  int get() const { return fd_; }

 private:
  void reset();
  int fd_ = -1;
};

using Clock = std::chrono::steady_clock;
using Deadline = std::optional<Clock::time_point>;  // nullopt = wait forever

Fd listen_tcp(const std::string& host, uint16_t port);
// Throws on failure, including when the deadline passes first.
Fd connect_tcp(const std::string& host, uint16_t port, Deadline deadline = {});
uint16_t local_port(int fd);

bool send_all(int fd, std::span<const uint8_t> data);

// nullopt on EOF, I/O error, malformed frame, or deadline expiry.
std::optional<Frame> read_frame(int fd, Deadline deadline = {});

}  // namespace kretrieve
