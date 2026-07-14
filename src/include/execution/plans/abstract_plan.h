//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// abstract_plan.h
//
// Identification: src/include/execution/plans/abstract_plan.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "catalog/schema.h"
#include "fmt/format.h"

namespace bumblebee {

/** Give `cname` a CloneWithChildren() that copies it and swaps in new children. */
#define BUMBLEBEE_PLAN_NODE_CLONE_WITH_CHILDREN(cname)                                                       \
  auto CloneWithChildren(std::vector<AbstractPlanNodeRef> children) const->std::unique_ptr<AbstractPlanNode> \
      override {                                                                                             \
    auto plan_node = cname(*this);                                                                           \
    plan_node.children_ = children;                                                                          \
    return std::make_unique<cname>(std::move(plan_node));                                                    \
  }

/** The kinds of plan node in the system. */
enum class PlanType {
  SeqScan,
  Insert,
  Update,
  Delete,
  Aggregation,
  Limit,
  NestedLoopJoin,
  HashJoin,
  Filter,
  Values,
  Projection,
  Sort,
  TopN,
};

class AbstractPlanNode;
using AbstractPlanNodeRef = std::shared_ptr<const AbstractPlanNode>;

/**
 * The base class of every plan node.
 *
 * A plan node is a purely declarative description of *what* an operator produces:
 * its output schema and its children. It says nothing about *how* the operator is
 * run — there is no Init()/Next() here — so this tree is equally valid for a
 * pull-based Volcano engine and for BumbleBee's push-based vectorized one. A later
 * milestone lowers this tree into physical operators by pattern-matching on
 * PlanType; keep it that way and do not smuggle execution strategy in here.
 *
 * The ordering of a node's children matters (e.g. the build and probe sides of a
 * join).
 */
class AbstractPlanNode {
 public:
  /**
   * @brief Construct a plan node.
   *
   * @param output_schema The schema of the tuples this node produces.
   * @param children The children of this node.
   */
  AbstractPlanNode(SchemaRef output_schema, std::vector<AbstractPlanNodeRef> children)
      : output_schema_(std::move(output_schema)), children_(std::move(children)) {}

  virtual ~AbstractPlanNode() = default;

  /** @return The schema of the tuples this node produces. */
  auto OutputSchema() const -> const Schema & { return *output_schema_; }

  /**
   * @brief Get the child at `child_idx`.
   *
   * @param child_idx The child index.
   * @return AbstractPlanNodeRef The child.
   */
  auto GetChildAt(uint32_t child_idx) const -> AbstractPlanNodeRef { return children_[child_idx]; }

  /** @return The children of this node. Their order matters. */
  auto GetChildren() const -> const std::vector<AbstractPlanNodeRef> & { return children_; }

  /** @return The type of this node. */
  virtual auto GetType() const -> PlanType = 0;

  /**
   * @brief Render this node and its children as an indented tree.
   *
   * @param with_schema When true, print each node's output schema alongside it.
   * @return std::string The rendering.
   */
  auto ToString(bool with_schema = true) const -> std::string {
    if (with_schema) {
      return fmt::format("{} | {}{}", PlanNodeToString(), output_schema_, ChildrenToString(2, with_schema));
    }
    return fmt::format("{}{}", PlanNodeToString(), ChildrenToString(2, with_schema));
  }

  /**
   * @brief Copy this node with a new set of children.
   *
   * @param children The new children.
   * @return std::unique_ptr<AbstractPlanNode> The copy.
   */
  virtual auto CloneWithChildren(std::vector<AbstractPlanNodeRef> children) const
      -> std::unique_ptr<AbstractPlanNode> = 0;

  /** The schema of the tuples this node produces. */
  SchemaRef output_schema_;

  /** The children of this node. */
  std::vector<AbstractPlanNodeRef> children_;

 protected:
  /** @return A string rendering of this node alone, without its children. */
  virtual auto PlanNodeToString() const -> std::string { return "<unknown>"; }

  /** @return A string rendering of this node's children, indented. */
  auto ChildrenToString(int indent, bool with_schema = true) const -> std::string;
};

}  // namespace bumblebee

template <typename T>
struct fmt::formatter<T, std::enable_if_t<std::is_base_of<bumblebee::AbstractPlanNode, T>::value, char>>
    : fmt::formatter<std::string> {
  template <typename FormatCtx>
  auto format(const T &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x.ToString(), ctx);
  }
};

template <typename T>
struct fmt::formatter<std::unique_ptr<T>,
                      std::enable_if_t<std::is_base_of<bumblebee::AbstractPlanNode, T>::value, char>>
    : fmt::formatter<std::string> {
  template <typename FormatCtx>
  auto format(const std::unique_ptr<T> &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x->ToString(), ctx);
  }
};

template <typename T>
struct fmt::formatter<std::shared_ptr<T>,
                      std::enable_if_t<std::is_base_of<bumblebee::AbstractPlanNode, T>::value, char>>
    : fmt::formatter<std::string> {
  template <typename FormatCtx>
  auto format(const std::shared_ptr<T> &x, FormatCtx &ctx) const {
    return fmt::formatter<std::string>::format(x == nullptr ? "<nullptr>" : x->ToString(), ctx);
  }
};
