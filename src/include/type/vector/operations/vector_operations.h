//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// vector_operations.h
//
// Identification: src/include/type/vector/operations/vector_operations.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

#include "type/vector/vector.h"

namespace bumblebee {

/**
 * The kernels: the operations that read and write whole Vectors.
 *
 * Everything here loops over rows without allocating and without per-row type dispatch:
 * the type switch happens once, at the top, and the loop it selects is a monomorphic
 * template. That is what makes the engine vectorized rather than tuple-at-a-time.
 *
 * NULL is handled uniformly by the ValidityMask, never by a sentinel value. The two
 * families of NULL semantics are both here and they are NOT interchangeable:
 *  - Equals / NotEquals are SQL `=` / `!=`: a NULL operand makes the comparison UNKNOWN,
 *    which is never TRUE, so the row is excluded from `true_sel`. That is what makes a
 *    NULL join key never match anything, including another NULL.
 *  - NotDistinctFrom / DistinctFrom are SQL `IS [NOT] DISTINCT FROM`: two NULLs ARE equal;
 *    a NULL and a non-NULL are not. That is what DISTINCT, GROUP BY and dedup need.
 */
class VectorOperations {
 public:
  // -- Comparison -----------------------------------------------------------
  //
  // Each comparison comes in two overloads: the 5-argument one only reports the matching
  // rows; the 7-argument one also reports the non-matching rows, which an OR-eval needs so
  // it can hand them to the next branch. In both, a NULL operand makes the row UNKNOWN —
  // not TRUE — so it lands in `false_sel`, and `count == true_count + false_count` holds.

  /**
   * @brief Select the rows where `left == right`.
   *
   * @param left The left input.
   * @param right The right input.
   * @param sel The rows to consider. Null means all of them.
   * @param count The number of rows.
   * @param true_sel Out: the matching rows. May be null to only count them.
   * @return idx_t The number of matching rows.
   */
  static auto Equals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count, SelectionVector *true_sel)
      -> idx_t;

  /**
   * @brief Select the rows where `left == right`, reporting the non-matching rows too.
   *
   * @param false_sel Out: the non-matching rows (which includes the UNKNOWN ones).
   * @param false_count Out: the number of non-matching rows.
   */
  static auto Equals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                     SelectionVector *false_sel, idx_t &false_count) -> idx_t;

  /** @brief Select the rows where `left != right`. */
  static auto NotEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count, SelectionVector *true_sel)
      -> idx_t;
  static auto NotEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                        SelectionVector *false_sel, idx_t &false_count) -> idx_t;

  /** @brief Select the rows where `left > right`. */
  static auto GreaterThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                          SelectionVector *true_sel) -> idx_t;
  static auto GreaterThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                          SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t;

  /** @brief Select the rows where `left >= right`. */
  static auto GreaterThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                SelectionVector *true_sel) -> idx_t;
  static auto GreaterThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t;

  /** @brief Select the rows where `left < right`. */
  static auto LessThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count, SelectionVector *true_sel)
      -> idx_t;
  static auto LessThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                       SelectionVector *false_sel, idx_t &false_count) -> idx_t;

  /** @brief Select the rows where `left <= right`. */
  static auto LessThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                             SelectionVector *true_sel) -> idx_t;
  static auto LessThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                             SelectionVector *true_sel, SelectionVector *false_sel, idx_t &false_count) -> idx_t;

  // -- NULL-aware comparison ------------------------------------------------

  /**
   * @brief Select the rows where `left IS NOT DISTINCT FROM right`.
   *
   * Unlike Equals, two NULLs match here. A NULL and a non-NULL do not.
   */
  static auto NotDistinctFrom(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                              SelectionVector *true_sel) -> idx_t;

  /** @brief Select the rows where `left IS DISTINCT FROM right`: the inverse of the above. */
  static auto DistinctFrom(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                           SelectionVector *true_sel) -> idx_t;

  // -- NULL predicates ------------------------------------------------------
  // Pure mask reads, independent of the value stored in the row.

  /** @brief Select the rows of `input` that are NULL. */
  static auto IsNull(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel) -> idx_t;
  static auto IsNull(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                     SelectionVector *false_sel, idx_t &false_count) -> idx_t;

  /** @brief Select the rows of `input` that are NOT NULL. */
  static auto IsNotNull(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel) -> idx_t;
  static auto IsNotNull(Vector &input, const SelectionVector *sel, idx_t count, SelectionVector *true_sel,
                        SelectionVector *false_sel, idx_t &false_count) -> idx_t;

  // -- Arithmetic -----------------------------------------------------------
  //
  // The result type is whatever `result` was created with: it drives the arithmetic, so
  // the caller decides the promotion. A NULL on either input yields a NULL result row.

  /** @brief `result = left + right`. */
  static void Sum(Vector &left, Vector &right, Vector &result, idx_t count);

  /** @brief `result = left * right`. */
  static void Dot(Vector &left, Vector &right, Vector &result, idx_t count);

  /** @brief `result = left / right`. */
  static void Division(Vector &left, Vector &right, Vector &result, idx_t count);

  /** @brief `result = left - right`. */
  static void Difference(Vector &left, Vector &right, Vector &result, idx_t count);

  /** @brief `result = -input`. */
  static void Negate(Vector &input, Vector &result, idx_t count);

  /** @brief `result = left % right`. */
  static void Modulo(Vector &left, Vector &right, Vector &result, idx_t count);

  /** @brief `result = left & right`: the bitwise AND. */
  static void LAnd(Vector &left, Vector &right, Vector &result, idx_t count);

  // -- Hashing --------------------------------------------------------------

  /** @brief Hash every row of `input` into `hashes` (a UBIGINT vector). */
  static void Hash(Vector &input, Vector &hashes, idx_t count);

  /** @brief Hash only the rows named by `rsel`, writing each at its own index. */
  static void Hash(Vector &input, Vector &hashes, const SelectionVector &rsel, idx_t count);

  /** @brief Fold the hash of every row of `input` into the existing `hashes`. */
  static void CombineHash(Vector &hashes, Vector &input, idx_t count);

  /** @brief Fold in only the rows named by `rsel`. */
  static void CombineHash(Vector &hashes, Vector &input, const SelectionVector &rsel, idx_t count);

  // -- Casting --------------------------------------------------------------

  /**
   * @brief Cast `source` into `target`, whose type is the one to cast to.
   *
   * A cast that overflows or fails to parse turns the row NULL. Use TryCast to learn that
   * it happened.
   */
  static void Cast(Vector &source, Vector &target, idx_t source_count);

  /**
   * @brief Cast `source` into `target`, reporting whether every row converted.
   *
   * @param error_message Out: the first error, if any. May be null.
   * @return True if every row converted.
   */
  [[nodiscard]] static auto TryCast(Vector &source, Vector &target, idx_t source_count, std::string *error_message)
      -> bool;

  // -- Sequence generation --------------------------------------------------

  /** @brief Fill `result` with `count` rows of start, start + increment, ... */
  static void GenerateSequence(Vector &result, idx_t count, int64_t start, int64_t increment);

  /** @brief Fill `result` with `count` rows of the circular sequence over [start, end]. */
  static void GenerateSequence(Vector &result, idx_t count, int64_t start, int64_t offset, int64_t stride, int64_t end);

  /** @brief Fill only the rows named by `sel`, each with the value the sequence has *at that row*. */
  static void GenerateSequence(Vector &result, idx_t count, const SelectionVector &sel, int64_t start,
                               int64_t increment);

  /** @brief Fill only the rows named by `sel` with the circular sequence over [start, end]. */
  static void GenerateSequence(Vector &result, idx_t count, const SelectionVector &sel, int64_t start, int64_t offset,
                               int64_t stride, int64_t end);

  // -- Copy -----------------------------------------------------------------

  /**
   * @brief Copy rows [source_offset, source_count) of `source` into `target` at `target_offset`.
   *
   * `source` may have any encoding; `target` must be FLAT (a CONSTANT target is accepted
   * when a single row is copied). NULLs are carried over.
   *
   * @param source The vector to read.
   * @param target The FLAT vector to write.
   * @param source_count The end of the source range (not a length).
   * @param source_offset The first source row to copy.
   * @param target_offset The first target row to write.
   */
  static void Copy(const Vector &source, Vector &target, idx_t source_count, idx_t source_offset, idx_t target_offset);

  /** @brief Copy through a source selection: source row i is `sel[i]`. */
  static void Copy(const Vector &source, Vector &target, const SelectionVector &sel, idx_t source_count,
                   idx_t source_offset, idx_t target_offset);

  /** @brief Copy through a source selection, scattering into `target` through `target_sel`. */
  static void Copy(const Vector &source, Vector &target, const SelectionVector &sel, const SelectionVector *target_sel,
                   idx_t source_count, idx_t source_offset, idx_t target_offset);
};

}  // namespace bumblebee
