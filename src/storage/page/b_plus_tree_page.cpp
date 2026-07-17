//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree_page.cpp
//
// Identification: src/storage/page/b_plus_tree_page.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/page/b_plus_tree_page.h"

namespace bumblebee {

auto BPlusTreePage::IsLeafPage() const -> bool { return page_type_ == IndexPageType::LEAF_PAGE; }
void BPlusTreePage::SetPageType(IndexPageType page_type) { page_type_ = page_type; }

auto BPlusTreePage::GetSize() const -> int { return size_; }
void BPlusTreePage::SetSize(int size) { size_ = size; }
void BPlusTreePage::ChangeSizeBy(int amount) { size_ += amount; }

auto BPlusTreePage::GetMaxSize() const -> int { return max_size_; }
void BPlusTreePage::SetMaxSize(int size) { max_size_ = size; }

auto BPlusTreePage::GetMinSize() const -> int { return max_size_ / 2; }

}  // namespace bumblebee
