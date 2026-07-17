//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// concurrency_test_util.h
//
// Identification: test/unit/include/concurrency_test_util.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

namespace bumblebee {

/**
 * @brief Spawn `num_threads` threads, each running `args...(thread_itr)`, then join them all.
 *
 * The worker callable receives its 0-based thread index as the last argument.
 */
template <typename... Args>
void LaunchParallelTest(uint64_t num_threads, Args &&...args) {
  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (uint64_t thread_itr = 0; thread_itr < num_threads; ++thread_itr) {
    threads.emplace_back(args..., thread_itr);
  }
  for (auto &t : threads) {
    t.join();
  }
}

}  // namespace bumblebee
