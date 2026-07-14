//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// parser_smoke_test.cpp
//
// Identification: test/unit/binder/parser_smoke_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "postgres_parser.hpp"

namespace bumblebee {

TEST(ParserSmokeTest, ParseSelect) {
  duckdb::PostgresParser parser;
  parser.Parse("SELECT 1;");
  EXPECT_TRUE(parser.success);
  EXPECT_NE(parser.parse_tree, nullptr);
}

TEST(ParserSmokeTest, ParseCreateTableWithArrayColumn) {
  duckdb::PostgresParser parser;
  parser.Parse("CREATE TABLE t(v INT, tags INT[], fixed INT[3]);");
  EXPECT_TRUE(parser.success);
  EXPECT_NE(parser.parse_tree, nullptr);
}

TEST(ParserSmokeTest, ReportsSyntaxError) {
  duckdb::PostgresParser parser;
  parser.Parse("SELECT FROM FROM;");
  EXPECT_FALSE(parser.success);
  EXPECT_FALSE(parser.error_message.empty());
}

}  // namespace bumblebee
