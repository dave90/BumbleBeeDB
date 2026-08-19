//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// list_vector_test.cpp
//
// Identification: test/unit/type/vector/list_vector_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <string>
#include <vector>

#include "common/exception.h"
#include "gtest/gtest.h"
#include "type/list_entry.h"
#include "type/logical_type.h"
#include "type/value.h"
#include "type/vector/data_chunk.h"
#include "type/vector/operations/create_sort_key.h"
#include "type/vector/vector.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

/** @return The type `INTEGER[]`. */
static auto IntListType() -> LogicalType { return LogicalType::List(LogicalTypeId::INTEGER); }

/** @return The type `INTEGER[n]`. */
static auto IntArrayType(idx_t n) -> LogicalType { return LogicalType::Array(LogicalTypeId::INTEGER, n); }

/** @return An `INTEGER[]` value holding `elements`. A nullopt element becomes a NULL element. */
static auto IntList(const std::vector<int32_t> &elements) -> Value {
  std::vector<Value> children;
  children.reserve(elements.size());
  for (auto e : elements) {
    children.emplace_back(e);
  }
  return Value::List(IntListType(), std::move(children));
}

/** @return An `INTEGER[n]` value holding `elements`. */
static auto IntArray(const std::vector<int32_t> &elements) -> Value {
  std::vector<Value> children;
  children.reserve(elements.size());
  for (auto e : elements) {
    children.emplace_back(e);
  }
  return Value::List(IntArrayType(elements.size()), std::move(children));
}

/** @return An `INTEGER[]` value whose elements are the given values verbatim (NULLs allowed). */
static auto ListOf(std::vector<Value> children) -> Value { return Value::List(IntListType(), std::move(children)); }

/** @return A NULL INTEGER element, as it appears INSIDE a list. */
static auto NullElement() -> Value { return Value::Null(LogicalTypeId::INTEGER); }

// ---------------------------------------------------------------------------
// The type system's view of a nested type
// ---------------------------------------------------------------------------

TEST(ListVectorTest, ListSizeOfIsTheEntry) {
  // A LIST row stores exactly one (offset, length) pair inline.
  EXPECT_EQ(LogicalType::SizeOf(PhysicalType::LIST), sizeof(ListEntry));
  // An ARRAY row stores nothing inline: its elements are a fixed slice of the child.
  EXPECT_EQ(LogicalType::SizeOf(PhysicalType::ARRAY), 0U);
}

TEST(ListVectorTest, NestedTypesAreNotConstantSize) {
  // IsConstantSize governs whether a column can be inlined in a ROW — a different question
  // from whether a Vector can stride over it. Both nested types must stay false.
  EXPECT_FALSE(LogicalType::IsConstantSize(PhysicalType::LIST));
  EXPECT_FALSE(LogicalType::IsConstantSize(PhysicalType::ARRAY));
  EXPECT_FALSE(IntListType().IsConstantSize());
  EXPECT_FALSE(IntArrayType(3).IsConstantSize());
}

// ---------------------------------------------------------------------------
// GetValue / SetValue round trip
// ---------------------------------------------------------------------------

TEST(ListVectorTest, ListRoundTrip) {
  Vector vec(IntListType(), 4);
  ASSERT_EQ(vec.GetType(), PhysicalType::LIST);

  vec.SetValue(0, IntList({1, 2, 3}));
  vec.SetValue(1, IntList({}));
  vec.SetValue(2, IntList({7}));

  EXPECT_EQ(vec.GetValue(0), IntList({1, 2, 3}));
  EXPECT_EQ(vec.GetValue(1), IntList({}));
  EXPECT_EQ(vec.GetValue(2), IntList({7}));

  // Every element written landed in the one child.
  EXPECT_EQ(ListVector::GetListSize(vec), 4U);
  const auto *entries = ListVector::GetEntries(vec);
  EXPECT_EQ(entries[0].offset_, 0U);
  EXPECT_EQ(entries[0].length_, 3U);
  EXPECT_EQ(entries[1].length_, 0U);
  EXPECT_EQ(entries[2].offset_, 3U);
  EXPECT_EQ(entries[2].length_, 1U);
}

TEST(ListVectorTest, ArrayRoundTrip) {
  Vector vec(IntArrayType(3), 4);
  ASSERT_EQ(vec.GetType(), PhysicalType::ARRAY);
  EXPECT_EQ(ArrayVector::GetArraySize(vec), 3U);

  vec.SetValue(0, IntArray({1, 2, 3}));
  vec.SetValue(1, IntArray({4, 5, 6}));

  EXPECT_EQ(vec.GetValue(0), IntArray({1, 2, 3}));
  EXPECT_EQ(vec.GetValue(1), IntArray({4, 5, 6}));

  // Row i IS the child slice [i * 3, (i + 1) * 3): no entries, no indirection.
  const auto &child = ArrayVector::GetChild(vec);
  EXPECT_EQ(child.GetValue(3), Value(4));
  EXPECT_EQ(child.GetValue(5), Value(6));
}

TEST(ListVectorTest, ArrayRejectsTheWrongLength) {
  Vector vec(IntArrayType(3), 2);
  EXPECT_THROW(vec.SetValue(0, IntArray({1, 2})), Exception);
}

TEST(ListVectorTest, ConstantListVector) {
  Vector vec(IntList({4, 5}));
  EXPECT_EQ(vec.GetVectorType(), VectorType::CONSTANT_VECTOR);
  EXPECT_EQ(vec.GetValue(0), IntList({4, 5}));
  // A constant is the same value at every row.
  EXPECT_EQ(vec.GetValue(7), IntList({4, 5}));
}

TEST(ListVectorTest, ToStringRendersTheElements) {
  Vector vec(IntListType(), 2);
  vec.SetValue(0, IntList({1, 2, 3}));
  const auto rendered = vec.ToString(1);
  EXPECT_NE(rendered.find("[1, 2, 3]"), std::string::npos) << rendered;
  // The type renders as INTEGER[] too.
  EXPECT_NE(rendered.find("INTEGER[]"), std::string::npos) << rendered;

  Vector arr(IntArrayType(2), 2);
  arr.SetValue(0, IntArray({8, 9}));
  const auto arr_rendered = arr.ToString(1);
  EXPECT_NE(arr_rendered.find("[8, 9]"), std::string::npos) << arr_rendered;
}

// ---------------------------------------------------------------------------
// Nulls: a NULL element, a NULL list and an EMPTY list are three different things
// ---------------------------------------------------------------------------

TEST(ListVectorTest, NullElementVsNullListVsEmptyList) {
  Vector vec(IntListType(), 4);
  // Row 0: a list holding one NULL element. The ROW is not null.
  vec.SetValue(0, ListOf({NullElement()}));
  // Row 1: the whole list is NULL.
  vec.SetValue(1, Value::Null(IntListType()));
  // Row 2: an empty list. Not null either.
  vec.SetValue(2, IntList({}));

  EXPECT_TRUE(vec.RowIsValid(0));
  EXPECT_FALSE(vec.RowIsValid(1));
  EXPECT_TRUE(vec.RowIsValid(2));

  EXPECT_FALSE(vec.GetValue(0).IsNull());
  EXPECT_EQ(vec.GetValue(0).GetChildren().size(), 1U);
  EXPECT_TRUE(vec.GetValue(0).GetChildren()[0].IsNull());

  EXPECT_TRUE(vec.GetValue(1).IsNull());

  EXPECT_FALSE(vec.GetValue(2).IsNull());
  EXPECT_TRUE(vec.GetValue(2).GetChildren().empty());

  // And they are all different values.
  EXPECT_NE(vec.GetValue(0), vec.GetValue(1));
  EXPECT_NE(vec.GetValue(0), vec.GetValue(2));
  EXPECT_NE(vec.GetValue(1), vec.GetValue(2));
}

TEST(ListVectorTest, NullArrayRow) {
  Vector vec(IntArrayType(2), 2);
  vec.SetValue(0, Value::Null(IntArrayType(2)));
  vec.SetValue(1, IntArray({3, 4}));
  EXPECT_TRUE(vec.GetValue(0).IsNull());
  EXPECT_EQ(vec.GetValue(1), IntArray({3, 4}));
}

// ---------------------------------------------------------------------------
// Normalify
// ---------------------------------------------------------------------------

TEST(ListVectorTest, FlattenConstantListReplicatesTheElements) {
  Vector vec(IntList({1, 2, 3}));
  ASSERT_EQ(vec.GetVectorType(), VectorType::CONSTANT_VECTOR);

  vec.Normalify(3);
  ASSERT_EQ(vec.GetVectorType(), VectorType::FLAT_VECTOR);
  for (idx_t i = 0; i < 3; i++) {
    EXPECT_EQ(vec.GetValue(i), IntList({1, 2, 3})) << "row " << i;
  }

  // The rows must NOT alias one range of the child: each got its own copy.
  const auto *entries = ListVector::GetEntries(vec);
  EXPECT_NE(entries[0].offset_, entries[1].offset_);
  EXPECT_EQ(ListVector::GetListSize(vec), 9U);

  // The proof: writing into row 0's elements leaves row 1 alone.
  auto &child = ListVector::GetChild(vec);
  child.SetValue(entries[0].offset_, Value(99));
  EXPECT_EQ(vec.GetValue(0), IntList({99, 2, 3}));
  EXPECT_EQ(vec.GetValue(1), IntList({1, 2, 3}));
  EXPECT_EQ(vec.GetValue(2), IntList({1, 2, 3}));
}

TEST(ListVectorTest, FlattenConstantNullList) {
  Vector vec(Value::Null(IntListType()));
  vec.Normalify(3);
  EXPECT_EQ(vec.GetVectorType(), VectorType::FLAT_VECTOR);
  for (idx_t i = 0; i < 3; i++) {
    EXPECT_TRUE(vec.GetValue(i).IsNull());
  }
}

TEST(ListVectorTest, FlattenDictionaryList) {
  Vector vec(IntListType(), 4);
  vec.SetValue(0, IntList({1}));
  vec.SetValue(1, IntList({2, 2}));
  vec.SetValue(2, IntList({3, 3, 3}));

  SelectionVector sel(2);
  sel.SetIndex(0, 2);
  sel.SetIndex(1, 0);
  vec.Slice(sel, 2);
  ASSERT_EQ(vec.GetVectorType(), VectorType::DICTIONARY_VECTOR);

  vec.Normalify(2);
  ASSERT_EQ(vec.GetVectorType(), VectorType::FLAT_VECTOR);
  EXPECT_EQ(vec.GetValue(0), IntList({3, 3, 3}));
  EXPECT_EQ(vec.GetValue(1), IntList({1}));
}

// ---------------------------------------------------------------------------
// Slicing
// ---------------------------------------------------------------------------

TEST(ListVectorTest, SliceThroughASelection) {
  Vector vec(IntListType(), 4);
  vec.SetValue(0, IntList({1, 1}));
  vec.SetValue(1, IntList({2}));
  vec.SetValue(2, IntList({3, 3, 3}));
  vec.SetValue(3, Value::Null(IntListType()));

  SelectionVector sel(3);
  sel.SetIndex(0, 2);
  sel.SetIndex(1, 3);
  sel.SetIndex(2, 0);

  Vector sliced(vec, sel, 3);
  EXPECT_EQ(sliced.GetVectorType(), VectorType::DICTIONARY_VECTOR);
  // The child survived the slice: the elements are still reachable through it.
  EXPECT_EQ(ListVector::GetListSize(sliced), 6U);
  EXPECT_EQ(sliced.GetValue(0), IntList({3, 3, 3}));
  EXPECT_TRUE(sliced.GetValue(1).IsNull());
  EXPECT_EQ(sliced.GetValue(2), IntList({1, 1}));
  EXPECT_FALSE(sliced.RowIsValid(1));
}

TEST(ListVectorTest, SliceAnArrayByOffset) {
  Vector vec(IntArrayType(2), 4);
  vec.SetValue(0, IntArray({1, 2}));
  vec.SetValue(1, IntArray({3, 4}));
  vec.SetValue(2, IntArray({5, 6}));

  Vector sliced(vec, 1);
  EXPECT_EQ(sliced.GetValue(0), IntArray({3, 4}));
  EXPECT_EQ(sliced.GetValue(1), IntArray({5, 6}));
}

// ---------------------------------------------------------------------------
// Copy: the entries must be REMAPPED into the target's own child
// ---------------------------------------------------------------------------

TEST(ListVectorTest, CopyRemapsTheOffsetsIntoTheTargetsChild) {
  // The target already holds a row, so the source's offsets cannot possibly be reused as
  // they are: a verbatim copy of the entries would be caught here even by luck.
  Vector target(IntListType(), 8);
  target.SetValue(0, IntList({42, 42, 42, 42, 42}));

  auto source = std::make_unique<Vector>(IntListType(), 4);
  source->SetValue(0, IntList({1, 2}));
  source->SetValue(1, Value::Null(IntListType()));
  source->SetValue(2, IntList({3}));

  VectorOperations::Copy(*source, target, 3, 0, 1);

  // The target reads the right lists...
  EXPECT_EQ(target.GetValue(0), IntList({42, 42, 42, 42, 42}));
  EXPECT_EQ(target.GetValue(1), IntList({1, 2}));
  EXPECT_TRUE(target.GetValue(2).IsNull());
  EXPECT_EQ(target.GetValue(3), IntList({3}));

  // ...and the elements live in the TARGET's child: its entries point past the row it
  // already held, and its child grew by exactly the three elements copied.
  const auto *entries = ListVector::GetEntries(target);
  EXPECT_GE(entries[1].offset_, 5U);
  EXPECT_EQ(ListVector::GetListSize(target), 8U);

  // The decisive check: scribble over the SOURCE's elements. If the target's entries still
  // pointed into the source's child, its rows would change with them.
  auto &source_child = ListVector::GetChild(*source);
  for (idx_t i = 0; i < ListVector::GetListSize(*source); i++) {
    source_child.SetValue(i, Value(-1));
  }
  EXPECT_EQ(target.GetValue(1), IntList({1, 2}));
  EXPECT_EQ(target.GetValue(3), IntList({3}));

  // And now destroy the source outright: the target must not depend on it at all.
  source.reset();
  EXPECT_EQ(target.GetValue(1), IntList({1, 2}));
  EXPECT_EQ(target.GetValue(3), IntList({3}));
}

TEST(ListVectorTest, CopyThroughASelection) {
  Vector source(IntListType(), 4);
  source.SetValue(0, IntList({1}));
  source.SetValue(1, IntList({2, 2}));
  source.SetValue(2, IntList({3, 3, 3}));

  SelectionVector sel(2);
  sel.SetIndex(0, 2);
  sel.SetIndex(1, 0);

  Vector target(IntListType(), 4);
  VectorOperations::Copy(source, target, sel, 2, 0, 0);
  EXPECT_EQ(target.GetValue(0), IntList({3, 3, 3}));
  EXPECT_EQ(target.GetValue(1), IntList({1}));
  EXPECT_EQ(ListVector::GetListSize(target), 4U);
}

TEST(ListVectorTest, CopyAConstantList) {
  Vector source(IntList({7, 8}));
  Vector target(IntListType(), 4);
  VectorOperations::Copy(source, target, 3, 0, 0);
  for (idx_t i = 0; i < 3; i++) {
    EXPECT_EQ(target.GetValue(i), IntList({7, 8}));
  }
  // Three independent copies, not three aliases of one.
  EXPECT_EQ(ListVector::GetListSize(target), 6U);
}

TEST(ListVectorTest, CopyAnArray) {
  Vector source(IntArrayType(2), 4);
  source.SetValue(0, IntArray({1, 2}));
  source.SetValue(1, IntArray({3, 4}));

  Vector target(IntArrayType(2), 4);
  target.SetValue(0, IntArray({9, 9}));
  VectorOperations::Copy(source, target, 2, 0, 1);

  EXPECT_EQ(target.GetValue(0), IntArray({9, 9}));
  EXPECT_EQ(target.GetValue(1), IntArray({1, 2}));
  EXPECT_EQ(target.GetValue(2), IntArray({3, 4}));

  // The elements are the target's own.
  auto &source_child = ArrayVector::GetChild(source);
  source_child.SetValue(0, Value(-1));
  EXPECT_EQ(target.GetValue(1), IntArray({1, 2}));
}

TEST(ListVectorTest, DataChunkAppendWithAListColumn) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, IntListType()};

  DataChunk first;
  first.Initialize(types);
  first.SetCardinality(2);
  first.SetValue(0, 0, Value(10));
  first.SetValue(1, 0, IntList({1, 2}));
  first.SetValue(0, 1, Value(11));
  first.SetValue(1, 1, IntList({}));

  DataChunk second;
  second.Initialize(types);
  second.SetCardinality(2);
  second.SetValue(0, 0, Value(20));
  second.SetValue(1, 0, IntList({3}));
  second.SetValue(0, 1, Value(21));
  second.SetValue(1, 1, Value::Null(IntListType()));

  first.Append(second);
  ASSERT_EQ(first.GetSize(), 4U);
  EXPECT_EQ(first.GetValue(1, 0), IntList({1, 2}));
  EXPECT_EQ(first.GetValue(1, 1), IntList({}));
  EXPECT_EQ(first.GetValue(1, 2), IntList({3}));
  EXPECT_TRUE(first.GetValue(1, 3).IsNull());
  EXPECT_EQ(first.GetValue(0, 3), Value(21));

  // The appended elements are the target chunk's own.
  second.Reset();
  EXPECT_EQ(first.GetValue(1, 2), IntList({3}));
}

// ---------------------------------------------------------------------------
// Hash
// ---------------------------------------------------------------------------

TEST(ListVectorTest, HashIsElementWise) {
  Vector vec(IntListType(), 8);
  vec.SetValue(0, IntList({1, 2, 3}));
  vec.SetValue(1, IntList({1, 2, 3}));       // equal to row 0
  vec.SetValue(2, IntList({1, 2, 4}));       // one element apart
  vec.SetValue(3, IntList({3, 2, 1}));       // the same elements, reordered
  vec.SetValue(4, IntList({1, 2}));          // a prefix
  vec.SetValue(5, IntList({}));              // empty
  vec.SetValue(6, ListOf({NullElement()}));  // a NULL element is not an empty list

  Vector hashes(LogicalTypeId::HASH);
  VectorOperations::Hash(vec, hashes, 7);

  const auto h = [&](idx_t i) { return hashes.GetValue(i); };
  EXPECT_EQ(h(0), h(1));
  EXPECT_NE(h(0), h(2));
  EXPECT_NE(h(0), h(3));
  EXPECT_NE(h(0), h(4));
  EXPECT_NE(h(4), h(5));
  EXPECT_NE(h(5), h(6));
}

TEST(ListVectorTest, CombineHashWithAListColumn) {
  std::vector<LogicalType> types{LogicalTypeId::INTEGER, IntListType()};
  DataChunk chunk;
  chunk.Initialize(types);
  chunk.SetCardinality(2);
  chunk.SetValue(0, 0, Value(1));
  chunk.SetValue(1, 0, IntList({5, 6}));
  chunk.SetValue(0, 1, Value(1));
  chunk.SetValue(1, 1, IntList({5, 7}));

  Vector hashes(LogicalTypeId::HASH);
  chunk.Hash(hashes);
  // The rows only differ in the list column: the list has to reach the combined hash.
  EXPECT_NE(hashes.GetValue(0), hashes.GetValue(1));
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

TEST(ListVectorTest, EqualsIsElementWiseAndExcludesNullRows) {
  Vector left(IntListType(), 8);
  Vector right(IntListType(), 8);

  left.SetValue(0, IntList({1, 2}));
  right.SetValue(0, IntList({1, 2}));  // equal

  left.SetValue(1, IntList({1, 2}));
  right.SetValue(1, IntList({1, 2, 3}));  // a longer list is not equal

  left.SetValue(2, IntList({1, 2}));
  right.SetValue(2, IntList({2, 1}));  // order matters

  left.SetValue(3, Value::Null(IntListType()));
  right.SetValue(3, Value::Null(IntListType()));  // NULL = NULL is UNKNOWN, never TRUE

  left.SetValue(4, ListOf({Value(1), NullElement()}));
  right.SetValue(4, ListOf({Value(1), NullElement()}));  // a NULL element equals a NULL element

  left.SetValue(5, ListOf({Value(1), NullElement()}));
  right.SetValue(5, IntList({1, 2}));  // a NULL element is not a 2

  left.SetValue(6, IntList({}));
  right.SetValue(6, IntList({}));  // two empty lists are equal

  SelectionVector matched(8);
  auto count = VectorOperations::Equals(left, right, nullptr, 7, &matched);
  ASSERT_EQ(count, 3U);
  EXPECT_EQ(matched.GetIndex(0), 0U);
  EXPECT_EQ(matched.GetIndex(1), 4U);
  EXPECT_EQ(matched.GetIndex(2), 6U);

  // NotEquals is the complement, minus the UNKNOWN (NULL) row.
  SelectionVector not_matched(8);
  auto ne_count = VectorOperations::NotEquals(left, right, nullptr, 7, &not_matched);
  ASSERT_EQ(ne_count, 3U);
  EXPECT_EQ(not_matched.GetIndex(0), 1U);
  EXPECT_EQ(not_matched.GetIndex(1), 2U);
  EXPECT_EQ(not_matched.GetIndex(2), 5U);
}

TEST(ListVectorTest, NotDistinctFromTreatsTwoNullListsAsEqual) {
  Vector left(IntListType(), 4);
  Vector right(IntListType(), 4);

  left.SetValue(0, IntList({1, 2}));
  right.SetValue(0, IntList({1, 2}));

  left.SetValue(1, Value::Null(IntListType()));
  right.SetValue(1, Value::Null(IntListType()));  // both NULL: NOT DISTINCT

  left.SetValue(2, Value::Null(IntListType()));
  right.SetValue(2, IntList({}));  // a NULL list and an empty list ARE distinct

  SelectionVector matched(4);
  auto count = VectorOperations::NotDistinctFrom(left, right, nullptr, 3, &matched);
  ASSERT_EQ(count, 2U);
  EXPECT_EQ(matched.GetIndex(0), 0U);
  EXPECT_EQ(matched.GetIndex(1), 1U);

  SelectionVector distinct(4);
  auto distinct_count = VectorOperations::DistinctFrom(left, right, nullptr, 3, &distinct);
  ASSERT_EQ(distinct_count, 1U);
  EXPECT_EQ(distinct.GetIndex(0), 2U);
}

TEST(ListVectorTest, ArraysCompareElementWise) {
  Vector left(IntArrayType(2), 4);
  Vector right(IntArrayType(2), 4);
  left.SetValue(0, IntArray({1, 2}));
  right.SetValue(0, IntArray({1, 2}));
  left.SetValue(1, IntArray({1, 2}));
  right.SetValue(1, IntArray({1, 3}));

  SelectionVector matched(4);
  auto count = VectorOperations::Equals(left, right, nullptr, 2, &matched);
  ASSERT_EQ(count, 1U);
  EXPECT_EQ(matched.GetIndex(0), 0U);
}

TEST(ListVectorTest, OrderingComparisonsThrow) {
  Vector left(IntListType(), 2);
  Vector right(IntListType(), 2);
  left.SetValue(0, IntList({1}));
  right.SetValue(0, IntList({2}));

  SelectionVector sel(2);
  EXPECT_THROW(VectorOperations::GreaterThan(left, right, nullptr, 1, &sel), NotImplementedException);
  EXPECT_THROW(VectorOperations::LessThan(left, right, nullptr, 1, &sel), NotImplementedException);
  EXPECT_THROW(VectorOperations::LessThanEquals(left, right, nullptr, 1, &sel), NotImplementedException);
  EXPECT_THROW(VectorOperations::GreaterThanEquals(left, right, nullptr, 1, &sel), NotImplementedException);
}

// ---------------------------------------------------------------------------
// The operations a nested value simply does not have
// ---------------------------------------------------------------------------

TEST(ListVectorTest, ArithmeticThrows) {
  Vector left(IntListType(), 2);
  Vector right(IntListType(), 2);
  left.SetValue(0, IntList({1}));
  right.SetValue(0, IntList({2}));
  Vector result(IntListType(), 2);

  EXPECT_THROW(VectorOperations::Sum(left, right, result, 1), NotImplementedException);
  EXPECT_THROW(VectorOperations::Difference(left, right, result, 1), NotImplementedException);
  EXPECT_THROW(VectorOperations::Dot(left, right, result, 1), NotImplementedException);
  EXPECT_THROW(VectorOperations::Division(left, right, result, 1), NotImplementedException);
  EXPECT_THROW(VectorOperations::Modulo(left, right, result, 1), NotImplementedException);
  EXPECT_THROW(VectorOperations::LAnd(left, right, result, 1), NotImplementedException);
  EXPECT_THROW(VectorOperations::Negate(left, result, 1), NotImplementedException);
}

TEST(ListVectorTest, CastThrows) {
  Vector source(IntListType(), 2);
  source.SetValue(0, IntList({1}));
  Vector target(LogicalTypeId::INTEGER, 2);
  EXPECT_THROW(VectorOperations::Cast(source, target, 1), NotImplementedException);

  std::string error;
  EXPECT_THROW(static_cast<void>(VectorOperations::TryCast(source, target, 1, &error)), NotImplementedException);
}

TEST(ListVectorTest, CreateSortKeyThrows) {
  Vector source(IntListType(), 2);
  source.SetValue(0, IntList({1}));
  Vector keys(LogicalTypeId::STRING, 2);
  EXPECT_THROW(CreateSortKey::Create(source, 1, {OrderType::ASCENDING}, keys), NotImplementedException);

  Vector arr(IntArrayType(2), 2);
  arr.SetValue(0, IntArray({1, 2}));
  EXPECT_THROW(CreateSortKey::Create(arr, 1, {OrderType::ASCENDING}, keys), NotImplementedException);
}

// ---------------------------------------------------------------------------
// Nesting
// ---------------------------------------------------------------------------

TEST(ListVectorTest, ListOfList) {
  const auto inner_type = IntListType();
  const auto outer_type = LogicalType::List(inner_type);

  const auto nested = [&](const std::vector<std::vector<int32_t>> &rows) {
    std::vector<Value> children;
    children.reserve(rows.size());
    for (const auto &row : rows) {
      children.push_back(IntList(row));
    }
    return Value::List(outer_type, std::move(children));
  };

  Vector vec(outer_type, 4);
  vec.SetValue(0, nested({{1, 2}, {3}}));
  vec.SetValue(1, nested({{}}));

  EXPECT_EQ(vec.GetValue(0), nested({{1, 2}, {3}}));
  EXPECT_EQ(vec.GetValue(1), nested({{}}));

  // A copy of a nested list has to remap the offsets at BOTH levels.
  Vector target(outer_type, 4);
  target.SetValue(0, nested({{9, 9, 9}}));
  VectorOperations::Copy(vec, target, 2, 0, 1);
  EXPECT_EQ(target.GetValue(0), nested({{9, 9, 9}}));
  EXPECT_EQ(target.GetValue(1), nested({{1, 2}, {3}}));
  EXPECT_EQ(target.GetValue(2), nested({{}}));

  // Hashing recurses too.
  Vector hashes(LogicalTypeId::HASH);
  VectorOperations::Hash(target, hashes, 3);
  EXPECT_NE(hashes.GetValue(0), hashes.GetValue(1));
}

}  // namespace bumblebee
