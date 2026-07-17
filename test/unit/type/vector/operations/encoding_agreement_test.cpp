//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// encoding_agreement_test.cpp
//
// Identification: test/unit/type/vector/operations/encoding_agreement_test.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/config.h"
#include "gtest/gtest.h"
#include "type/vector/vector.h"
#include "type/vector/operations/vector_operations.h"

namespace bumblebee {

/**
 * A kernel must not care how its inputs are ENCODED.
 *
 * `Equals(constant, dictionary)` and `Equals(flat, flat)` are the same question about the
 * same logical data, and they must give the same answer. Each encoding is a different code
 * path inside the kernel — the constant fast path, the flat tight loop, the orrified
 * generic loop — and only the flat one is what most tests exercise. A bug in any of the
 * others survives a port and then corrupts a join months later, when the planner happens to
 * hand a hash join a dictionary vector on one side.
 *
 * So: build the SAME logical column in every encoding that can represent it, run every
 * pairing through every kernel, and require the answer to match `flat OP flat`.
 */
class EncodingAgreementTest : public ::testing::Test {
 protected:
  static constexpr idx_t N = 8;

  /** A logical column: the values, with a nullopt for a NULL. */
  using Column = std::vector<std::optional<int32_t>>;

  /** Keeps every Vector (and every dictionary's base) alive for the test's duration. */
  std::vector<std::unique_ptr<Vector>> owned_;

  auto Own(Vector &&v) -> Vector * {
    owned_.push_back(std::make_unique<Vector>(std::move(v)));
    return owned_.back().get();
  }

  /** @brief A plain FLAT vector holding `col`. */
  auto MakeFlat(const Column &col) -> Vector * {
    Vector v(PhysicalType::INTEGER, N);
    for (idx_t i = 0; i < N; i++) {
      if (col[i].has_value()) {
        v.SetValue(i, Value(*col[i]));
      } else {
        v.SetValue(i, Value::Null(PhysicalType::INTEGER));
      }
    }
    return Own(std::move(v));
  }

  /**
   * @brief A DICTIONARY vector holding `col`.
   *
   * The base is twice as long and holds the real values at the ODD rows, with decoy values
   * (and a NULL) at the even ones. The selection picks the odd rows back out. So the
   * dictionary's data pointer, its validity mask and its row indices ALL disagree with the
   * logical column — which is exactly the misalignment a kernel that forgets to read
   * through the selection would trip on.
   */
  auto MakeDict(const Column &col) -> Vector * {
    Vector base(PhysicalType::INTEGER, 2 * N);
    for (idx_t i = 0; i < N; i++) {
      // Decoys at the even rows: a value that is in neither dataset, and a NULL.
      base.SetValue(2 * i, Value::Null(PhysicalType::INTEGER));
      if (col[i].has_value()) {
        base.SetValue(2 * i + 1, Value(*col[i]));
      } else {
        base.SetValue(2 * i + 1, Value::Null(PhysicalType::INTEGER));
      }
    }
    SelectionVector sel(N);
    for (idx_t i = 0; i < N; i++) {
      sel.SetIndex(i, 2 * i + 1);
    }
    Vector *base_owned = Own(std::move(base));
    Vector dict(*base_owned, sel, N);
    return Own(std::move(dict));
  }

  /** @brief A CONSTANT vector holding `col`. Only valid when every row of `col` is equal. */
  auto MakeConstant(const Column &col) -> Vector * {
    Value v = col[0].has_value() ? Value(*col[0]) : Value::Null(PhysicalType::INTEGER);
    Vector c(v);
    return Own(std::move(c));
  }

  /** @brief A SEQUENCE vector. Only valid when `col` is an arithmetic run with no NULLs. */
  auto MakeSequence(int64_t start, int64_t increment) -> Vector * {
    Vector v(PhysicalType::INTEGER, N);
    v.Sequence(start, increment);
    return Own(std::move(v));
  }

  /** @return True if every row of `col` holds the same thing (value or NULL). */
  static auto IsUniform(const Column &col) -> bool {
    return std::all_of(col.begin(), col.end(), [&](const std::optional<int32_t> &x) { return x == col[0]; });
  }

  /** @return True if `col` is start, start + inc, ... with no NULLs. */
  static auto IsArithmetic(const Column &col, int64_t &start, int64_t &inc) -> bool {
    if (!col[0].has_value() || !col[1].has_value()) {
      return false;
    }
    start = *col[0];
    inc = static_cast<int64_t>(*col[1]) - start;
    for (idx_t i = 0; i < N; i++) {
      if (!col[i].has_value() || *col[i] != static_cast<int32_t>(start + static_cast<int64_t>(i) * inc)) {
        return false;
      }
    }
    return true;
  }

  /**
   * @return The names of every encoding that can represent `col`. FLAT is always first.
   *
   * Names, not Vectors, because a Vector must be built FRESH for each pairing: reading a
   * SEQUENCE (or a DICTIONARY) through Orrify() normalifies it in place, so reusing one
   * across pairings would quietly turn it into a FLAT vector and the test would stop
   * exercising the encoding it claims to.
   */
  static auto EncodingNames(const Column &col) -> std::vector<std::string> {
    std::vector<std::string> out = {"FLAT", "DICTIONARY"};
    if (IsUniform(col)) {
      out.emplace_back("CONSTANT");
    }
    int64_t start;
    int64_t inc;
    if (IsArithmetic(col, start, inc)) {
      out.emplace_back("SEQUENCE");
    }
    return out;
  }

  /** @brief Build `col` in the named encoding. */
  auto Make(const std::string &encoding, const Column &col) -> Vector * {
    if (encoding == "FLAT") {
      return MakeFlat(col);
    }
    if (encoding == "DICTIONARY") {
      return MakeDict(col);
    }
    if (encoding == "CONSTANT") {
      return MakeConstant(col);
    }
    int64_t start;
    int64_t inc;
    EXPECT_TRUE(IsArithmetic(col, start, inc));
    return MakeSequence(start, inc);
  }

  /** @return The rows a comparison selected, sorted, so that two runs compare as sets. */
  static auto TrueRows(idx_t true_count, const SelectionVector &sel) -> std::vector<idx_t> {
    std::vector<idx_t> out;
    out.reserve(true_count);
    for (idx_t i = 0; i < true_count; i++) {
      out.push_back(sel.GetIndex(i));
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  using CompareFn = idx_t (*)(Vector &, Vector &, const SelectionVector *, idx_t, SelectionVector *);

  /** @brief Run `fn` over every encoding pairing of `a` and `b`; all must match flat/flat. */
  void CheckComparisonAgrees(const char *op_name, CompareFn fn, const Column &a, const Column &b) {
    auto names_a = EncodingNames(a);
    auto names_b = EncodingNames(b);

    // The baseline: flat OP flat, which every other pairing has to reproduce.
    SelectionVector base_sel(N);
    idx_t base_count = fn(*Make("FLAT", a), *Make("FLAT", b), nullptr, N, &base_sel);
    auto baseline = TrueRows(base_count, base_sel);

    for (const auto &na : names_a) {
      for (const auto &nb : names_b) {
        // Fresh vectors per pairing: Orrify() normalifies in place.
        Vector *va = Make(na, a);
        Vector *vb = Make(nb, b);
        SelectionVector sel(N);
        idx_t count = fn(*va, *vb, nullptr, N, &sel);
        auto got = TrueRows(count, sel);
        EXPECT_EQ(got, baseline) << op_name << "(" << na << ", " << nb << ") disagrees with " << op_name
                                 << "(FLAT, FLAT)";
      }
    }
  }

  using ArithFn = void (*)(Vector &, Vector &, Vector &, idx_t);

  /** @brief Run `fn` over every encoding pairing; the values AND the nulls must match. */
  void CheckArithAgrees(const char *op_name, ArithFn fn, const Column &a, const Column &b) {
    auto names_a = EncodingNames(a);
    auto names_b = EncodingNames(b);

    Vector base_result(PhysicalType::INTEGER, N);
    fn(*Make("FLAT", a), *Make("FLAT", b), base_result, N);
    std::vector<std::optional<int32_t>> baseline(N);
    for (idx_t i = 0; i < N; i++) {
      baseline[i] = base_result.RowIsValid(i) ? std::optional<int32_t>(base_result.GetValue(i).GetAs<int32_t>())
                                              : std::nullopt;
    }

    for (const auto &na : names_a) {
      for (const auto &nb : names_b) {
        Vector *va = Make(na, a);
        Vector *vb = Make(nb, b);
        Vector result(PhysicalType::INTEGER, N);
        fn(*va, *vb, result, N);
        for (idx_t i = 0; i < N; i++) {
          bool valid = result.RowIsValid(i);
          EXPECT_EQ(valid, baseline[i].has_value())
              << op_name << "(" << na << ", " << nb << ") row " << i << ": nullness disagrees with FLAT/FLAT";
          if (valid && baseline[i].has_value()) {
            EXPECT_EQ(result.GetValue(i).GetAs<int32_t>(), *baseline[i])
                << op_name << "(" << na << ", " << nb << ") row " << i << ": value disagrees with FLAT/FLAT";
          }
        }
      }
    }
  }

  // The datasets. Between them they cover every encoding: `kRun` is an arithmetic run (so it
  // is also a SEQUENCE), `kUniform` is all-one-value (so it is also a CONSTANT), `kNulls`
  // carries NULLs mid-column, and `kAllNull` is a NULL constant.
  static auto RunColumn() -> Column { return {1, 2, 3, 4, 5, 6, 7, 8}; }
  static auto UniformColumn() -> Column { return {5, 5, 5, 5, 5, 5, 5, 5}; }
  static auto NullsColumn() -> Column {
    return {1, std::nullopt, 3, 4, std::nullopt, 6, 7, std::nullopt};
  }
  static auto AllNullColumn() -> Column {
    return {std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, std::nullopt};
  }
  static auto ShuffledColumn() -> Column { return {5, 1, 8, 5, 3, 5, 2, 8}; }

  /** @return Every dataset, with a name for the failure message. */
  static auto AllColumns() -> std::vector<std::pair<std::string, Column>> {
    return {{"run", RunColumn()},
            {"uniform", UniformColumn()},
            {"nulls", NullsColumn()},
            {"all_null", AllNullColumn()},
            {"shuffled", ShuffledColumn()}};
  }

  /** @brief Run a comparison kernel over every dataset pair AND every encoding pair. */
  void CheckComparisonOverAllData(const char *op_name, CompareFn fn) {
    for (const auto &[name_a, col_a] : AllColumns()) {
      for (const auto &[name_b, col_b] : AllColumns()) {
        SCOPED_TRACE(std::string(op_name) + ": " + name_a + " vs " + name_b);
        owned_.clear();  // the previous pair's vectors are done with
        CheckComparisonAgrees(op_name, fn, col_a, col_b);
      }
    }
  }

  /** @brief Run an arithmetic kernel over every dataset pair AND every encoding pair. */
  void CheckArithOverAllData(const char *op_name, ArithFn fn) {
    for (const auto &[name_a, col_a] : AllColumns()) {
      for (const auto &[name_b, col_b] : AllColumns()) {
        SCOPED_TRACE(std::string(op_name) + ": " + name_a + " vs " + name_b);
        owned_.clear();
        CheckArithAgrees(op_name, fn, col_a, col_b);
      }
    }
  }

  using UnaryArithFn = void (*)(Vector &, Vector &, idx_t);

  /** @brief Run a unary kernel over every encoding of `a`; the values AND nulls must match. */
  void CheckUnaryArithOverAllData(const char *op_name, UnaryArithFn fn) {
    for (const auto &[name_a, col_a] : AllColumns()) {
      SCOPED_TRACE(std::string(op_name) + ": " + name_a);
      owned_.clear();

      Vector base_result(PhysicalType::INTEGER, N);
      fn(*Make("FLAT", col_a), base_result, N);
      std::vector<std::optional<int32_t>> baseline(N);
      for (idx_t i = 0; i < N; i++) {
        baseline[i] = base_result.RowIsValid(i) ? std::optional<int32_t>(base_result.GetValue(i).GetAs<int32_t>())
                                                : std::nullopt;
      }

      for (const auto &na : EncodingNames(col_a)) {
        Vector *va = Make(na, col_a);
        Vector result(PhysicalType::INTEGER, N);
        fn(*va, result, N);
        for (idx_t i = 0; i < N; i++) {
          bool valid = result.RowIsValid(i);
          EXPECT_EQ(valid, baseline[i].has_value())
              << op_name << "(" << na << ") row " << i << ": nullness disagrees with FLAT";
          if (valid && baseline[i].has_value()) {
            EXPECT_EQ(result.GetValue(i).GetAs<int32_t>(), *baseline[i])
                << op_name << "(" << na << ") row " << i << ": value disagrees with FLAT";
          }
        }
      }
    }
  }
};

TEST_F(EncodingAgreementTest, EqualsAgreesAcrossEncodings) {
  CheckComparisonOverAllData("Equals", [](Vector &l, Vector &r, const SelectionVector *s, idx_t c,
                                          SelectionVector *t) { return VectorOperations::Equals(l, r, s, c, t); });
}

TEST_F(EncodingAgreementTest, GreaterThanAgreesAcrossEncodings) {
  CheckComparisonOverAllData("GreaterThan", [](Vector &l, Vector &r, const SelectionVector *s, idx_t c,
                                               SelectionVector *t) {
    return VectorOperations::GreaterThan(l, r, s, c, t);
  });
}

TEST_F(EncodingAgreementTest, LessThanAgreesAcrossEncodings) {
  CheckComparisonOverAllData("LessThan", [](Vector &l, Vector &r, const SelectionVector *s, idx_t c,
                                            SelectionVector *t) { return VectorOperations::LessThan(l, r, s, c, t); });
}

TEST_F(EncodingAgreementTest, NotDistinctFromAgreesAcrossEncodings) {
  CheckComparisonOverAllData("NotDistinctFrom", [](Vector &l, Vector &r, const SelectionVector *s, idx_t c,
                                                   SelectionVector *t) {
    return VectorOperations::NotDistinctFrom(l, r, s, c, t);
  });
}

TEST_F(EncodingAgreementTest, LessThanEqualsAgreesAcrossEncodings) {
  CheckComparisonOverAllData("LessThanEquals", [](Vector &l, Vector &r, const SelectionVector *s, idx_t c,
                                                  SelectionVector *t) {
    return VectorOperations::LessThanEquals(l, r, s, c, t);
  });
}

TEST_F(EncodingAgreementTest, GreaterThanEqualsAgreesAcrossEncodings) {
  CheckComparisonOverAllData("GreaterThanEquals", [](Vector &l, Vector &r, const SelectionVector *s, idx_t c,
                                                     SelectionVector *t) {
    return VectorOperations::GreaterThanEquals(l, r, s, c, t);
  });
}

TEST_F(EncodingAgreementTest, NotEqualsAgreesAcrossEncodings) {
  CheckComparisonOverAllData("NotEquals", [](Vector &l, Vector &r, const SelectionVector *s, idx_t c,
                                             SelectionVector *t) {
    return VectorOperations::NotEquals(l, r, s, c, t);
  });
}

TEST_F(EncodingAgreementTest, SumAgreesAcrossEncodings) {
  CheckArithOverAllData(
      "Sum", [](Vector &l, Vector &r, Vector &res, idx_t c) { VectorOperations::Sum(l, r, res, c); });
}

TEST_F(EncodingAgreementTest, DifferenceAgreesAcrossEncodings) {
  CheckArithOverAllData("Difference",
                        [](Vector &l, Vector &r, Vector &res, idx_t c) { VectorOperations::Difference(l, r, res, c); });
}

TEST_F(EncodingAgreementTest, DotAgreesAcrossEncodings) {
  CheckArithOverAllData(
      "Dot", [](Vector &l, Vector &r, Vector &res, idx_t c) { VectorOperations::Dot(l, r, res, c); });
}

// The datasets hold no zero, so Division / Modulo never hit the divide-by-zero guard here —
// this pins that the plain quotient / remainder is identical whatever the operands' encoding.
TEST_F(EncodingAgreementTest, DivisionAgreesAcrossEncodings) {
  CheckArithOverAllData("Division",
                        [](Vector &l, Vector &r, Vector &res, idx_t c) { VectorOperations::Division(l, r, res, c); });
}

TEST_F(EncodingAgreementTest, ModuloAgreesAcrossEncodings) {
  CheckArithOverAllData(
      "Modulo", [](Vector &l, Vector &r, Vector &res, idx_t c) { VectorOperations::Modulo(l, r, res, c); });
}

TEST_F(EncodingAgreementTest, NegateAgreesAcrossEncodings) {
  CheckUnaryArithOverAllData("Negate",
                             [](Vector &in, Vector &res, idx_t c) { VectorOperations::Negate(in, res, c); });
}

// The NULL-aware comparators are the ones DISTINCT / GROUP BY / dedup rely on, and they are
// the pair most likely to drift apart across encodings. Whatever the encoding, they must
// stay exact complements: every row is either NOT DISTINCT or DISTINCT, never both, never
// neither — including the rows where both sides are NULL.
TEST_F(EncodingAgreementTest, DistinctFromIsTheComplementOfNotDistinctFromInEveryEncoding) {
  for (const auto &[name_a, col_a] : AllColumns()) {
    for (const auto &[name_b, col_b] : AllColumns()) {
      owned_.clear();
      for (const auto &na : EncodingNames(col_a)) {
        for (const auto &nb : EncodingNames(col_b)) {
          SCOPED_TRACE(name_a + " (" + na + ") vs " + name_b + " (" + nb + ")");
          // NotDistinctFrom and DistinctFrom each read their inputs, so each needs its own.
          SelectionVector nd_sel(N);
          idx_t nd = VectorOperations::NotDistinctFrom(*Make(na, col_a), *Make(nb, col_b), nullptr, N, &nd_sel);
          SelectionVector d_sel(N);
          idx_t d = VectorOperations::DistinctFrom(*Make(na, col_a), *Make(nb, col_b), nullptr, N, &d_sel);

          EXPECT_EQ(nd + d, N) << "every row must be exactly one of NOT DISTINCT / DISTINCT";

          auto nd_rows = TrueRows(nd, nd_sel);
          auto d_rows = TrueRows(d, d_sel);
          std::vector<idx_t> together;
          std::set_union(nd_rows.begin(), nd_rows.end(), d_rows.begin(), d_rows.end(),
                         std::back_inserter(together));
          EXPECT_EQ(together.size(), N) << "the two selections must partition the rows";
        }
      }
    }
  }
}

// A NULL key must never match anything — not even another NULL. That is what keeps a NULL
// join key from joining, and it has to hold whatever encoding the key column arrives in.
TEST_F(EncodingAgreementTest, NullKeyNeverEqualsAnythingInAnyEncoding) {
  for (const auto &[name_b, col_b] : AllColumns()) {
    owned_.clear();
    Column all_null = AllNullColumn();
    for (const auto &na : EncodingNames(all_null)) {
      for (const auto &nb : EncodingNames(col_b)) {
        SCOPED_TRACE("all_null (" + na + ") vs " + name_b + " (" + nb + ")");
        SelectionVector sel(N);
        idx_t matched = VectorOperations::Equals(*Make(na, all_null), *Make(nb, col_b), nullptr, N, &sel);
        EXPECT_EQ(matched, 0U) << "a NULL key matched under Equals";
      }
    }
  }
}

// A guard on the harness itself: if the encoding factory silently stopped producing the
// CONSTANT or SEQUENCE variants, every test above would still pass — while covering nothing
// but FLAT and DICTIONARY. Pin down what each dataset is expected to produce, and check that
// each vector really carries the encoding its name claims.
TEST_F(EncodingAgreementTest, HarnessCoversEveryEncoding) {
  EXPECT_EQ(EncodingNames(RunColumn()), (std::vector<std::string>{"FLAT", "DICTIONARY", "SEQUENCE"}));
  // A uniform column is also an arithmetic run, of increment 0 — so it encodes four ways.
  EXPECT_EQ(EncodingNames(UniformColumn()),
            (std::vector<std::string>{"FLAT", "DICTIONARY", "CONSTANT", "SEQUENCE"}));
  EXPECT_EQ(EncodingNames(NullsColumn()), (std::vector<std::string>{"FLAT", "DICTIONARY"}));
  EXPECT_EQ(EncodingNames(AllNullColumn()), (std::vector<std::string>{"FLAT", "DICTIONARY", "CONSTANT"}));

  EXPECT_EQ(Make("FLAT", RunColumn())->GetVectorType(), VectorType::FLAT_VECTOR);
  EXPECT_EQ(Make("DICTIONARY", RunColumn())->GetVectorType(), VectorType::DICTIONARY_VECTOR);
  EXPECT_EQ(Make("SEQUENCE", RunColumn())->GetVectorType(), VectorType::SEQUENCE_VECTOR);
  EXPECT_EQ(Make("CONSTANT", UniformColumn())->GetVectorType(), VectorType::CONSTANT_VECTOR);
  EXPECT_EQ(Make("CONSTANT", AllNullColumn())->GetVectorType(), VectorType::CONSTANT_VECTOR);

  // And every encoding really does hold the logical column it was built from.
  Column col = NullsColumn();
  for (const auto &name : EncodingNames(col)) {
    Vector *v = Make(name, col);
    for (idx_t i = 0; i < N; i++) {
      SCOPED_TRACE(name + " row " + std::to_string(i));
      EXPECT_EQ(v->RowIsValid(i), col[i].has_value());
      if (col[i].has_value()) {
        EXPECT_EQ(v->GetValue(i).GetAs<int32_t>(), *col[i]);
      }
    }
  }
  // The SEQUENCE variant too, which only the no-null datasets have.
  Column run = RunColumn();
  Vector *seq = Make("SEQUENCE", run);
  for (idx_t i = 0; i < N; i++) {
    EXPECT_EQ(seq->GetValue(i).GetAs<int32_t>(), *run[i]);
  }
}

}  // namespace bumblebee
