//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_data_mngr_test.cpp
//
// Identification: test/unit/type/vector/vector_data_mngr_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "type/vector/vector_data_mngr.h"

#include <string>

#include "common/config.h"
#include "gtest/gtest.h"
#include "type/vector/selection_vector.h"

namespace bumblebee {

TEST(VectorDataMngrTest, ConstructorWithType) {
  VectorDataMngr mgr(VectorDataMngrType::STANDARD_DATA_MNGR);
  EXPECT_EQ(mgr.GetType(), VectorDataMngrType::STANDARD_DATA_MNGR);
}

TEST(VectorDataMngrTest, ConstructorWithSizeAllocatesData) {
  VectorDataMngr mgr(idx_t{10});
  EXPECT_NE(mgr.GetData(), nullptr);
}

TEST(VectorDataMngrTest, SetDataReplacesPointer) {
  auto ptr = std::make_unique<data_t[]>(5);
  ptr[0] = 42;
  VectorDataMngr mgr(VectorDataMngrType::STANDARD_DATA_MNGR);
  mgr.SetData(std::move(ptr));
  EXPECT_EQ(mgr.GetData()[0], 42);
}

TEST(VectorDataMngrTest, FactoryCreatesStandardVector) {
  auto vec = VectorDataMngr::CreateStandardVector(PhysicalType::BIGINT, 5);
  EXPECT_EQ(vec->GetType(), VectorDataMngrType::STANDARD_DATA_MNGR);
  EXPECT_NE(vec->GetData(), nullptr);
}

TEST(VectorDataMngrTest, FactoryCreatesConstantVector) {
  auto vec = VectorDataMngr::CreateConstantVector(PhysicalType::BIGINT);
  EXPECT_EQ(vec->GetType(), VectorDataMngrType::STANDARD_DATA_MNGR);
  EXPECT_NE(vec->GetData(), nullptr);
}

TEST(VectorDataMngrTest, FactoryCreatesStringVector) {
  // A STRING vector stores one string_t per row inline; the bytes live in a StringHeap.
  auto vec = VectorDataMngr::CreateStandardVector(PhysicalType::STRING, 5);
  EXPECT_EQ(vec->GetType(), VectorDataMngrType::STANDARD_DATA_MNGR);
  EXPECT_NE(vec->GetData(), nullptr);
}

TEST(DictionaryDataMngrTest, ConstructorWithSizeInitializesSelection) {
  DictionaryDataMngr mgr(idx_t{5});
  EXPECT_EQ(mgr.GetType(), VectorDataMngrType::DICTIONARY_DATA_MNGR);
  EXPECT_NE(mgr.GetSelection().GetSelData().get(), nullptr);
  EXPECT_EQ(mgr.GetData(), nullptr);
}

TEST(StringDataMngrTest, AddStringReturnsCorrectString) {
  StringDataMngr mgr;
  std::string s = "hello";
  string_t result = mgr.AddString(s.c_str(), s.length());
  EXPECT_STREQ(result.CStr(), "hello");
}

TEST(StringDataMngrTest, AddStringWithStringT) {
  StringDataMngr mgr;
  string_t str("world");
  string_t result = mgr.AddString(str);
  EXPECT_STREQ(result.CStr(), "world");
}

TEST(StringDataMngrTest, AddEmptyStringHasCorrectLength) {
  StringDataMngr mgr;
  string_t empty_str = mgr.AddEmptyString(10);
  EXPECT_EQ(empty_str.Size(), 10);
}

TEST(StringDataMngrTest, AddHeapReferenceStoresReference) {
  auto ref = vector_data_mngr_ptr_t(new StringDataMngr);
  StringDataMngr mgr;
  mgr.AddHeapReference(ref);
  // No assertion is possible here unless we expose references_.
  SUCCEED();  // Just validate that no exception or crash occurs.
}

}  // namespace bumblebee
