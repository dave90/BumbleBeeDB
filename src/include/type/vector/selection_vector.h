//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// selection_vector.h
//
// Identification: src/include/type/vector/selection_vector.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>

#include "common/config.h"

namespace bumblebee {

using sel_ptr_t = std::shared_ptr<sel_t[]>;

/**
 * An indirection layer over the rows of a Vector: `sel[i]` is the position, in the
 * underlying data, of the i-th row of the selection.
 *
 * Filters and joins produce a SelectionVector instead of copying data, so a subset of a
 * Vector costs nothing but the indices. A default-constructed (null) SelectionVector is
 * the identity: GetIndex(i) == i.
 *
 * The indices are held in a shared_ptr, so copying a SelectionVector shares the buffer.
 *
 * NOTE: critical class — the hot-path accessors are inlined here.
 */
class SelectionVector {
 public:
  SelectionVector() = default;

  /** @brief Reference an externally owned index array. */
  SelectionVector(sel_t *sel) { Initialize(sel); }  // NOLINT(google-explicit-constructor)

  /** @brief Allocate an (uninitialized) index array of `count` entries. */
  SelectionVector(idx_t count) { Initialize(count); }  // NOLINT(google-explicit-constructor)

  /** @brief Build the selection [start, start + count). */
  SelectionVector(idx_t start, idx_t count) {
    Initialize(STANDARD_VECTOR_SIZE);
    for (idx_t i = 0; i < count; i++) {
      SetIndex(i, start + i);
    }
  }

  /** @brief Share the index array of `sel_vector`. */
  SelectionVector(const SelectionVector &sel_vector) { Initialize(sel_vector); }

  /** @brief Take a share of an existing index array. */
  SelectionVector(sel_ptr_t data) { Initialize(std::move(data)); }  // NOLINT(google-explicit-constructor)

  auto operator=(const SelectionVector &sel_vector) -> SelectionVector & {
    if (this != &sel_vector) {
      Initialize(sel_vector);
    }
    return *this;
  }

  /** @brief Point at an externally owned index array. Ownership is not taken. */
  void Initialize(sel_t *sel) {
    sel_vector_ = sel;
    sel_data_.reset();
  }

  /** @brief Allocate and own an (uninitialized) index array of `count` entries. */
  void Initialize(idx_t count) {
    sel_data_ = sel_ptr_t(new sel_t[count]);
    sel_vector_ = sel_data_.get();
  }

  /** @brief Take a share of an existing index array. */
  void Initialize(sel_ptr_t data) {
    sel_data_ = std::move(data);
    sel_vector_ = sel_data_.get();
  }

  /** @brief Share the index array of `other`. */
  void Initialize(const SelectionVector &other) {
    sel_data_ = other.sel_data_;
    sel_vector_ = other.sel_vector_;
  }

  /** @brief Make the i-th selected row point at position `loc`. */
  void SetIndex(idx_t idx, idx_t loc) { sel_vector_[idx] = static_cast<sel_t>(loc); }

  /** @brief Exchange the i-th and j-th entries. */
  void Swap(idx_t i, idx_t j) {
    sel_t tmp = sel_vector_[i];
    sel_vector_[i] = sel_vector_[j];
    sel_vector_[j] = tmp;
  }

  /** @return The raw index array, or nullptr for the identity selection. */
  auto GetData() -> sel_t * { return sel_vector_; }

  /** @return The raw index array, or nullptr for the identity selection. */
  auto GetData() const -> const sel_t * { return sel_vector_; }

  /** @return The owned index array, if this selection owns one. */
  auto GetSelData() const -> sel_ptr_t { return sel_data_; }

  /** @return The position of the i-th selected row. The identity if there is no array. */
  auto GetIndex(idx_t idx) const -> idx_t { return sel_vector_ != nullptr ? sel_vector_[idx] : idx; }

  /**
   * @brief Compose two selections: result[i] = this[sel[i]].
   *
   * @param sel The selection to apply on top of this one.
   * @param count The number of entries to produce.
   * @return sel_ptr_t A freshly allocated index array of `count` entries.
   */
  auto Slice(const SelectionVector &sel, idx_t count) const -> sel_ptr_t;

  /** @return A comma-separated rendering of the first `count` indices. */
  auto ToString(idx_t count = 0) const -> std::string;

  auto operator[](idx_t index) -> sel_t & { return sel_vector_[index]; }

 private:
  sel_t *sel_vector_{nullptr};
  sel_ptr_t sel_data_;
};

}  // namespace bumblebee
