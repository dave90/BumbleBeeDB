//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// b_plus_tree.cpp
//
// Identification: src/storage/index/b_plus_tree.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "storage/index/b_plus_tree.h"

#include <utility>

#include "common/macros.h"
#include "storage/table/rid.h"

namespace bumblebee {

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager *buffer_pool_manager,
                          const KeyComparator &comparator, int leaf_max_size, int internal_max_size)
    : bpm_(buffer_pool_manager),
      index_name_(std::move(name)),
      comparator_(comparator),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id) {
  WritePageGuard guard = bpm_->WritePage(header_page_id_);
  auto root_page = guard.AsMut<BPlusTreeHeaderPage>();
  root_page->root_page_id_ = INVALID_PAGE_ID;
}

INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(OpenExisting, std::string name, page_id_t header_page_id,
                          BufferPoolManager *buffer_pool_manager, const KeyComparator &comparator,
                          int leaf_max_size, int internal_max_size)
    : bpm_(buffer_pool_manager),
      index_name_(std::move(name)),
      comparator_(comparator),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id) {
  // Opening an existing tree: the header page already holds the real root on disk — do NOT touch it.
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const -> bool {
  ReadPageGuard hguard = bpm_->ReadPage(header_page_id_);
  auto header_page = hguard.As<BPlusTreeHeaderPage>();
  if (header_page->root_page_id_ == INVALID_PAGE_ID) {
    return true;
  }
  ReadPageGuard rguard = bpm_->ReadPage(header_page->root_page_id_);
  return rguard.As<BPlusTreePage>()->GetSize() == 0;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType &key, std::vector<ValueType> *result) -> bool {
  ReadPageGuard guard = bpm_->ReadPage(header_page_id_);
  auto page_id = guard.As<BPlusTreeHeaderPage>()->root_page_id_;
  if (page_id == INVALID_PAGE_ID) {
    return false;
  }
  Context ctx;
  ctx.read_set_.push_back(std::move(guard));
  guard = bpm_->ReadPage(page_id);
  ctx.read_set_.pop_front();

  while (!guard.As<BPlusTreePage>()->IsLeafPage()) {
    auto internal = guard.As<InternalPage>();
    int idx = BPlusTreePage::BisectRight(*internal, key, comparator_, 1) - 1;
    ctx.read_set_.push_back(std::move(guard));
    guard = bpm_->ReadPage(internal->ValueAt(idx));
    ctx.read_set_.pop_back();
  }
  auto leaf = guard.As<LeafPage>();
  auto idx = BPlusTreePage::BisectRight(*leaf, key, comparator_) - 1;
  if (idx < 0 || comparator_(leaf->KeyAt(idx), key) != 0) {
    return false;
  }
  result->push_back(leaf->ValueAt(idx));
  return true;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::InitBPlusTree(const KeyType &key, const ValueType &value) -> page_id_t {
  auto leaf_page_id = bpm_->NewPage();
  WritePageGuard guard = bpm_->WritePage(leaf_page_id);
  auto leaf = guard.AsMut<LeafPage>();
  leaf->Init(leaf_max_size_);
  leaf->Insert(0, key, value);
  return leaf_page_id;
}

namespace {

template <typename PageT, typename KeyComparatorT, typename KeyTypeT, typename ValueTypeT>
void InsertAfterSplit(PageT &page1, PageT &page2, KeyComparatorT comparator, const KeyTypeT &key,
                      const ValueTypeT &value) {
  if (comparator(page2.KeyAt(0), key) < 0) {
    page2.Insert(comparator, key, value);
  } else {
    page1.Insert(comparator, key, value);
  }
}

template <typename PageT>
auto NewWritablePage(BufferPoolManager &bpm) -> std::pair<WritePageGuard, PageT *> {
  auto new_page_id = bpm.NewPage();
  auto new_wguard = bpm.WritePage(new_page_id);
  auto new_page = new_wguard.template AsMut<PageT>();
  return {std::move(new_wguard), new_page};
}

}  // namespace

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::RecursiveInsertLeaf(Context &ctx, const KeyType &key, const ValueType &value)
    -> InsertInternalResult {
  BUMBLEBEE_ASSERT(!ctx.write_set_.empty(), "write set empty in RecursiveInsertLeaf");
  auto &guard = ctx.write_set_.back();
  auto leaf = guard.AsMut<LeafPage>();
  if (leaf->GetSize() < leaf->GetMaxSize()) {
    // The leaf can absorb an insert without splitting, so ancestors are safe to release.
    while (ctx.write_set_.size() > 1) {
      ctx.write_set_.pop_front();
    }
  }
  if (leaf->ContainsKey(comparator_, key) == KeyStatus::LIVE) {
    ctx.write_set_.clear();
    return {std::nullopt, std::nullopt, false};  // duplicate key
  }
  auto idx = BPlusTreePage::BisectRight(*leaf, key, comparator_);
  if (leaf->GetSize() < leaf->GetMaxSize()) {
    leaf->Insert(idx, key, value);
    ctx.write_set_.clear();
    return {std::nullopt, std::nullopt, true};
  }
  // Full leaf: split, then insert into whichever half the key belongs to.
  auto [new_wguard, new_page] = NewWritablePage<LeafPage>(*bpm_);
  new_page->Init(leaf_max_size_);
  leaf->Split(*new_page, new_wguard.GetPageId());
  InsertAfterSplit(*leaf, *new_page, comparator_, key, value);
  ctx.write_set_.pop_back();
  return {new_page->KeyAt(0), new_wguard.GetPageId(), true};
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::RecursiveInsert(Context &ctx, const KeyType &key, const ValueType &value)
    -> InsertInternalResult {
  if (ctx.write_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
    return RecursiveInsertLeaf(ctx, key, value);
  }

  auto &guard = ctx.write_set_.back();
  auto page_id = guard.GetPageId();
  auto internal = guard.AsMut<InternalPage>();
  if (internal->GetSize() < internal->GetMaxSize()) {
    while (ctx.write_set_.size() > 1) {
      ctx.write_set_.pop_front();
    }
  }
  int idx = BPlusTreePage::BisectRight(*internal, key, comparator_, 1) - 1;
  ctx.write_set_.push_back(bpm_->WritePage(internal->ValueAt(idx)));
  auto result = RecursiveInsert(ctx, key, value);
  if (result.new_page_id_ == std::nullopt) {
    return result;
  }
  BUMBLEBEE_ASSERT(!ctx.write_set_.empty() && ctx.write_set_.back().GetPageId() == page_id,
                   "guard chain corrupted during insert");
  BUMBLEBEE_ASSERT(result.inserted_, "new page created but insert reported false");
  auto internal_new_page = result.new_page_id_.value();
  auto internal_new_key = result.new_key_.value();
  if (internal->GetSize() < internal->GetMaxSize()) {
    internal->Insert(comparator_, internal_new_key, internal_new_page);
    ctx.write_set_.clear();
    return {std::nullopt, std::nullopt, true};
  }
  // The internal node is also full: split it and push the new separator up.
  auto new_page = NewWritablePage<InternalPage>(*bpm_);
  new_page.second->Init(internal_max_size_);
  internal->Split(*new_page.second, new_page.first.GetPageId());
  InsertAfterSplit(*internal, *new_page.second, comparator_, internal_new_key, internal_new_page);
  ctx.write_set_.pop_back();
  return {new_page.second->KeyAt(0), new_page.first.GetPageId(), true};
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::TryOptimisticInsert(const KeyType &key, const ValueType &value, bool &insert_result) -> bool {
  auto header_guard = bpm_->ReadPage(header_page_id_);
  auto root_page_id = header_guard.template As<BPlusTreeHeaderPage>()->root_page_id_;
  if (root_page_id == INVALID_PAGE_ID) {
    return false;  // empty tree: let the pessimistic path create the root
  }
  Context ctx;
  ctx.read_set_.push_back(std::move(header_guard));
  ctx.read_set_.push_back(bpm_->ReadPage(root_page_id));
  if (ctx.read_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
    ctx.read_set_.pop_back();
    ctx.write_set_.push_back(bpm_->WritePage(root_page_id));
  }
  ctx.read_set_.pop_front();  // drop the header read latch

  // Read-crab the internals, releasing each ancestor, until the leaf's write latch is held.
  while (ctx.write_set_.empty()) {
    auto internal = ctx.read_set_.back().template As<InternalPage>();
    int idx = BPlusTreePage::BisectRight(*internal, key, comparator_, 1) - 1;
    auto page_id = internal->ValueAt(idx);
    ctx.read_set_.push_back(bpm_->ReadPage(page_id));
    if (ctx.read_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
      ctx.read_set_.pop_back();
      ctx.write_set_.push_back(bpm_->WritePage(page_id));
    }
    ctx.read_set_.pop_front();
  }
  auto leaf = ctx.write_set_.back().AsMut<LeafPage>();
  if (leaf->GetSize() >= leaf->GetMaxSize()) {
    return false;  // unsafe: the leaf would split → retry pessimistically
  }
  insert_result = RecursiveInsertLeaf(ctx, key, value).inserted_;
  return true;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value) -> bool {
  bool opt_result = false;
  if (TryOptimisticInsert(key, value, opt_result)) {
    return opt_result;
  }

  auto header_guard = bpm_->WritePage(header_page_id_);
  auto root_page_id = header_guard.template As<BPlusTreeHeaderPage>()->root_page_id_;
  if (root_page_id == INVALID_PAGE_ID) {
    header_guard.template AsMut<BPlusTreeHeaderPage>()->root_page_id_ = InitBPlusTree(key, value);
    return true;
  }

  Context ctx;
  ctx.root_page_id_ = root_page_id;
  ctx.write_set_.push_back(std::move(header_guard));
  ctx.write_set_.push_back(bpm_->WritePage(root_page_id));
  auto result = RecursiveInsert(ctx, key, value);
  if (!result.inserted_) {
    return false;  // duplicate key
  }
  if (result.new_page_id_ == std::nullopt) {
    return true;
  }

  // The root split: build a new root pointing at the two halves.
  BUMBLEBEE_ASSERT(!ctx.write_set_.empty(), "header guard lost during root split");
  auto &guard = ctx.write_set_.front();
  auto [new_wguard, new_page] = NewWritablePage<InternalPage>(*bpm_);
  new_page->Init(internal_max_size_);
  new_page->SetValueAt(0, root_page_id);
  new_page->Insert(1, result.new_key_.value(), result.new_page_id_.value());
  new_page->ChangeSizeBy(1);
  guard.AsMut<BPlusTreeHeaderPage>()->root_page_id_ = new_wguard.GetPageId();
  return true;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetBestSiblingForMerge(InternalPage &parent_page, int pidx, int &sib_idx) -> WritePageGuard {
  BUMBLEBEE_ASSERT(parent_page.GetSize() > 1, "GetBestSiblingForMerge needs >= 2 children");
  if (pidx == 0) {
    sib_idx = 1;
    return bpm_->WritePage(parent_page.ValueAt(1));
  }
  if (pidx >= parent_page.GetSize() - 1) {
    sib_idx = pidx - 1;
    return bpm_->WritePage(parent_page.ValueAt(pidx - 1));
  }
  auto right = bpm_->WritePage(parent_page.ValueAt(pidx + 1));
  auto left = bpm_->WritePage(parent_page.ValueAt(pidx - 1));
  if (right.template As<BPlusTreePage>()->GetSize() > left.template As<BPlusTreePage>()->GetSize()) {
    sib_idx = pidx + 1;
    return right;
  }
  sib_idx = pidx - 1;
  return left;
}

namespace {

// Try to borrow one entry from `sib` into `page`; on failure, merge the two into the left node.
template <typename PageT, typename InternalNode, typename KeyComparatorT>
auto StealOrMergeNodes(KeyComparatorT comparator, PageT *page, page_id_t page_id, InternalNode *parent, PageT *sib,
                       page_id_t sib_id, int pidx, bool sib_left) -> page_id_t {
  int sep_idx = sib_left ? pidx : pidx + 1;
  auto parent_sep = parent->KeyAt(sep_idx);
  decltype(parent_sep) new_sep{};
  if (page->StealValues(comparator, *sib, sib_left, parent_sep, new_sep)) {
    parent->SetKeyAt(sep_idx, new_sep);
    return INVALID_PAGE_ID;
  }
  if (sib_left) {
    sib->InsertAllValues(comparator, *page, parent_sep);
    parent->Remove(sep_idx);
    return page_id;
  }
  page->InsertAllValues(comparator, *sib, parent_sep);
  parent->Remove(sep_idx);
  return sib_id;
}

}  // namespace

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::RecursiveDeleteLeaf(Context &ctx, const KeyType &key) -> bool {
  BUMBLEBEE_ASSERT(!ctx.write_set_.empty(), "write set empty in RecursiveDeleteLeaf");
  auto guard = std::move(ctx.write_set_.back());
  ctx.write_set_.pop_back();
  auto leaf = guard.AsMut<LeafPage>();

  auto kidx = BPlusTreePage::BisectRight(*leaf, key, comparator_) - 1;
  if (leaf->ContainsKey(comparator_, key, kidx) != KeyStatus::LIVE) {
    ctx.write_set_.clear();
    ctx.header_page_ = std::nullopt;
    return false;
  }
  leaf->Remove(comparator_, key, kidx);
  if (leaf->GetSize() >= leaf->GetMinSize() || ctx.write_set_.empty() || guard.GetPageId() == ctx.root_page_id_) {
    ctx.write_set_.clear();
    if (guard.GetPageId() != ctx.root_page_id_) {
      ctx.header_page_ = std::nullopt;
    }
    return true;
  }
  // Underflow: fix using the parent (top of write_set_).
  auto parent = ctx.write_set_.back().AsMut<InternalPage>();
  int pidx = BPlusTreePage::BisectRight(*parent, key, comparator_, 1) - 1;
  if (parent->GetSize() <= 1) {
    if (leaf->GetSize() == 0) {
      auto dead = guard.GetPageId();
      parent->Remove(pidx);
      guard.Drop();
      bpm_->DeletePage(dead);
    }
    return true;
  }
  int sib_idx;
  auto sib_guard = GetBestSiblingForMerge(*parent, pidx, sib_idx);
  auto sib = sib_guard.template AsMut<LeafPage>();
  auto dead = StealOrMergeNodes(comparator_, leaf, guard.GetPageId(), parent, sib, sib_guard.GetPageId(), pidx,
                                sib_idx < pidx);
  if (dead != INVALID_PAGE_ID) {
    if (dead == guard.GetPageId()) {
      guard.Drop();
    } else {
      sib_guard.Drop();
    }
    bpm_->DeletePage(dead);
  }
  return true;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::RecursiveDelete(Context &ctx, const KeyType &key) -> bool {
  if (ctx.write_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
    return RecursiveDeleteLeaf(ctx, key);
  }

  auto cur = ctx.write_set_.back().AsMut<InternalPage>();
  if (cur->GetSize() > cur->GetMinSize()) {
    while (ctx.write_set_.size() > 1) {
      ctx.write_set_.pop_front();
    }
    if (ctx.write_set_.back().GetPageId() != ctx.root_page_id_) {
      ctx.header_page_ = std::nullopt;
    }
  }
  int idx = BPlusTreePage::BisectRight(*cur, key, comparator_, 1) - 1;
  ctx.write_set_.push_back(bpm_->WritePage(cur->ValueAt(idx)));
  auto result = RecursiveDelete(ctx, key);
  if (ctx.write_set_.empty()) {
    return result;
  }

  auto guard = std::move(ctx.write_set_.back());
  ctx.write_set_.pop_back();
  auto internal = guard.AsMut<InternalPage>();
  if (internal->GetSize() >= internal->GetMinSize() || ctx.write_set_.empty() ||
      guard.GetPageId() == ctx.root_page_id_) {
    ctx.write_set_.clear();
    if (guard.GetPageId() != ctx.root_page_id_) {
      ctx.header_page_ = std::nullopt;
    }
    return result;
  }
  auto parent_page = ctx.write_set_.back().AsMut<InternalPage>();
  int pidx = BPlusTreePage::BisectRight(*parent_page, key, comparator_, 1) - 1;
  if (internal->GetSize() == 0) {
    auto dead = guard.GetPageId();
    parent_page->Remove(pidx);
    guard.Drop();
    bpm_->DeletePage(dead);
    return result;
  }
  if (parent_page->GetSize() <= 1) {
    return result;
  }
  int sib_idx;
  auto sib_guard = GetBestSiblingForMerge(*parent_page, pidx, sib_idx);
  auto sib = sib_guard.template AsMut<InternalPage>();
  auto dead_page_id = StealOrMergeNodes(comparator_, internal, guard.GetPageId(), parent_page, sib,
                                        sib_guard.GetPageId(), pidx, sib_idx < pidx);
  if (dead_page_id != INVALID_PAGE_ID) {
    if (dead_page_id == guard.GetPageId()) {
      guard.Drop();
    } else {
      sib_guard.Drop();
    }
    bpm_->DeletePage(dead_page_id);
  }
  return result;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::TryOptimisticDelete(const KeyType &key) -> bool {
  auto header_guard = bpm_->ReadPage(header_page_id_);
  auto root_page_id = header_guard.template As<BPlusTreeHeaderPage>()->root_page_id_;
  if (root_page_id == INVALID_PAGE_ID) {
    return true;  // empty tree: nothing to delete, handled
  }
  Context ctx;
  ctx.read_set_.push_back(std::move(header_guard));
  ctx.read_set_.push_back(bpm_->ReadPage(root_page_id));
  if (ctx.read_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
    ctx.read_set_.pop_back();
    ctx.write_set_.push_back(bpm_->WritePage(root_page_id));
  }
  ctx.read_set_.pop_front();

  while (ctx.write_set_.empty()) {
    auto internal = ctx.read_set_.back().template As<InternalPage>();
    int idx = BPlusTreePage::BisectRight(*internal, key, comparator_, 1) - 1;
    auto page_id = internal->ValueAt(idx);
    ctx.read_set_.push_back(bpm_->ReadPage(page_id));
    if (ctx.read_set_.back().As<BPlusTreePage>()->IsLeafPage()) {
      ctx.read_set_.pop_back();
      ctx.write_set_.push_back(bpm_->WritePage(page_id));
    }
    ctx.read_set_.pop_front();
  }
  auto &wguard = ctx.write_set_.back();
  bool leaf_is_root = wguard.GetPageId() == root_page_id;
  auto leaf = wguard.AsMut<LeafPage>();
  // Unsafe if a non-root leaf could underflow (merge/redistribute), or if a leaf-root delete could
  // empty it — the latter needs the pessimistic path's root-collapse.
  if (leaf_is_root ? (leaf->GetSize() <= 1) : (leaf->GetSize() <= leaf->GetMinSize())) {
    return false;
  }
  return RecursiveDeleteLeaf(ctx, key);
}

INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key) {
  if (TryOptimisticDelete(key)) {
    return;
  }

  Context ctx;
  ctx.header_page_ = bpm_->WritePage(header_page_id_);
  auto root_page_id = ctx.header_page_->As<BPlusTreeHeaderPage>()->root_page_id_;
  if (root_page_id == INVALID_PAGE_ID) {
    return;
  }
  ctx.root_page_id_ = root_page_id;
  ctx.write_set_.push_back(bpm_->WritePage(root_page_id));
  RecursiveDelete(ctx, key);
  ctx.write_set_.clear();
  if (ctx.header_page_ == std::nullopt) {
    return;
  }

  // Root collapse (header still held): unwind degenerate single-child internals and an empty root.
  auto hp = ctx.header_page_->AsMut<BPlusTreeHeaderPage>();
  auto rid = hp->root_page_id_;
  while (rid != INVALID_PAGE_ID) {
    auto guard = bpm_->WritePage(rid);
    auto page = guard.As<BPlusTreePage>();
    if (!page->IsLeafPage() && page->GetSize() == 1) {
      auto child = guard.As<InternalPage>()->ValueAt(0);
      hp->root_page_id_ = child;
      guard.Drop();
      bpm_->DeletePage(rid);
      rid = child;
    } else if (page->IsLeafPage() && page->GetSize() == 0) {
      hp->root_page_id_ = INVALID_PAGE_ID;
      guard.Drop();
      bpm_->DeletePage(rid);
      rid = INVALID_PAGE_ID;
    } else {
      break;
    }
  }
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin() -> IndexIteratorType {
  ReadPageGuard guard = bpm_->ReadPage(header_page_id_);
  auto page_id = guard.As<BPlusTreeHeaderPage>()->root_page_id_;
  if (page_id == INVALID_PAGE_ID) {
    return IndexIteratorType();
  }
  Context ctx;
  ctx.read_set_.push_back(std::move(guard));
  guard = bpm_->ReadPage(page_id);
  ctx.read_set_.pop_front();

  while (!guard.As<BPlusTreePage>()->IsLeafPage()) {
    auto internal = guard.As<InternalPage>();
    ctx.read_set_.push_back(std::move(guard));
    guard = bpm_->ReadPage(internal->ValueAt(0));
    ctx.read_set_.pop_back();
  }
  return IndexIteratorType(bpm_, std::move(guard));
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType &key) -> IndexIteratorType {
  ReadPageGuard guard = bpm_->ReadPage(header_page_id_);
  auto page_id = guard.As<BPlusTreeHeaderPage>()->root_page_id_;
  if (page_id == INVALID_PAGE_ID) {
    return IndexIteratorType();
  }
  Context ctx;
  ctx.read_set_.push_back(std::move(guard));
  guard = bpm_->ReadPage(page_id);
  ctx.read_set_.pop_front();

  while (!guard.As<BPlusTreePage>()->IsLeafPage()) {
    auto internal = guard.As<InternalPage>();
    ctx.read_set_.push_back(std::move(guard));
    auto idx = BPlusTreePage::BisectRight(*internal, key, comparator_, 1) - 1;
    guard = bpm_->ReadPage(internal->ValueAt(idx));
    ctx.read_set_.pop_back();
  }
  auto it = IndexIteratorType(bpm_, std::move(guard));
  while (!it.IsEnd() && comparator_((*it).first, key) < 0) {
    ++it;
  }
  return it;
}

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End() -> IndexIteratorType { return IndexIteratorType(); }

INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId() -> page_id_t {
  auto header_rguard = bpm_->ReadPage(header_page_id_);
  return header_rguard.template As<BPlusTreeHeaderPage>()->root_page_id_;
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;
template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;
template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;
template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;
template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bumblebee
