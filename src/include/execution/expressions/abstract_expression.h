//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// abstract_expression.h
//
// Identification: src/include/execution/expressions/abstract_expression.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog/column.h"
#include "catalog/schema.h"
#include "fmt/format.h"

/** Give `cname` a CloneWithChildren() that copies it and swaps in new children. */
#define BUMBLEBEE_EXPR_CLONE_WITH_CHILDREN(cname)                                                                \
  auto CloneWithChildren(std::vector<AbstractExpressionRef> children) const->std::unique_ptr<AbstractExpression> \
      override {                                                                                                 \
    auto expr = cname(*this);                                                                                    \
    expr.children_ = children;                                                                                   \
    return std::make_unique<cname>(std::move(expr));                                                             \
  }

namespace bumblebee {

class AbstractExpression;
using AbstractExpressionRef = std::shared_ptr<AbstractExpression>;

/**
 * The base class of every expression in the system. Expressions are trees, so an
 * expression may have any number of children, and their order matters.
 *
 * Note what is NOT here: there is no Evaluate(). An expression tree is built by
 * the binder and the planner, rewritten by the optimizer, and printed — none of
 * which requires evaluating it. The evaluation entry point is deliberately left
 * to the execution milestone, because BumbleBee's engine is push-based and
 * vectorized: it will want to evaluate an expression over a whole batch of rows
 * at once, e.g.
 *
 *   virtual void EvaluateBatch(const RowBatch &input, ValueVector &out) const;
 *
 * rather than one tuple at a time. Committing to a tuple-at-a-time signature now
 * would mean writing it only to delete it, and it would drag the storage layer
 * (Tuple, RID) into the frontend for no benefit.
 */
class AbstractExpression {
 public:
  /**
   * @brief Construct an expression with the given children and return type.
   *
   * @param children The children of this expression.
   * @param ret_type The type this expression evaluates to.
   */
  AbstractExpression(std::vector<AbstractExpressionRef> children, Column ret_type)
      : children_{std::move(children)}, ret_type_{std::move(ret_type)} {}

  virtual ~AbstractExpression() = default;

  /**
   * @brief Get the child at `child_idx`.
   *
   * @param child_idx The child index.
   * @return const AbstractExpressionRef& The child.
   */
  auto GetChildAt(uint32_t child_idx) const -> const AbstractExpressionRef & { return children_[child_idx]; }

  /** @return The children of this expression. Their order may matter. */
  auto GetChildren() const -> const std::vector<AbstractExpressionRef> & { return children_; }

  /** @return The type this expression evaluates to. */
  virtual auto GetReturnType() const -> Column { return ret_type_; }

  /** @return A string rendering of this expression and its children. */
  virtual auto ToString() const -> std::string { return "<unknown>"; }

  /**
   * @brief Copy this expression with a new set of children.
   *
   * @param children The new children.
   * @return std::unique_ptr<AbstractExpression> The copy.
   */
  virtual auto CloneWithChildren(std::vector<AbstractExpressionRef> children) const
      -> std::unique_ptr<AbstractExpression> = 0;

  /** The children of this expression. Their order may matter. */
  std::vector<AbstractExpressionRef> children_;

 private:
  /** The type this expression evaluates to. */
  Column ret_type_;
};

}  // namespace bumblebee

template <typename T>
struct fmt::formatter<T, std::enable_if_t<std::is_base_of<bumblebee::AbstractExpression, T>::value, char>>
    : fmt::formatter<std::string> {
  template <typename FormatCtx>
  auto format(const T &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x.ToString(), ctx);
  }
};

template <typename T>
struct fmt::formatter<std::shared_ptr<T>,
                      std::enable_if_t<std::is_base_of<bumblebee::AbstractExpression, T>::value, char>>
    : fmt::formatter<std::string> {
  template <typename FormatCtx>
  auto format(const std::shared_ptr<T> &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x == nullptr ? "<nullptr>" : x->ToString(), ctx);
  }
};
