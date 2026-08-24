//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// query_memory_manager.h
//
// Identification: src/include/main/query_memory_manager.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>

#include "common/config.h"
#include "main/resource_manager.h"

namespace bumblebee {

/**
 * @brief The per-query memory budget the out-of-core operators reserve against before they spill.
 *
 * One instance per query, on the `ClientContext`, shared by every task. A spillable sink reserves bytes
 * as it builds its in-memory working set; when a reservation would exceed the budget it flushes that set
 * to buffer-pool-backed spill pages (which the buffer pool evicts to disk under pressure) and releases
 * the reservation. Bounding the budget tiny forces many runs / partitions — how the on-disk paths are
 * exercised in tests.
 */
class QueryMemoryManager {
 public:
  ~QueryMemoryManager() {
    const auto outstanding = used_.exchange(0, std::memory_order_acq_rel);
    if (global_ != nullptr && outstanding > 0) {
      global_->Release(outstanding);
    }
  }

  QueryMemoryManager() = default;
  QueryMemoryManager(const QueryMemoryManager &) = delete;
  auto operator=(const QueryMemoryManager &) -> QueryMemoryManager & = delete;

  void SetBudget(idx_t budget) { budget_ = budget; }
  auto Budget() const -> idx_t { return budget_; }
  void SetGlobalManager(GlobalQueryMemoryManager *global) { global_ = global; }
  auto Used() const -> idx_t { return used_.load(std::memory_order_acquire); }

  /** @brief Reserve `bytes` if that keeps us within budget; returns false (reserving nothing) if not. */
  [[nodiscard]] auto TryReserve(idx_t bytes) -> bool {
    idx_t cur = used_.load(std::memory_order_relaxed);
    do {
      if (bytes > budget_ || cur > budget_ - bytes) {
        return false;
      }
    } while (!used_.compare_exchange_weak(cur, cur + bytes, std::memory_order_acq_rel,
                                          std::memory_order_relaxed));
    if (global_ != nullptr && !global_->TryReserve(bytes)) {
      used_.fetch_sub(bytes, std::memory_order_acq_rel);
      return false;
    }
    return true;
  }

  void Release(idx_t bytes) {
    used_.fetch_sub(bytes, std::memory_order_acq_rel);
    if (global_ != nullptr) {
      global_->Release(bytes);
    }
  }

 private:
  idx_t budget_{MAX_MEMORY};
  std::atomic<idx_t> used_{0};
  GlobalQueryMemoryManager *global_{nullptr};
};

}  // namespace bumblebee
