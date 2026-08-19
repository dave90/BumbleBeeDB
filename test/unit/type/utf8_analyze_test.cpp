//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// utf8_analyze_test.cpp
//
// Identification: test/unit/type/utf8_analyze_test.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//
//
// Utf8Proc::Analyze is on the parquet string-decode hot path and carries two word-at-a-time fast
// paths (an ASCII one and a 2-byte-sequence one for Cyrillic-like text). These pin its
// classification across the fast/slow path boundaries: run lengths around the 8-byte word size,
// mixed scripts, NUL detection with the exact position, and truncated/invalid sequences — which
// must be INVALID, never a read past the buffer.
//
//===----------------------------------------------------------------------===//

#include <cstring>
#include <string>

#include "gtest/gtest.h"
#include "utf8proc/utf8proc_wrapper.hpp"

namespace bumblebee {

static auto TypeOf(const std::string &s) -> UnicodeType { return Utf8Proc::Analyze(s.data(), s.size()); }

/** @brief Analyze expecting INVALID; returns the reported reason/position pair. */
static auto InvalidAt(const std::string &s) -> std::pair<UnicodeInvalidReason, size_t> {
  UnicodeInvalidReason reason{};
  size_t pos = 0;
  EXPECT_EQ(Utf8Proc::Analyze(s.data(), s.size(), &reason, &pos), UnicodeType::INVALID);
  return {reason, pos};
}

TEST(Utf8AnalyzeTest, AsciiAndEmpty) {
  EXPECT_EQ(TypeOf(""), UnicodeType::ASCII);
  EXPECT_EQ(TypeOf("a"), UnicodeType::ASCII);
  EXPECT_EQ(TypeOf("hello world"), UnicodeType::ASCII);         // crosses the 8-byte word
  EXPECT_EQ(TypeOf("0123456"), UnicodeType::ASCII);             // 7 bytes: scalar tail only
  EXPECT_EQ(TypeOf("01234567"), UnicodeType::ASCII);            // exactly one word
  EXPECT_EQ(TypeOf(std::string(65, 'x')), UnicodeType::ASCII);  // words + 1 tail byte
}

TEST(Utf8AnalyzeTest, TwoByteRuns) {
  // Pure Cyrillic of varying lengths: exercises the batched 2-byte word path (4 chars/word) and
  // its scalar tail on both sides of the 8-byte boundary.
  const std::string ru = "привет";  // 6 chars = 12 bytes: one word + 2 tail
  EXPECT_EQ(TypeOf(ru), UnicodeType::UNICODE);
  EXPECT_EQ(TypeOf("фыва"), UnicodeType::UNICODE);                  // exactly 8 bytes
  EXPECT_EQ(TypeOf("ф"), UnicodeType::UNICODE);                     // single 2-byte char, below word size
  EXPECT_EQ(TypeOf("это поисковый запрос"), UnicodeType::UNICODE);  // 2-byte runs + ASCII spaces
  EXPECT_EQ(TypeOf("abcф"), UnicodeType::UNICODE);                  // ASCII prefix into a trailing 2-byte char
  EXPECT_EQ(TypeOf("фabc"), UnicodeType::UNICODE);                  // 2-byte char back into ASCII
}

TEST(Utf8AnalyzeTest, WiderSequences) {
  EXPECT_EQ(TypeOf("\xE2\x82\xAC"), UnicodeType::UNICODE);      // € (3-byte)
  EXPECT_EQ(TypeOf("\xF0\x9F\x90\x9D"), UnicodeType::UNICODE);  // 🐝 (4-byte)
  EXPECT_EQ(TypeOf("на\xE2\x82\xAC!"), UnicodeType::UNICODE);   // 2-byte + 3-byte + ASCII mix
}

TEST(Utf8AnalyzeTest, NulByteReportsPosition) {
  EXPECT_EQ(InvalidAt(std::string("ab\0cd", 5)), (std::pair{UnicodeInvalidReason::NULL_BYTE, size_t{2}}));
  // A NUL inside word-aligned ASCII must be found by the SWAR test too.
  EXPECT_EQ(InvalidAt(std::string("01234567\0x", 10)), (std::pair{UnicodeInvalidReason::NULL_BYTE, size_t{8}}));
}

TEST(Utf8AnalyzeTest, InvalidAndTruncatedSequences) {
  // A bare continuation byte, and a lead byte followed by a non-continuation.
  EXPECT_EQ(InvalidAt("\x80").first, UnicodeInvalidReason::BYTE_MISMATCH);
  EXPECT_EQ(InvalidAt("\xD1z").first, UnicodeInvalidReason::BYTE_MISMATCH);
  // Truncated at the end of the buffer: 2-, 3- and 4-byte leads with missing continuations must
  // classify INVALID (bounds-checked — not read past the string).
  EXPECT_EQ(InvalidAt("\xD1").first, UnicodeInvalidReason::BYTE_MISMATCH);
  EXPECT_EQ(InvalidAt("\xE2\x82").first, UnicodeInvalidReason::BYTE_MISMATCH);
  EXPECT_EQ(InvalidAt("\xF0\x9F\x90").first, UnicodeInvalidReason::BYTE_MISMATCH);
  EXPECT_EQ(InvalidAt("привет\xD1").first, UnicodeInvalidReason::BYTE_MISMATCH);  // after a 2-byte run
}

}  // namespace bumblebee
