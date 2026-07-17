//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// channel.h
//
// Identification: src/include/storage/common/channel.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <condition_variable>  // NOLINT
#include <mutex>               // NOLINT
#include <queue>
#include <utility>

namespace bumblebee {

/**
 * @brief A thread-safe, multi-producer multi-consumer FIFO queue.
 *
 * Used by the disk scheduler to hand requests to its background worker thread.
 */
template <class T>
class Channel {
 public:
  Channel() = default;
  ~Channel() = default;

  /**
   * @brief Insert an element into the queue and wake a waiting consumer.
   *
   * @param element The element to enqueue.
   */
  void Put(T element) {
    std::unique_lock<std::mutex> lk(m_);
    q_.push(std::move(element));
    lk.unlock();
    cv_.notify_all();
  }

  /**
   * @brief Remove and return the front element, blocking until one is available.
   *
   * @return T The dequeued element.
   */
  auto Get() -> T {
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [&]() { return !q_.empty(); });
    T element = std::move(q_.front());
    q_.pop();
    return element;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::queue<T> q_;
};

}  // namespace bumblebee
