//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bumble_string_test.cpp
//
// Identification: test/unit/type/bumble_string_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <cstring>
#include <string>

#include "gtest/gtest.h"
#include "type/bumble_string.h"

namespace bumblebee {

TEST(BumbleStringTest, InlineConstructorShortString) {
  const char *str = "short";  // length = 5
  BumbleString s(str);
  std::string expected = "short";

  EXPECT_EQ(s.Length(), 5);
  EXPECT_TRUE(s.IsInlined());
  EXPECT_STREQ(s.CStr(), str);
  EXPECT_STREQ(s.GetString().c_str(), expected.c_str());
  EXPECT_EQ(strcmp(s.GetDataUnsafe(), str), 0);
  EXPECT_EQ(strcmp(s.GetPrefix(), str), 0);
}

TEST(BumbleStringTest, NonInlineConstructorLongString) {
  const char *str = "this_is_a_longer_string";  // > 11
  BumbleString s(str);
  std::string exp = "this_is_a_longer_string";

  EXPECT_EQ(s.Length(), strlen(str));
  EXPECT_FALSE(s.IsInlined());
  EXPECT_STREQ(s.CStr(), str);
  EXPECT_EQ(s.GetString(), exp);

  // The prefix should contain the first 11 chars.
  std::string expected_prefix(str, BumbleString::PREFIX_LENGTH);
  EXPECT_EQ(std::string(s.GetPrefix()), expected_prefix);
}

TEST(BumbleStringTest, ConstructorWithExplicitLength) {
  const char *str = "123456789012345";  // len = 15
  std::string exp = "123456789012345";
  BumbleString s(str, 15);

  EXPECT_EQ(s.Length(), 15);
  EXPECT_FALSE(s.IsInlined());
  EXPECT_STREQ(s.CStr(), str);
  EXPECT_EQ(s.GetString(), exp);
}

TEST(BumbleStringTest, CopyConstructor) {
  const char *str = "inline_cp";
  BumbleString original(str);
  BumbleString copy(original);

  EXPECT_EQ(copy.Length(), original.Length());
  EXPECT_STREQ(copy.CStr(), original.CStr());
  EXPECT_EQ(copy.GetString(), original.GetString());
  EXPECT_EQ(copy.IsInlined(), original.IsInlined());
}

TEST(BumbleStringTest, ComparisonOperator) {
  BumbleString a("apple");
  BumbleString b("banana");

  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);

  BumbleString c("apple");
  EXPECT_FALSE(a < c);
  EXPECT_FALSE(c < a);
}

TEST(BumbleStringTest, GetDataWriteableReturnsSameAsUnsafe) {
  const char *str = "shorty";
  BumbleString s(str);

  EXPECT_STREQ(s.GetDataUnsafe(), s.GetDataWriteable());
}

TEST(BumbleStringTest, StaticIsInlinedFunction) {
  EXPECT_TRUE(BumbleString::IsInlined(0));
  EXPECT_TRUE(BumbleString::IsInlined(11));
  EXPECT_FALSE(BumbleString::IsInlined(12));
  EXPECT_FALSE(BumbleString::IsInlined(100));
}

}  // namespace bumblebee
