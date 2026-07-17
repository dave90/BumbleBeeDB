//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree_header_page.h
//
// Identification: src/include/storage/page/b_plus_tree_header_page.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "common/config.h"

namespace bumblebee {

/**
 * @brief The header page holds only the root page id, giving root changes a single latch point.
 */
class BPlusTreeHeaderPage {
 public:
  BPlusTreeHeaderPage() = delete;
  BPlusTreeHeaderPage(const BPlusTreeHeaderPage &other) = delete;

  page_id_t root_page_id_;
};

}  // namespace bumblebee
