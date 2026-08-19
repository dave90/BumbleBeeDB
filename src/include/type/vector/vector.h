//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector.h
//
// Identification: src/include/type/vector/vector.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "common/macros.h"
#include "type/list_entry.h"
#include "type/logical_type.h"
#include "type/value.h"
#include "type/vector/selection_vector.h"
#include "type/vector/validity_mask.h"
#include "type/vector/vector_data_mngr.h"

namespace bumblebee {

/** How the rows of a Vector are physically encoded. */
enum class VectorType : uint8_t {
  /** A standard, uncompressed array of values. */
  FLAT_VECTOR,
  /** A single value, logically repeated for every row. */
  CONSTANT_VECTOR,
  /** A selection vector on top of another vector. */
  DICTIONARY_VECTOR,
  /** An arithmetic sequence: a start and an increment. */
  SEQUENCE_VECTOR,
  /**
   * A sequence over [start, end] that restarts once it runs past `end`, with each value
   * repeated `stride` times and the whole thing shifted by `offset`.
   */
  SEQUENCE_CIRCULAR_VECTOR
};

/** Caches the dictionary managers produced when slicing several vectors by one selection. */
struct SelCache {
  std::unordered_map<sel_t *, vector_data_mngr_ptr_t> cache_;
};

/**
 * A flat view of a Vector: the data, the selection to read it through, and the validity.
 *
 * This is what Vector::Orrify() produces — the uniform (data, sel, validity) triple every
 * kernel loops over, whatever the encoding of the Vector it came from.
 */
struct VectorData {
  /** The selection to read `data_` and `validity_` through. Never null after Orrify(). */
  const SelectionVector *sel_{nullptr};
  /** The underlying flat/constant data. */
  data_ptr_t data_{nullptr};
  /** Backing storage when the selection had to be materialized (constant vectors). */
  SelectionVector owned_sel_;
  /** Validity of the underlying data, read through `sel_` exactly like `data_`. */
  const ValidityMask *validity_{nullptr};
};

/**
 * A column of values: the unit the vectorized engine computes on.
 *
 * A Vector does not own its memory — a VectorDataMngr does — which is what lets vectors
 * reference one another for free. The encoding (VectorType) says how the rows are laid
 * out: flat, a single repeated constant, a selection over another vector (dictionary), or
 * a generated sequence.
 *
 * NULLs live in a ValidityMask, never in a sentinel value.
 */
class Vector {
  friend struct ConstantVector;
  friend struct FlatVector;
  friend struct DictionaryVector;
  friend struct StringVector;
  friend struct SequenceVector;
  friend struct CircularSequenceVector;
  friend struct ListVector;
  friend struct ArrayVector;

 public:
  /** @brief Reference `other`: share its data, encoding and validity. */
  explicit Vector(Vector &other);

  /** @brief Reference `other` through a selection: a DICTIONARY_VECTOR over it. */
  explicit Vector(Vector &other, const SelectionVector &sel, idx_t count);

  /** @brief Reference `other` starting from row `offset`. */
  explicit Vector(Vector &other, idx_t offset);

  /** @brief A CONSTANT_VECTOR holding `value`. Keeps the value's full LogicalType. */
  explicit Vector(const Value &value);

  /** @brief A FLAT_VECTOR of `capacity` rows of `type`, with the data allocated. */
  explicit Vector(LogicalType type, idx_t capacity = STANDARD_VECTOR_SIZE);

  /** @brief A FLAT_VECTOR of `type` over memory owned by someone else. */
  Vector(LogicalType type, data_ptr_t dataptr);

  /**
   * @brief A FLAT_VECTOR of `type`, optionally allocating (and zeroing) its data.
   *
   * @param type The type of the values.
   * @param create_data Whether to allocate the data.
   * @param zero_data Whether to zero the allocation.
   * @param capacity The number of rows.
   */
  Vector(LogicalType type, bool create_data, bool zero_data, idx_t capacity = STANDARD_VECTOR_SIZE);

  Vector(Vector &&other) noexcept;
  Vector(const Vector &) = delete;
  auto operator=(const Vector &) -> Vector & = delete;
  auto operator=(Vector &&) -> Vector & = delete;
  ~Vector() = default;

  // -- Referencing ----------------------------------------------------------

  /** @brief Become a CONSTANT_VECTOR over `value`. */
  void Reference(const Value &value);

  /** @brief Share the data, encoding and validity of `other`. */
  void Reference(const Vector &other);

  /** @brief Share the data of `other`, taking its type as well. */
  void Reinterpret(const Vector &other);

  /** @brief Take the type of `other` and then reference it. */
  void ReferenceAndSetType(Vector &other);

  // -- Slicing --------------------------------------------------------------

  /** @brief Reference `other` from row `offset` on. */
  void Slice(Vector &other, idx_t offset);

  /** @brief Reference `other` through `sel`: this becomes a DICTIONARY_VECTOR. */
  void Slice(Vector &other, const SelectionVector &sel, idx_t count);

  /** @brief Apply `sel` to this vector, composing with an existing selection. */
  void Slice(const SelectionVector &sel, idx_t count);

  /** @brief Slice, reusing the dictionary manager `cache` already holds for `sel`. */
  void Slice(const SelectionVector &sel, idx_t count, SelCache &cache);

  /** @brief Exchange the contents of the two vectors. */
  void Swap(Vector &other);

  // -- Initialization -------------------------------------------------------

  /** @brief Allocate the data of a flat vector of `capacity` rows. */
  void Initialize(bool zero_data, idx_t capacity);
  void Initialize(idx_t capacity) { Initialize(false, capacity); }
  void Initialize(bool zero_data) { Initialize(zero_data, STANDARD_VECTOR_SIZE); }

  /**
   * @brief Back to an empty flat vector over `cache_mngr`'s standard-size buffer.
   *
   * Reuses the buffer only when the cache is its sole remaining owner (checked after this
   * vector drops its own references); otherwise the buffer stays alive under whoever still
   * references it and `cache_mngr` is replaced with a fresh allocation. Keeps the current
   * logical type. Not for LIST/ARRAY vectors — their child buffers cannot be reused in place.
   */
  void ResetFromCache(vector_data_mngr_ptr_t &cache_mngr);

  /** @return A rendering of the first `count` rows. */
  auto ToString(idx_t count) const -> std::string;

  /** @return A rendering of the vector's type and encoding. */
  auto ToString() const -> std::string;

  // -- Encoding conversions -------------------------------------------------

  /** @brief Turn this into a FLAT_VECTOR, materializing whatever encoding it had. */
  void Normalify(idx_t count);

  /** @brief Materialize only the rows named by `sel`. The other rows are left untouched. */
  void Normalify(const SelectionVector &sel, idx_t count);

  /**
   * @brief Produce the (data, selection, validity) triple the kernels read this vector through.
   *
   * @param count The number of rows the caller will read.
   * @param data The triple to fill in.
   */
  void Orrify(idx_t count, VectorData &data);

  /** @brief Turn this into a SEQUENCE_VECTOR: start, start + increment, ... */
  void Sequence(int64_t start, int64_t increment);

  /**
   * @brief Turn this into a SEQUENCE_CIRCULAR_VECTOR.
   *
   * Row i holds `start + ((i + offset) / stride) % (end - start + 1)`: the values of
   * [start, end], each repeated `stride` times, wrapping around, shifted by `offset`.
   */
  void Sequence(int64_t start, int64_t offset, int64_t stride, int64_t end);

  // -- Value access ---------------------------------------------------------

  /** @return The value of row `index`, carrying this vector's LogicalType. */
  auto GetValue(idx_t index) const -> Value;

  /** @brief Write `val` into row `index`. A NULL clears the row's validity bit. */
  void SetValue(idx_t index, const Value &val);

  // -- Validity (NULL) handling ---------------------------------------------

  /**
   * @return The mask owning the per-row validity. For a DICTIONARY_VECTOR that is the
   *         child's mask, which is indexed through the selection — not by logical row.
   */
  auto Validity() -> ValidityMask &;
  auto Validity() const -> const ValidityMask &;

  /** @return True if the logical row `idx` is valid (not null), whatever the encoding. */
  auto RowIsValid(idx_t idx) const -> bool;

  /** @brief Mark the logical row `idx` valid, whatever the encoding. */
  void SetValid(idx_t idx);

  /** @brief Mark the logical row `idx` invalid (null), whatever the encoding. */
  void SetInvalid(idx_t idx);

  /** @brief Grow the data (and the validity mask) from `cur_size` to `new_size` rows. */
  void Resize(idx_t cur_size, idx_t new_size);

  /** @brief Force the encoding. The caller is responsible for the data matching it. */
  void SetVectorType(VectorType vector_type);

  // -- Accessors ------------------------------------------------------------

  /** @return The encoding of this vector. */
  auto GetVectorType() const -> VectorType { return vtype_; }

  /** @return The physical type of the values. */
  auto GetType() const -> PhysicalType { return type_.GetPhysicalType(); }

  /** @return The logical type id of the values. */
  auto GetLogicalTypeId() const -> LogicalTypeId { return type_.GetTypeId(); }

  /** @return The full logical type of the values. */
  auto GetLogicalType() const -> LogicalType { return type_; }

  /** @return The raw data. Meaningless for the sequence encodings. */
  auto GetData() -> data_ptr_t { return data_; }

  /** @return The manager owning the data. */
  auto GetDataMngr() -> vector_data_mngr_ptr_t { return data_mngr_; }

  /** @return The data manager as a raw pointer (identity checks without refcount traffic). */
  auto DataMngrPtr() const -> const VectorDataMngr * { return data_mngr_.get(); }

  /** @brief Replace the auxiliary manager. */
  void SetAuxiliary(vector_data_mngr_ptr_t new_buffer) { aux_data_mngr_ = std::move(new_buffer); }

 private:
  /**
   * @brief Create the auxiliary manager a nested type needs: the child Vector.
   *
   * A LIST gets a ListDataMngr (a child of STANDARD_VECTOR_SIZE elements, grown on
   * demand); an ARRAY gets an ArrayDataMngr (a child of exactly `capacity * array_size`
   * elements, since an ARRAY row is a fixed slice of the child). A scalar type gets none.
   *
   * @param capacity The number of ROWS the vector holds.
   */
  void InitializeAuxiliary(idx_t capacity);

  /** @brief Write the defensive physical NULL fill into row `index`. */
  void WriteNullFill(idx_t index);

  /**
   * @brief Re-type a Value read out of the raw data with this vector's LogicalType.
   *
   * The raw read yields a Value of the *physical* type (a BOOLEAN row reads back as a
   * UTINYINT); this puts the logical identity back on it so GetValue/SetValue round-trip.
   */
  auto WrapValue(Value raw) const -> Value;

  /** @brief Throw if the type is one the vector layer does not support yet. */
  void CheckSupportedType() const;

 protected:
  /** The encoding. */
  VectorType vtype_{VectorType::FLAT_VECTOR};
  /** The type of the values. */
  LogicalType type_;
  /** The data. Owned by data_mngr_, not by the Vector. */
  data_ptr_t data_{nullptr};
  /** The owner of the data. */
  vector_data_mngr_ptr_t data_mngr_;
  /** The string heap (STRING vectors) or the child vector (DICTIONARY vectors). */
  vector_data_mngr_ptr_t aux_data_mngr_;
  /**
   * Per-row validity, all-valid by default. For a DICTIONARY_VECTOR the authoritative
   * mask lives on the child and this member is unused.
   */
  ValidityMask validity_;
};

/** The data manager holding the child (the real data) of a dictionary vector. */
class VectorChildDataMngr : public VectorDataMngr {
 public:
  explicit VectorChildDataMngr(Vector vector)
      : VectorDataMngr(VectorDataMngrType::VECTOR_CHILD_BUFFER), data_(std::move(vector)) {}

  /** The vector the dictionary's selection reads through. */
  Vector data_;
};

/**
 * The data manager of a LIST Vector: the child Vector holding the elements.
 *
 * The Vector's own data is the array of ListEntry — one (offset, length) per row — and
 * every one of those offsets indexes into `child_`. `size_` is the number of child slots
 * actually in use (the high-water mark of everything appended so far); `capacity_` is how
 * many the child can hold before it has to grow.
 */
class ListDataMngr : public VectorDataMngr {
 public:
  ListDataMngr(Vector child, idx_t capacity)
      : VectorDataMngr(VectorDataMngrType::LIST_BUFFER), child_(std::move(child)), capacity_(capacity) {}

  /** @return The Vector holding the elements of every row. */
  auto GetChild() -> Vector & { return child_; }
  auto GetChild() const -> const Vector & { return child_; }

  /** @return The number of child slots in use. */
  auto GetSize() const -> idx_t { return size_; }

  /** @brief Declare how many child slots are in use. */
  void SetSize(idx_t size) { size_ = size; }

  /** @return The number of child slots allocated. */
  auto GetCapacity() const -> idx_t { return capacity_; }

  /** @brief Grow the child so that it can hold at least `required` elements. */
  void Reserve(idx_t required);

 private:
  Vector child_;
  idx_t size_{0};
  idx_t capacity_;
};

/**
 * The data manager of an ARRAY Vector: the child Vector holding the elements.
 *
 * An ARRAY is fixed width, so it needs no entries at all: row i is the child slice
 * [i * array_size_, (i + 1) * array_size_). The Vector's own data is therefore empty.
 */
class ArrayDataMngr : public VectorDataMngr {
 public:
  ArrayDataMngr(Vector child, idx_t array_size)
      : VectorDataMngr(VectorDataMngrType::ARRAY_BUFFER), child_(std::move(child)), array_size_(array_size) {}

  /** @return The Vector holding the elements of every row, `array_size_` per row. */
  auto GetChild() -> Vector & { return child_; }
  auto GetChild() const -> const Vector & { return child_; }

  /** @return The fixed number of elements of every row. */
  auto GetArraySize() const -> idx_t { return array_size_; }

 private:
  Vector child_;
  idx_t array_size_;
};

// ---------------------------------------------------------------------------
// The accessor structs: the typed views of a Vector's data, per encoding.
//
// NOTE: critical classes — the hot-path accessors are inlined here.
// ---------------------------------------------------------------------------

/** Accessors for a CONSTANT_VECTOR: a single value, logically repeated. */
struct ConstantVector {
  static auto GetData(const Vector &vector) -> const_data_ptr_t {
    BUMBLEBEE_ASSERT(
        vector.GetVectorType() == VectorType::CONSTANT_VECTOR || vector.GetVectorType() == VectorType::FLAT_VECTOR,
        "ConstantVector::GetData on a non constant/flat vector");
    return vector.data_;
  }

  static auto GetData(Vector &vector) -> data_ptr_t {
    BUMBLEBEE_ASSERT(
        vector.GetVectorType() == VectorType::CONSTANT_VECTOR || vector.GetVectorType() == VectorType::FLAT_VECTOR,
        "ConstantVector::GetData on a non constant/flat vector");
    return vector.data_;
  }

  template <class T>
  static auto GetData(const Vector &vector) -> const T * {
    return reinterpret_cast<const T *>(GetData(vector));
  }

  template <class T>
  static auto GetData(Vector &vector) -> T * {
    return reinterpret_cast<T *>(GetData(vector));
  }

  /**
   * @brief A selection whose every entry is 0, so that reading through it always hits row 0.
   *
   * @param count The number of rows the caller will read.
   * @param owned_sel Backing storage, used only when `count` exceeds STANDARD_VECTOR_SIZE.
   * @return const SelectionVector* The all-zero selection.
   */
  static auto ZeroSelectionVector(idx_t count, SelectionVector &owned_sel) -> const SelectionVector *;

  /** @brief Turn `vector` into a constant holding row `position` of `source`. */
  static void Reference(Vector &vector, Vector &source, idx_t position, idx_t count);

  /** @return True if the single value of the constant is NULL. */
  static auto IsNull(const Vector &vector) -> bool {
    BUMBLEBEE_ASSERT(vector.GetVectorType() == VectorType::CONSTANT_VECTOR,
                     "ConstantVector::IsNull on a non constant vector");
    return !vector.validity_.RowIsValid(0);
  }

  /** @brief Set the nullness of the single value of the constant. */
  static void SetNull(Vector &vector, bool is_null) {
    BUMBLEBEE_ASSERT(vector.GetVectorType() == VectorType::CONSTANT_VECTOR,
                     "ConstantVector::SetNull on a non constant vector");
    vector.validity_.Set(0, !is_null);
  }

  static const sel_t ZERO_VECTOR[STANDARD_VECTOR_SIZE];
  static const SelectionVector ZERO_SELECTION_VECTOR;
};

/** Accessors for a DICTIONARY_VECTOR: a selection over a child vector. */
struct DictionaryVector {
  static auto SelVector(const Vector &vector) -> const SelectionVector & {
    BUMBLEBEE_ASSERT(vector.GetVectorType() == VectorType::DICTIONARY_VECTOR,
                     "DictionaryVector::SelVector on a non dictionary vector");
    return static_cast<const DictionaryDataMngr &>(*vector.data_mngr_).GetSelection();
  }

  static auto SelVector(Vector &vector) -> SelectionVector & {
    BUMBLEBEE_ASSERT(vector.GetVectorType() == VectorType::DICTIONARY_VECTOR,
                     "DictionaryVector::SelVector on a non dictionary vector");
    return static_cast<DictionaryDataMngr &>(*vector.data_mngr_).GetSelection();
  }

  static auto Child(const Vector &vector) -> const Vector & {
    BUMBLEBEE_ASSERT(vector.GetVectorType() == VectorType::DICTIONARY_VECTOR,
                     "DictionaryVector::Child on a non dictionary vector");
    return static_cast<const VectorChildDataMngr &>(*vector.aux_data_mngr_).data_;
  }

  static auto Child(Vector &vector) -> Vector & {
    BUMBLEBEE_ASSERT(vector.GetVectorType() == VectorType::DICTIONARY_VECTOR,
                     "DictionaryVector::Child on a non dictionary vector");
    return static_cast<VectorChildDataMngr &>(*vector.aux_data_mngr_).data_;
  }
};

/** Accessors for a FLAT_VECTOR: the plain array of values. */
struct FlatVector {
  static auto GetData(Vector &vector) -> data_ptr_t { return ConstantVector::GetData(vector); }

  template <class T>
  static auto GetData(const Vector &vector) -> const T * {
    return ConstantVector::GetData<T>(vector);
  }

  template <class T>
  static auto GetData(Vector &vector) -> T * {
    return ConstantVector::GetData<T>(vector);
  }

  static void SetData(Vector &vector, data_ptr_t data) {
    BUMBLEBEE_ASSERT(vector.GetVectorType() == VectorType::FLAT_VECTOR, "FlatVector::SetData on a non flat vector");
    vector.data_ = data;
  }

  template <class T>
  static auto GetValue(Vector &vector, idx_t idx) -> T {
    BUMBLEBEE_ASSERT(vector.GetVectorType() == VectorType::FLAT_VECTOR, "FlatVector::GetValue on a non flat vector");
    return FlatVector::GetData<T>(vector)[idx];
  }

  static auto Validity(Vector &vector) -> ValidityMask & { return vector.validity_; }
  static auto Validity(const Vector &vector) -> const ValidityMask & { return vector.validity_; }

  /** The materialized identity 0..STANDARD_VECTOR_SIZE-1, for callers that need a real `sel_t` array. */
  static const std::array<sel_t, STANDARD_VECTOR_SIZE> INCREMENTAL_VECTOR;
  /** The identity selection: GetIndex(i) == i, backed by no array at all. */
  static const SelectionVector INCREMENTAL_SELECTION_VECTOR;
};

/** The string heap of a STRING Vector. */
struct StringVector {
  /** @brief Copy `len` bytes of `data` into the vector's heap. */
  static auto AddString(Vector &vector, const char *data, idx_t len) -> string_t;

  /** @brief Copy the NUL-terminated `data` into the vector's heap. */
  static auto AddString(Vector &vector, const char *data) -> string_t;

  /** @brief Copy the bytes of `data` into the vector's heap. */
  static auto AddString(Vector &vector, string_t data) -> string_t;

  /** @brief Copy `data` into the vector's heap. */
  static auto AddString(Vector &vector, const std::string &data) -> string_t;

  /** @brief Reserve `len` bytes in the vector's heap, for the caller to write into. */
  static auto EmptyString(Vector &vector, idx_t len) -> string_t;

  /** @brief Keep `buffer` alive for as long as this vector lives. */
  static void AddBuffer(Vector &vector, vector_data_mngr_ptr_t buffer);

  /** @brief Keep the heap of `other` alive for as long as this vector lives. */
  static void AddHeapReference(Vector &vector, Vector &other);
};

/**
 * Accessors for a LIST Vector: the entries, and the child holding the elements.
 *
 * Every accessor resolves a DICTIONARY_VECTOR down to the list underneath it, so slicing
 * a list vector keeps working: the dictionary owns the sliced list as its child, and the
 * list's own manager (and therefore its elements) survives inside it. The indices the
 * accessors then take and return are the indices of THAT underlying list, i.e. the ones a
 * dictionary's selection maps to.
 */
struct ListVector {
  /** @return The Vector holding the elements of every row. */
  static auto GetChild(Vector &vector) -> Vector &;
  static auto GetChild(const Vector &vector) -> const Vector &;

  /** @return The number of elements currently held by the child. */
  static auto GetListSize(const Vector &vector) -> idx_t;

  /** @brief Declare how many elements the child holds. */
  static void SetListSize(Vector &vector, idx_t size);

  /** @brief Grow the child so that it can hold at least `required` elements. */
  static void Reserve(Vector &vector, idx_t required);

  /**
   * @brief Append rows [source_offset, source_count) of `source` to the child.
   *
   * The elements are COPIED, so the target's child owns them: the offsets the caller
   * writes into its entries stay valid however the source is later reused or destroyed.
   *
   * @param vector The LIST vector whose child is appended to.
   * @param source The vector to read the elements from. Its type is the LIST's child type.
   * @param source_count The END of the source range (not a length), as in VectorOperations::Copy.
   * @param source_offset The first source row to append.
   * @return idx_t The child index the appended elements start at.
   */
  static auto Append(Vector &vector, const Vector &source, idx_t source_count, idx_t source_offset = 0) -> idx_t;

  /** @return The per-row (offset, length) entries. */
  static auto GetEntries(Vector &vector) -> ListEntry *;
  static auto GetEntries(const Vector &vector) -> const ListEntry *;

 private:
  /** @return The manager owning the list's child, looking through any dictionary. */
  static auto Mngr(Vector &vector) -> ListDataMngr &;
  static auto Mngr(const Vector &vector) -> const ListDataMngr &;
};

/** Accessors for an ARRAY Vector: row i is the child slice [i * size, (i + 1) * size). */
struct ArrayVector {
  /** @return The Vector holding the elements of every row, `GetArraySize()` per row. */
  static auto GetChild(Vector &vector) -> Vector &;
  static auto GetChild(const Vector &vector) -> const Vector &;

  /** @return The fixed number of elements of every row. */
  static auto GetArraySize(const Vector &vector) -> idx_t;

 private:
  /** @return The manager owning the array's child, looking through any dictionary. */
  static auto Mngr(Vector &vector) -> ArrayDataMngr &;
  static auto Mngr(const Vector &vector) -> const ArrayDataMngr &;
};

/** Accessors for a SEQUENCE_VECTOR. */
struct SequenceVector {
  /** @brief Read back the start and the increment of the sequence. */
  static void GetSequence(const Vector &vector, int64_t &start, int64_t &increment);
};

/** Accessors for a SEQUENCE_CIRCULAR_VECTOR. */
struct CircularSequenceVector {
  /**
   * @brief Read back the parameters of the circular sequence.
   *
   * `stride` is how often a value repeats (the product of the lengths of every set before
   * this column in a cartesian product); `offset` shifts the whole sequence.
   */
  static void GetSequence(const Vector &vector, int64_t &start, int64_t &offset, int64_t &stride, int64_t &end);
};

using array_vector_data_t = std::unique_ptr<VectorData[]>;

}  // namespace bumblebee
