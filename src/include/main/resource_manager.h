//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// resource_manager.h
//
// Identification: src/include/main/resource_manager.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>  // NOLINT
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>  // NOLINT

#include "common/config.h"

namespace bumblebee {

/**
 * @brief Fair database-wide admission for native query workers.
 *
 * Every query first acquires one blocking admission slot. Once its pipeline DAG is initialized it may
 * non-blockingly borrow more currently idle slots for intra-query workers. Waiting queries always take
 * priority over these additional grants, and all tokens are move-only so every exit path returns the
 * complete grant to the database-wide budget.
 */
class WorkerSlotManager {
 public:
  class Token {
   public:
    Token() = default;
    Token(WorkerSlotManager *owner, idx_t slots) : owner_(owner), slots_(slots) {}
    ~Token() { Reset(); }

    Token(const Token &) = delete;
    auto operator=(const Token &) -> Token & = delete;

    Token(Token &&other) noexcept : owner_(other.owner_), slots_(other.slots_) {
      other.owner_ = nullptr;
      other.slots_ = 0;
    }
    auto operator=(Token &&other) noexcept -> Token & {
      if (this != &other) {
        Reset();
        owner_ = other.owner_;
        slots_ = other.slots_;
        other.owner_ = nullptr;
        other.slots_ = 0;
      }
      return *this;
    }

    auto Slots() const -> idx_t { return slots_; }
    explicit operator bool() const { return owner_ != nullptr; }

   private:
    void Reset() {
      if (owner_ != nullptr) {
        owner_->Release(slots_);
        owner_ = nullptr;
        slots_ = 0;
      }
    }

    WorkerSlotManager *owner_{nullptr};
    idx_t slots_{0};
  };

  explicit WorkerSlotManager(idx_t capacity) : capacity_(std::max<idx_t>(1, capacity)) {}

  /** @brief Wait in FIFO order for one database-wide execution slot. */
  auto Acquire() -> Token {
    std::unique_lock lock(mutex_);
    const uint64_t ticket = next_ticket_++;
    waiters_.push_back(ticket);
    cv_.wait(lock, [&] { return !waiters_.empty() && waiters_.front() == ticket && used_ < capacity_; });
    waiters_.pop_front();
    used_++;
    peak_ = std::max(peak_, used_);
    cv_.notify_all();
    return Token(this, 1);
  }

  /**
   * @brief Borrow up to `requested` additional slots without waiting.
   *
   * A queued base-slot waiter has priority, so a running query cannot enlarge its worker pool while a
   * new query is already waiting for admission.
   */
  auto TryAcquire(idx_t requested) -> Token {
    if (requested == 0) {
      return {};
    }
    std::lock_guard lock(mutex_);
    if (!waiters_.empty() || used_ >= capacity_) {
      return {};
    }
    const idx_t granted = std::min(requested, capacity_ - used_);
    used_ += granted;
    peak_ = std::max(peak_, used_);
    return Token(this, granted);
  }

  auto Capacity() const -> idx_t { return capacity_; }
  auto Used() const -> idx_t {
    std::lock_guard lock(mutex_);
    return used_;
  }
  auto Peak() const -> idx_t {
    std::lock_guard lock(mutex_);
    return peak_;
  }

 private:
  void Release(idx_t slots) {
    std::lock_guard lock(mutex_);
    used_ -= slots;
    cv_.notify_all();
  }

  const idx_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<uint64_t> waiters_;
  uint64_t next_ticket_{0};
  idx_t used_{0};
  idx_t peak_{0};
};

/** @brief Atomic aggregate ceiling for all query-local memory reservations in one database. */
class GlobalQueryMemoryManager {
 public:
  explicit GlobalQueryMemoryManager(idx_t budget) : budget_(budget) {}

  [[nodiscard]] auto TryReserve(idx_t bytes) -> bool {
    idx_t current = used_.load(std::memory_order_relaxed);
    do {
      if (bytes > budget_ || current > budget_ - bytes) {
        return false;
      }
    } while (
        !used_.compare_exchange_weak(current, current + bytes, std::memory_order_acq_rel, std::memory_order_relaxed));
    UpdatePeak(current + bytes);
    return true;
  }

  void Release(idx_t bytes) { used_.fetch_sub(bytes, std::memory_order_acq_rel); }

  auto Budget() const -> idx_t { return budget_; }
  auto Used() const -> idx_t { return used_.load(std::memory_order_acquire); }
  auto Peak() const -> idx_t { return peak_.load(std::memory_order_acquire); }

 private:
  void UpdatePeak(idx_t value) {
    idx_t peak = peak_.load(std::memory_order_relaxed);
    while (value > peak && !peak_.compare_exchange_weak(peak, value, std::memory_order_relaxed)) {
    }
  }

  const idx_t budget_;
  std::atomic<idx_t> used_{0};
  std::atomic<idx_t> peak_{0};
};

}  // namespace bumblebee
