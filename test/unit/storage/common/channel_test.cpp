//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// channel_test.cpp
//
// Identification: test/unit/storage/common/channel_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/common/channel.h"

#include <optional>
#include <thread>  // NOLINT
#include <vector>

#include "common/config.h"
#include "common/exception.h"
#include "gtest/gtest.h"

namespace bumblebee {

TEST(StorageConfigTest, PageConstants) {
  // The page size is a positive power of two, and the invalid sentinels are negative.
  EXPECT_EQ(PAGE_SIZE, 8192);
  EXPECT_GT(BUFFER_POOL_SIZE, 0);
  EXPECT_LT(INVALID_PAGE_ID, 0);
  EXPECT_LT(INVALID_FRAME_ID, 0);
  static_assert(sizeof(page_id_t) == 4, "page_id_t is 32-bit on disk");
}

TEST(StorageConfigTest, ExecutionExceptionCarriesType) {
  try {
    throw ExecutionException("boom");
  } catch (const Exception &e) {
    EXPECT_EQ(e.GetType(), ExceptionType::EXECUTION);
  }
}

TEST(ChannelTest, SingleProducerSingleConsumerFifo) {
  Channel<std::optional<int>> channel;
  std::vector<int> received;
  std::thread consumer([&]() {
    while (true) {
      auto item = channel.Get();
      if (!item.has_value()) {
        break;
      }
      received.push_back(*item);
    }
  });

  for (int i = 0; i < 100; i++) {
    channel.Put(i);
  }
  channel.Put(std::nullopt);  // stop sentinel
  consumer.join();

  ASSERT_EQ(received.size(), 100U);
  for (int i = 0; i < 100; i++) {
    EXPECT_EQ(received[i], i);
  }
}

}  // namespace bumblebee
