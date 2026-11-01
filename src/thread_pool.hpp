// Fixed-size worker pool for coordinator fan-out.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace kretrieve {

class ThreadPool {
 public:
  explicit ThreadPool(size_t threads) {
    if (threads == 0) throw std::invalid_argument("thread pool needs at least one thread");
    workers_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { work(); });
  }
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Runs every queued task, then joins.
  ~ThreadPool() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_) worker.join();
  }

  template <typename F>
  std::future<std::invoke_result_t<F>> submit(F task) {
    // shared_ptr because std::function requires a copyable callable.
    auto packaged =
        std::make_shared<std::packaged_task<std::invoke_result_t<F>()>>(std::move(task));
    auto future = packaged->get_future();
    {
      std::lock_guard lock(mutex_);
      tasks_.emplace([packaged] { (*packaged)(); });
    }
    ready_.notify_one();
    return future;
  }

 private:
  void work() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty()) return;  // stopping and drained
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      task();  // packaged_task stores exceptions in the future
    }
  }

  std::mutex mutex_;
  std::condition_variable ready_;
  bool stopping_ = false;
  std::queue<std::function<void()>> tasks_;
  std::vector<std::thread> workers_;
};

}  // namespace kretrieve
