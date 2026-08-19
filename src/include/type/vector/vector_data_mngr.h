//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_data_mngr.h
//
// Identification: src/include/type/vector/vector_data_mngr.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <vector>

#include "common/config.h"
#include "type/bumble_string.h"
#include "type/logical_type.h"
#include "type/string_heap.h"
#include "type/vector/selection_vector.h"

namespace bumblebee {

/** What a VectorDataMngr holds on behalf of its Vector. */
enum class VectorDataMngrType : uint8_t {
  /** A single flat array of values. */
  STANDARD_DATA_MNGR,
  /** A selection vector over another Vector's data. */
  DICTIONARY_DATA_MNGR,
  /** The string heap backing a STRING Vector. */
  STRING_BUFFER,
  /** A child Vector (for the nested types). */
  VECTOR_CHILD_BUFFER,
  /** The child Vector holding the elements of a LIST Vector, plus how many are in use. */
  LIST_BUFFER,
  /** The child Vector holding the elements of an ARRAY Vector, plus the array size. */
  ARRAY_BUFFER
};

/**
 * The owner of the memory a Vector reads and writes.
 *
 * Separating the storage from the Vector is what lets vectors reference each other: two
 * Vectors can share one data manager, and slicing a Vector only swaps in a
 * DictionaryDataMngr holding a SelectionVector instead of copying any data.
 *
 * NOTE: critical class — the accessors are inlined here.
 */
class VectorDataMngr {
 public:
  using vector_data_mngr_ptr_t = std::shared_ptr<VectorDataMngr>;
  using vector_vdm_ptr_t = std::vector<vector_data_mngr_ptr_t>;

  explicit VectorDataMngr(VectorDataMngrType type) : type_(type) {}
  explicit VectorDataMngr(std::unique_ptr<data_t[]> data)
      : type_(VectorDataMngrType::STANDARD_DATA_MNGR), data_(std::move(data)) {}
  explicit VectorDataMngr(idx_t size)
      : type_(VectorDataMngrType::STANDARD_DATA_MNGR), data_(std::make_unique_for_overwrite<data_t[]>(size)) {}
  virtual ~VectorDataMngr() = default;

  /** @return What this manager holds. */
  auto GetType() const -> VectorDataMngrType { return type_; }

  /** @return The raw data array, or nullptr if this manager holds no flat data. */
  auto GetData() -> data_ptr_t { return data_.get(); }

  /** @brief Replace the raw data array. */
  void SetData(std::unique_ptr<data_t[]> data) { data_ = std::move(data); }

  // -- Factories ------------------------------------------------------------

  /**
   * @brief Allocate the flat data of a Vector of `capacity` rows of `type`.
   *
   * @param type The physical type of the values.
   * @param capacity The number of rows.
   * @return vector_data_mngr_ptr_t The data manager owning the allocation.
   */
  static auto CreateStandardVector(PhysicalType type, idx_t capacity = STANDARD_VECTOR_SIZE) -> vector_data_mngr_ptr_t {
    return vector_data_mngr_ptr_t(new VectorDataMngr(capacity * LogicalType::SizeOf(type)));
  }

  /**
   * @brief Allocate the flat data of a constant Vector: room for a single value.
   *
   * @param type The physical type of the value.
   * @return vector_data_mngr_ptr_t The data manager owning the allocation.
   */
  static auto CreateConstantVector(PhysicalType type) -> vector_data_mngr_ptr_t {
    return vector_data_mngr_ptr_t(new VectorDataMngr(LogicalType::SizeOf(type)));
  }

 protected:
  VectorDataMngrType type_;
  std::unique_ptr<data_t[]> data_;
};

using vector_data_mngr_ptr_t = VectorDataMngr::vector_data_mngr_ptr_t;

/** The data manager of a dictionary Vector: a selection over some other Vector's data. */
class DictionaryDataMngr : public VectorDataMngr {
 public:
  explicit DictionaryDataMngr(const SelectionVector &sel)
      : VectorDataMngr(VectorDataMngrType::DICTIONARY_DATA_MNGR), sel_(sel) {}
  explicit DictionaryDataMngr(sel_ptr_t sel)
      : VectorDataMngr(VectorDataMngrType::DICTIONARY_DATA_MNGR), sel_(std::move(sel)) {}
  explicit DictionaryDataMngr(idx_t size) : VectorDataMngr(VectorDataMngrType::DICTIONARY_DATA_MNGR), sel_(size) {}

  /** @return The selection the Vector reads its rows through. */
  auto GetSelection() const -> const SelectionVector & { return sel_; }

  /** @return The selection the Vector reads its rows through. */
  auto GetSelection() -> SelectionVector & { return sel_; }

  /** @brief Point the Vector at a different selection. */
  void SetSelection(const SelectionVector &sel) { sel_.Initialize(sel); }

 private:
  SelectionVector sel_;
};

/** The data manager of a STRING Vector: the heap that owns the referenced bytes. */
class StringDataMngr : public VectorDataMngr {
 public:
  StringDataMngr() : VectorDataMngr(VectorDataMngrType::STRING_BUFFER) {}

  /** @brief Copy `len` bytes of `data` into the heap. */
  auto AddString(const char *data, idx_t len) -> string_t { return heap_.AddString(data, len); }

  /** @brief Copy the bytes of `data` into the heap. */
  auto AddString(string_t data) -> string_t { return heap_.AddString(data); }

  /** @brief Copy the raw bytes of `data` into the heap, terminator excluded. */
  auto AddBlob(string_t data) -> string_t { return heap_.AddBlob(data.GetDataWriteable(), data.Size()); }

  /**
   * @brief Keep another data manager (and its heap) alive for as long as this one lives.
   *
   * Used when a Vector's strings reference bytes owned by another Vector's heap — e.g.
   * when two string vectors are merged.
   */
  void AddHeapReference(vector_data_mngr_ptr_t heap) { references_.push_back(std::move(heap)); }

  /** @brief Reserve room for a string of `len` bytes, to be written by the caller. */
  auto AddEmptyString(idx_t len) -> string_t { return heap_.AddEmptyString(len); }

 private:
  StringHeap heap_;
  /** Heaps owned elsewhere that this Vector's strings point into. */
  vector_vdm_ptr_t references_;
};

}  // namespace bumblebee
