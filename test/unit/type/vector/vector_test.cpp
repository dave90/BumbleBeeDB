//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_test.cpp
//
// Identification: test/unit/type/vector/vector_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/vector.h"

#include "common/config.h"
#include "gtest/gtest.h"
#include "type/value.h"

namespace bumblebee {

// ---------- Construction ----------

TEST(VectorTest, ConstructFromValue) {
  Value val(42);
  Vector vec(val);
  EXPECT_EQ(vec.GetVectorType(), VectorType::CONSTANT_VECTOR);
  EXPECT_EQ(vec.GetValue(0), val);
}

TEST(VectorTest, ConstructFlatVectorZeroInit) {
  Vector vec(PhysicalType::INTEGER, true, true, 10);
  EXPECT_EQ(vec.GetVectorType(), VectorType::FLAT_VECTOR);
  for (idx_t i = 0; i < 10; ++i) {
    EXPECT_EQ(vec.GetValue(i), Value(0));
  }
}

TEST(VectorTest, ConstructSequenceVector) {
  Vector vec(PhysicalType::BIGINT);
  vec.Sequence(5, 2);
  EXPECT_EQ(vec.GetVectorType(), VectorType::SEQUENCE_VECTOR);
  for (idx_t i = 0; i < 5; ++i) {
    // NOTE: the original wrote `Value(5 + 2 * i)`, which is a UBIGINT because `i` is an
    // idx_t. It only matched because the original's sequence GetValue had the same
    // unsigned-promotion bug. Ours returns a BIGINT, as a BIGINT sequence should.
    EXPECT_EQ(vec.GetValue(i), Value(static_cast<int64_t>(5 + 2 * i)));
  }
}

TEST(VectorTest, ConstructCircularSequenceVector) {
  Vector vec(PhysicalType::BIGINT);
  vec.Sequence(5, 0, 1, 10);  // 5 6 7 8 9 10 5 6 7 8 ...
  EXPECT_EQ(vec.GetVectorType(), VectorType::SEQUENCE_CIRCULAR_VECTOR);
  for (idx_t i = 0; i < 10; ++i) {
    EXPECT_EQ(vec.GetValue(i), Value(static_cast<int64_t>(5 + (i % 6))));
  }
}

TEST(VectorTest, ConstructCircularSequenceVectorWithOffsetAndStride) {
  Vector vec(PhysicalType::BIGINT);
  vec.Sequence(5, 4, 2, 10);  // 7 7 8 8 9 9 10 10 5 5 6 6 ...
  EXPECT_EQ(vec.GetVectorType(), VectorType::SEQUENCE_CIRCULAR_VECTOR);
  for (idx_t i = 0; i < 10; ++i) {
    EXPECT_EQ(vec.GetValue(i), Value(static_cast<int64_t>(5 + (i + 4) / 2 % 6)));
  }
}

TEST(VectorTest, ConstructNegativeCircularSequenceVector) {
  Vector vec(PhysicalType::BIGINT);
  vec.Sequence(-4, 0, 3, 4);  // -4 -4 -4 -3 -3 -3 -2 -2 -2 -1 -1 -1 ...
  EXPECT_EQ(vec.GetVectorType(), VectorType::SEQUENCE_CIRCULAR_VECTOR);
  for (idx_t i = 0; i < 100; ++i) {
    auto val = static_cast<int64_t>(-4 + (i / 3 % 9));
    EXPECT_EQ(vec.GetValue(i), Value(val));
  }
}

TEST(VectorTest, MoveConstructor) {
  Vector vec1(PhysicalType::INTEGER, true);
  vec1.SetValue(0, Value(123));
  Vector vec2(std::move(vec1));
  EXPECT_EQ(vec2.GetValue(0), Value(123));
}

// ---------- Slice & reference ----------

TEST(VectorTest, SliceFlatVectorOffset) {
  Vector vec(PhysicalType::INTEGER);
  for (idx_t i = 0; i < 5; ++i) {
    vec.SetValue(i, Value(static_cast<int32_t>(i)));
  }
  Vector sliced(vec, 2);
  EXPECT_EQ(sliced.GetValue(0), Value(2));
}

TEST(VectorTest, SliceWithSelectionVector) {
  Vector vec(PhysicalType::INTEGER);
  for (idx_t i = 0; i < 10; ++i) {
    vec.SetValue(i, Value(static_cast<int32_t>(i * 10)));
  }

  SelectionVector sel(3);
  sel.SetIndex(0, 1);
  sel.SetIndex(1, 2);
  sel.SetIndex(2, 3);

  Vector dict_vec(vec, sel, 2);
  EXPECT_EQ(dict_vec.GetVectorType(), VectorType::DICTIONARY_VECTOR);
  EXPECT_EQ(dict_vec.GetValue(0), Value(10));
  EXPECT_EQ(dict_vec.GetValue(1), Value(20));
}

TEST(VectorTest, SliceOfSliceSelectionVector) {
  Vector vec(PhysicalType::INTEGER);
  for (idx_t i = 0; i < 10; ++i) {
    vec.SetValue(i, Value(static_cast<int32_t>(i * 10)));
  }

  {
    SelectionVector sel1(5);
    sel1.SetIndex(0, 3);
    sel1.SetIndex(1, 4);
    sel1.SetIndex(2, 5);
    sel1.SetIndex(3, 7);
    sel1.SetIndex(4, 8);
    vec.Slice(sel1, 5);
  }
  EXPECT_EQ(vec.GetVectorType(), VectorType::DICTIONARY_VECTOR);
  EXPECT_EQ(vec.GetValue(0).GetAs<int32_t>(), 30);
  EXPECT_EQ(vec.GetValue(1).GetAs<int32_t>(), 40);
  EXPECT_EQ(vec.GetValue(2).GetAs<int32_t>(), 50);
  EXPECT_EQ(vec.GetValue(3).GetAs<int32_t>(), 70);
  EXPECT_EQ(vec.GetValue(4).GetAs<int32_t>(), 80);

  {
    // A selection over a dictionary composes the two selections instead of nesting.
    SelectionVector sel2(3);
    sel2.SetIndex(0, 2);
    sel2.SetIndex(1, 3);
    sel2.SetIndex(2, 4);
    vec.Slice(sel2, 3);
  }
  EXPECT_EQ(vec.GetVectorType(), VectorType::DICTIONARY_VECTOR);
  EXPECT_EQ(vec.GetValue(0).GetAs<int32_t>(), 50);
  EXPECT_EQ(vec.GetValue(1).GetAs<int32_t>(), 70);
  EXPECT_EQ(vec.GetValue(2).GetAs<int32_t>(), 80);
}

// ---------- Value access ----------

TEST(VectorTest, GetSetValuesInteger) {
  Vector vec(PhysicalType::INTEGER);
  vec.SetValue(5, Value(77));
  EXPECT_EQ(vec.GetValue(5), Value(77));
}

TEST(VectorTest, GetSetValuesString) {
  Vector vec(PhysicalType::STRING);
  vec.SetValue(0, Value("hello"));
  EXPECT_EQ(vec.GetValue(0), Value("hello"));
}

// ---------- Flattening ----------

TEST(VectorTest, NormalifyConstantVector) {
  Vector vec(Value(42));
  vec.Normalify(3);
  EXPECT_EQ(vec.GetVectorType(), VectorType::FLAT_VECTOR);
  for (idx_t i = 0; i < 3; ++i) {
    EXPECT_EQ(vec.GetValue(i), Value(42));
  }
}

TEST(VectorTest, NormalifyStringConstantVector) {
  Vector vec(Value("MIAO"));
  vec.Normalify(3);
  EXPECT_EQ(vec.GetVectorType(), VectorType::FLAT_VECTOR);
  for (idx_t i = 0; i < 3; ++i) {
    EXPECT_EQ(vec.GetValue(i), Value("MIAO"));
  }
}

TEST(VectorTest, NormalifyDictionaryVector) {
  Vector base_vec(PhysicalType::INTEGER, 5);
  for (idx_t i = 0; i < 5; ++i) {
    base_vec.SetValue(i, Value(static_cast<int32_t>(i * 10)));
  }

  SelectionVector sel(2);
  sel.SetIndex(0, 2);
  sel.SetIndex(1, 3);

  Vector dict_vec(base_vec, sel, 2);
  EXPECT_EQ(dict_vec.GetVectorType(), VectorType::DICTIONARY_VECTOR);

  dict_vec.Normalify(2);

  EXPECT_EQ(dict_vec.GetVectorType(), VectorType::FLAT_VECTOR);
  EXPECT_EQ(dict_vec.GetValue(0), Value(20));
  EXPECT_EQ(dict_vec.GetValue(1), Value(30));
}

TEST(VectorTest, NormalifySequenceVector) {
  Vector vec(PhysicalType::BIGINT);
  vec.Sequence(10, 3);
  vec.Normalify(4);

  EXPECT_EQ(vec.GetVectorType(), VectorType::FLAT_VECTOR);
  EXPECT_EQ(vec.GetValue(0), Value(static_cast<int64_t>(10)));
  EXPECT_EQ(vec.GetValue(1), Value(static_cast<int64_t>(13)));
  EXPECT_EQ(vec.GetValue(2), Value(static_cast<int64_t>(16)));
  EXPECT_EQ(vec.GetValue(3), Value(static_cast<int64_t>(19)));
}

TEST(VectorTest, NormalifyCircularSequenceVector) {
  Vector vec(PhysicalType::BIGINT);
  vec.Sequence(10, 0, 1, 20);  // 10 11 12 ... 20 10 11 12 ...
  vec.Normalify(20);

  EXPECT_EQ(vec.GetVectorType(), VectorType::FLAT_VECTOR);
  EXPECT_EQ(vec.GetValue(0), Value(static_cast<int64_t>(10)));
  EXPECT_EQ(vec.GetValue(1), Value(static_cast<int64_t>(11)));
  EXPECT_EQ(vec.GetValue(2), Value(static_cast<int64_t>(12)));
  EXPECT_EQ(vec.GetValue(10), Value(static_cast<int64_t>(20)));
  EXPECT_EQ(vec.GetValue(11), Value(static_cast<int64_t>(10)));
}

TEST(VectorTest, NormalifyCircularSequenceVectorWithOffset) {
  Vector vec(PhysicalType::BIGINT);
  vec.Sequence(10, 10, 1, 20);  // 20 10 11 12 13 ...
  vec.Normalify(15);

  EXPECT_EQ(vec.GetVectorType(), VectorType::FLAT_VECTOR);
  EXPECT_EQ(vec.GetValue(0), Value(static_cast<int64_t>(20)));
  EXPECT_EQ(vec.GetValue(1), Value(static_cast<int64_t>(10)));
  EXPECT_EQ(vec.GetValue(2), Value(static_cast<int64_t>(11)));
  EXPECT_EQ(vec.GetValue(12), Value(static_cast<int64_t>(10)));
}

TEST(VectorTest, NormalifySequenceWithSelection) {
  Vector vec(PhysicalType::BIGINT);
  vec.Sequence(100, 5);

  SelectionVector sel(3);
  sel.SetIndex(0, 0);
  sel.SetIndex(1, 2);
  sel.SetIndex(2, 4);
  vec.Normalify(sel, 3);

  EXPECT_EQ(vec.GetVectorType(), VectorType::FLAT_VECTOR);
  EXPECT_EQ(vec.GetValue(sel.GetIndex(0)), Value(static_cast<int64_t>(100)));  // 100 + 5*0
  EXPECT_EQ(vec.GetValue(sel.GetIndex(1)), Value(static_cast<int64_t>(110)));  // 100 + 5*2
  EXPECT_EQ(vec.GetValue(sel.GetIndex(2)), Value(static_cast<int64_t>(120)));  // 100 + 5*4
}

TEST(VectorTest, SliceAndNormalifyCircularSequenceWithOffset) {
  Vector vec(PhysicalType::BIGINT);
  vec.Sequence(-3, 5, 3, -1);
  // -2 -1 -1 -1 -3 -3 -3 -2 -2 -2

  SelectionVector sel(4);
  sel.SetIndex(0, 0);
  sel.SetIndex(1, 1);
  sel.SetIndex(2, 4);
  sel.SetIndex(3, 7);
  vec.Slice(sel, 4);
  vec.Normalify(4);

  EXPECT_EQ(vec.GetVectorType(), VectorType::FLAT_VECTOR);
  EXPECT_EQ(vec.GetValue(0), Value(static_cast<int64_t>(-2)));
  EXPECT_EQ(vec.GetValue(1), Value(static_cast<int64_t>(-1)));
  EXPECT_EQ(vec.GetValue(2), Value(static_cast<int64_t>(-3)));
  EXPECT_EQ(vec.GetValue(3), Value(static_cast<int64_t>(-2)));
}

// ---------- Orrify ----------

TEST(VectorTest, OrrifyFlatVector) {
  Vector vec(PhysicalType::INTEGER, 10);
  for (idx_t i = 0; i < 3; ++i) {
    vec.SetValue(i, Value(static_cast<int32_t>(i)));
  }

  VectorData vdata;
  vec.Orrify(3, vdata);
  EXPECT_NE(vdata.data_, nullptr);
  EXPECT_NE(vdata.sel_, nullptr);
  for (idx_t i = 0; i < 3; ++i) {
    EXPECT_EQ(reinterpret_cast<int32_t *>(vdata.data_)[i], static_cast<int32_t>(i));
  }
}

TEST(VectorTest, OrrifyConstVector) {
  Vector vec(Value(static_cast<int32_t>(100)));

  VectorData vdata;
  vec.Orrify(10, vdata);

  EXPECT_NE(vdata.data_, nullptr);
  EXPECT_NE(vdata.sel_, nullptr);
  auto *data = vdata.data_;
  const auto *sel = vdata.sel_->GetData();
  for (idx_t i = 0; i < 3; ++i) {
    EXPECT_EQ(reinterpret_cast<int32_t *>(data)[sel[i]], 100);
  }
}

TEST(VectorTest, OrrifyDictionaryVector) {
  Vector base_vec(PhysicalType::INTEGER);
  base_vec.SetValue(0, Value(100));
  base_vec.SetValue(1, Value(200));
  base_vec.SetValue(2, Value(300));

  SelectionVector sel(2);
  sel.SetIndex(0, 2);
  sel.SetIndex(1, 0);

  Vector dict_vec(base_vec, sel, 2);

  VectorData vdata;
  dict_vec.Orrify(2, vdata);

  ASSERT_NE(vdata.data_, nullptr);
  ASSERT_NE(vdata.sel_, nullptr);
  EXPECT_EQ(vdata.sel_->GetIndex(0), 2);
  EXPECT_EQ(vdata.sel_->GetIndex(1), 0);

  const auto *data = reinterpret_cast<const int32_t *>(vdata.data_);
  EXPECT_EQ(data[2], 300);
  EXPECT_EQ(data[0], 100);
}

TEST(VectorTest, OrrifyFlatVectorContents) {
  Vector vec(PhysicalType::INTEGER);
  vec.SetValue(0, Value(11));
  vec.SetValue(1, Value(22));
  vec.SetValue(2, Value(33));

  VectorData vdata;
  vec.Orrify(3, vdata);

  ASSERT_NE(vdata.data_, nullptr);
  ASSERT_NE(vdata.sel_, nullptr);
  EXPECT_EQ(reinterpret_cast<int32_t *>(vdata.data_)[vdata.sel_->GetIndex(0)], 11);
  EXPECT_EQ(reinterpret_cast<int32_t *>(vdata.data_)[vdata.sel_->GetIndex(1)], 22);
  EXPECT_EQ(reinterpret_cast<int32_t *>(vdata.data_)[vdata.sel_->GetIndex(2)], 33);
}

TEST(VectorTest, OrrifyDictionaryWithSequenceChild) {
  Vector seq_vec(PhysicalType::BIGINT);
  seq_vec.Sequence(100, 10);  // 100, 110, 120, ...

  SelectionVector sel(3);
  sel.SetIndex(0, 0);  // 100
  sel.SetIndex(1, 2);  // 120
  sel.SetIndex(2, 4);  // 140

  Vector dict_vec(seq_vec, sel, 3);
  EXPECT_EQ(dict_vec.GetVectorType(), VectorType::DICTIONARY_VECTOR);

  VectorData vdata;
  dict_vec.Orrify(3, vdata);

  ASSERT_NE(vdata.data_, nullptr);
  ASSERT_NE(vdata.sel_, nullptr);

  const auto *data = reinterpret_cast<const int64_t *>(vdata.data_);
  const sel_t *indices = vdata.sel_->GetData();

  EXPECT_EQ(data[indices[0]], 100);
  EXPECT_EQ(data[indices[1]], 120);
  EXPECT_EQ(data[indices[2]], 140);
}

TEST(VectorTest, OrrifyConstStringVector) {
  Vector vec(Value("buzz"));

  VectorData vdata;
  vec.Orrify(4, vdata);

  ASSERT_NE(vdata.data_, nullptr);
  ASSERT_NE(vdata.sel_, nullptr);
  const auto *sel = vdata.sel_->GetData();
  auto *data = reinterpret_cast<string_t *>(vdata.data_);
  for (idx_t i = 0; i < 4; ++i) {
    EXPECT_EQ(data[sel[i]].GetString(), "buzz");
  }
}

// ---------- Auxiliary data ----------

TEST(VectorTest, StringVectorHeapReference) {
  Vector vec(PhysicalType::STRING);
  string_t s = StringVector::AddString(vec, "test");
  EXPECT_EQ(std::string(s.GetDataUnsafe(), s.Size()), "test");
}

// ---------- Resize ----------

TEST(VectorTest, ResizeData) {
  Vector vec(PhysicalType::INTEGER);
  vec.SetValue(0, Value(10));
  vec.Resize(1, 3);
  vec.SetValue(1, Value(20));
  EXPECT_EQ(vec.GetValue(0), Value(10));
  EXPECT_EQ(vec.GetValue(1), Value(20));
}

}  // namespace bumblebee
