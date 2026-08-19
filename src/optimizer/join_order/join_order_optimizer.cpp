//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// join_order_optimizer.cpp
//
// Identification: src/optimizer/join_order/join_order_optimizer.cpp
//
// Copyright (C) 2026 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include <functional>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

#include "catalog/schema.h"
#include "common/macros.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/comparison_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/logic_expression.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/projection_plan.h"
#include "execution/plans/seq_scan_plan.h"
#include "optimizer/join_order/join_graph.h"
#include "optimizer/optimizer.h"

namespace bumblebee {

/** @brief Flatten an AND-tree of predicates into its conjuncts (a non-AND expr is one conjunct). */
static void FlattenConjuncts(const AbstractExpressionRef &expr, std::vector<AbstractExpressionRef> &out) {
  if (const auto *logic = dynamic_cast<const LogicExpression *>(expr.get());
      logic != nullptr && logic->logic_type_ == LogicType::And) {
    FlattenConjuncts(logic->GetChildAt(0), out);
    FlattenConjuncts(logic->GetChildAt(1), out);
    return;
  }
  out.push_back(expr);
}

/** @brief Gather every flat column index an expression references. Returns false if any reference is
 * not tuple 0 (i.e. the expression is not in the region's flat frame — bail out of reordering). */
static auto CollectFlatColumns(const AbstractExpressionRef &expr, std::vector<uint32_t> &cols) -> bool {
  if (const auto *col = dynamic_cast<const ColumnValueExpression *>(expr.get()); col != nullptr) {
    if (col->GetTupleIdx() != 0) {
      return false;
    }
    cols.push_back(col->GetColIdx());
    return true;
  }
  for (const auto &child : expr->GetChildren()) {
    if (!CollectFlatColumns(child, cols)) {
      return false;
    }
  }
  return true;
}

/** @brief Clone `expr`, rewriting each flat `#0.f` column reference via `remap(f) -> (tuple, col)`. */
static auto Reindex(const AbstractExpressionRef &expr,
                    const std::function<std::pair<uint32_t, uint32_t>(uint32_t)> &remap) -> AbstractExpressionRef {
  if (const auto *col = dynamic_cast<const ColumnValueExpression *>(expr.get()); col != nullptr) {
    const auto [tuple, idx] = remap(col->GetColIdx());
    return std::make_shared<ColumnValueExpression>(tuple, idx, col->GetReturnType());
  }
  std::vector<AbstractExpressionRef> children;
  children.reserve(expr->GetChildren().size());
  for (const auto &child : expr->GetChildren()) {
    children.push_back(Reindex(child, remap));
  }
  return expr->CloneWithChildren(children);
}

/** @brief Conjoin a list of predicates with AND (empty ⇒ nullptr). */
static auto ConjoinAll(std::vector<AbstractExpressionRef> preds) -> AbstractExpressionRef {
  if (preds.empty()) {
    return nullptr;
  }
  auto result = preds[0];
  for (idx_t i = 1; i < preds.size(); i++) {
    result = std::make_shared<LogicExpression>(result, preds[i], LogicType::And);
  }
  return result;
}

/** @brief Is `p` a comparison `<column> = <column>` between two DIFFERENT leaves (an equi edge)? */
static auto IsEquiEdge(const AbstractExpressionRef &pred, const std::vector<uint32_t> &cols) -> bool {
  const auto *cmp = dynamic_cast<const ComparisonExpression *>(pred.get());
  if (cmp == nullptr || cmp->comp_type_ != ComparisonType::Equal) {
    return false;
  }
  return cols.size() == 2 && dynamic_cast<const ColumnValueExpression *>(cmp->GetChildAt(0).get()) != nullptr &&
         dynamic_cast<const ColumnValueExpression *>(cmp->GetChildAt(1).get()) != nullptr;
}

/** @brief The plan's cost: the sum of every internal join's estimated output cardinality (C_out). */
static auto SumInternalCardinalities(const JoinTreeNode *node) -> double {
  if (node == nullptr || node->IsLeaf()) {
    return 0;
  }
  return node->cardinality_ + SumInternalCardinalities(node->left_.get()) +
         SumInternalCardinalities(node->right_.get());
}

/** @brief The cost of the planner's FROM-order left-deep join, under the same estimator. */
static auto LeftDeepCost(const JoinGraph &graph, const CardinalityEstimator &estimator) -> double {
  const auto &relations = graph.Relations();
  if (relations.size() < 2) {
    return 0;
  }
  RelationSet acc = RelationSet::Singleton(0);
  double acc_card = relations[0].cardinality_;
  double total = 0;
  for (idx_t k = 1; k < relations.size(); k++) {
    const auto leaf = RelationSet::Singleton(k);
    const double card = estimator.EstimateJoin(acc_card, relations[k].cardinality_, graph.EdgesCrossing(acc, leaf));
    total += card;
    acc = acc.Union(leaf);
    acc_card = card;
  }
  return total;
}

/** @brief A subtree emitted from a join-tree node, remembering its leaves' output-column order. */
struct Emitted {
  AbstractPlanNodeRef plan_;
  std::vector<idx_t> leaf_order_;  // leaf ids, in this subtree's output-column order
};

/** @brief The output position, within `emitted`, of the region-flat column `flat_col`. */
static auto LocalPosition(const Emitted &emitted, const std::vector<JoinRelation> &relations, uint32_t flat_col)
    -> uint32_t {
  uint32_t pos = 0;
  for (const auto leaf_id : emitted.leaf_order_) {
    const auto &rel = relations[leaf_id];
    if (flat_col >= rel.base_col_offset_ && flat_col < rel.base_col_offset_ + rel.col_count_) {
      return pos + (flat_col - static_cast<uint32_t>(rel.base_col_offset_));
    }
    pos += static_cast<uint32_t>(rel.col_count_);
  }
  BUMBLEBEE_ASSERT(false, "flat column not found in emitted subtree");
  return 0;
}

/** @brief The relation that owns region-flat column `flat_col`. */
static auto OwnerLeaf(const std::vector<JoinRelation> &relations, uint32_t flat_col) -> idx_t {
  for (const auto &rel : relations) {
    if (flat_col >= rel.base_col_offset_ && flat_col < rel.base_col_offset_ + rel.col_count_) {
      return rel.id_;
    }
  }
  BUMBLEBEE_ASSERT(false, "flat column has no owning relation");
  return 0;
}

/** @brief The concatenation of two schemas' columns (the output of joining them, left then right). */
static auto ConcatSchemas(const Schema &left, const Schema &right) -> SchemaRef {
  std::vector<Column> cols = left.GetColumns();
  for (const auto &c : right.GetColumns()) {
    cols.push_back(c);
  }
  return std::make_shared<Schema>(cols);
}

/** @brief A join edge waiting for its key-NDV annotation before entering the graph. */
struct PendingEdge {
  RelationSet refs_;
  AbstractExpressionRef pred_;
  bool is_equi_;
  uint32_t a_;  // the two equi-key flat columns (meaningful only when is_equi_)
  uint32_t b_;
};

/** @brief Collect a region's leaves in left-to-right (flat-schema) order, descending only through
 * inner cross-product joins. Anything else (outer join, formed join, aggregate, ...) is an opaque
 * leaf. */
static void CollectRegionLeaves(const AbstractPlanNodeRef &n, std::vector<AbstractPlanNodeRef> &leaves) {
  if (n->GetType() == PlanType::NestedLoopJoin) {
    const auto &nlj = dynamic_cast<const NestedLoopJoinPlanNode &>(*n);
    if (nlj.GetJoinType() == JoinType::INNER && Optimizer::IsPredicateTrue(nlj.Predicate())) {
      CollectRegionLeaves(nlj.GetLeftPlan(), leaves);
      CollectRegionLeaves(nlj.GetRightPlan(), leaves);
      return;
    }
  }
  leaves.push_back(n);
}

/**
 * @brief Split the region's WHERE conjuncts into per-leaf local filters and cross-leaf join edges.
 *
 * @return False when a conjunct is not the clean flat-frame shape the rewrite expects — the
 *         caller must then leave the whole region untouched.
 */
static auto ClassifyRegionConjuncts(const std::vector<AbstractExpressionRef> &conjuncts,
                                    const std::vector<JoinRelation> &relations,
                                    std::vector<std::vector<AbstractExpressionRef>> &single_filters,
                                    std::vector<PendingEdge> &pending) -> bool {
  for (const auto &conjunct : conjuncts) {
    std::vector<uint32_t> cols;
    if (!CollectFlatColumns(conjunct, cols)) {
      return false;
    }
    RelationSet refs;
    for (const auto c : cols) {
      refs = refs.Union(RelationSet::Singleton(OwnerLeaf(relations, c)));
    }
    if (refs.Count() <= 1) {
      // A single-table (or constant) predicate: attach it to its leaf (or the first leaf if constant).
      const idx_t leaf = cols.empty() ? 0 : OwnerLeaf(relations, cols.front());
      single_filters[leaf].push_back(conjunct);
      continue;
    }
    PendingEdge edge{refs, conjunct, IsEquiEdge(conjunct, cols), 0, 0};
    if (edge.is_equi_) {  // record the two key columns for the NDV equivalence classes
      const auto &cmp = dynamic_cast<const ComparisonExpression &>(*conjunct);
      edge.a_ = dynamic_cast<const ColumnValueExpression &>(*cmp.GetChildAt(0)).GetColIdx();
      edge.b_ = dynamic_cast<const ColumnValueExpression &>(*cmp.GetChildAt(1)).GetColIdx();
    }
    pending.push_back(std::move(edge));
  }
  return true;
}

/**
 * @brief Enter the pending edges into the graph, annotating each equi edge with its key NDV.
 *
 * Join-key NDV from row counts alone: union the flat columns joined by equality, then each
 * class's distinct count ≈ the smallest table row count in it (the PK/dimension side bounds the
 * key). This is what makes a low-cardinality key (nationkey ≈ 25) read as near-cartesian without
 * any scan.
 */
static void AddEdgesWithKeyNdv(JoinGraph &graph, const std::vector<PendingEdge> &pending,
                               const std::vector<JoinRelation> &relations, idx_t flat_width) {
  std::vector<uint32_t> uf(flat_width);
  std::iota(uf.begin(), uf.end(), 0U);
  std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
    while (uf[x] != x) {
      uf[x] = uf[uf[x]];
      x = uf[x];
    }
    return x;
  };
  for (const auto &e : pending) {
    if (e.is_equi_) {
      uf[find(e.a_)] = find(e.b_);
    }
  }
  std::unordered_map<uint32_t, double> class_ndv;
  auto note_ndv = [&](uint32_t flat) {
    const auto root = find(flat);
    const double card = relations[OwnerLeaf(relations, flat)].cardinality_;
    const auto it = class_ndv.find(root);
    class_ndv[root] = it == class_ndv.end() ? card : std::min(it->second, card);
  };
  for (const auto &e : pending) {
    if (e.is_equi_) {
      note_ndv(e.a_);
      note_ndv(e.b_);
    }
  }
  for (const auto &e : pending) {
    graph.AddEdge(e.refs_, e.pred_, e.is_equi_, e.is_equi_ ? class_ndv[find(e.a_)] : 0.0);
  }
}

/**
 * @brief Materialize the chosen join tree back into plan nodes.
 *
 * Leaves get their local filters re-indexed onto themselves; every internal node builds the
 * smaller side (child 0 — the INNER hash-join build side by convention) and re-indexes each
 * crossing predicate from the flat frame into its own #0/#1 spaces.
 */
static auto EmitJoinTree(const JoinTreeNode *tree_node, const JoinGraph &graph,
                         const std::vector<JoinRelation> &relations,
                         const std::vector<std::vector<AbstractExpressionRef>> &single_filters) -> Emitted {
  if (tree_node->IsLeaf()) {
    const auto leaf_id = tree_node->leaf_id_;
    auto leaf_plan = relations[leaf_id].plan_;
    if (!single_filters[leaf_id].empty()) {
      const auto base = relations[leaf_id].base_col_offset_;
      auto pred = Reindex(ConjoinAll(single_filters[leaf_id]),
                          [base](uint32_t f) { return std::make_pair<uint32_t, uint32_t>(0, f - base); });
      leaf_plan = std::make_shared<FilterPlanNode>(leaf_plan->output_schema_, std::move(pred), leaf_plan);
    }
    return Emitted{leaf_plan, {leaf_id}};
  }

  Emitted left = EmitJoinTree(tree_node->left_.get(), graph, relations, single_filters);
  Emitted right = EmitJoinTree(tree_node->right_.get(), graph, relations, single_filters);
  // Build the smaller side (child 0 — the INNER hash-join build side by convention).
  if (tree_node->left_->cardinality_ > tree_node->right_->cardinality_) {
    std::swap(left, right);
  }

  RelationSet left_set;
  for (const auto id : left.leaf_order_) {
    left_set = left_set.Union(RelationSet::Singleton(id));
  }
  RelationSet right_set;
  for (const auto id : right.leaf_order_) {
    right_set = right_set.Union(RelationSet::Singleton(id));
  }

  // Re-index each crossing predicate from the flat frame into this join's #0 (left) / #1 (right).
  std::vector<AbstractExpressionRef> preds;
  for (const auto *edge : graph.EdgesCrossing(left_set, right_set)) {
    preds.push_back(Reindex(edge->predicate_, [&](uint32_t f) -> std::pair<uint32_t, uint32_t> {
      const auto owner = OwnerLeaf(relations, f);
      if (left_set.Contains(owner)) {
        return {0, LocalPosition(left, relations, f)};
      }
      return {1, LocalPosition(right, relations, f)};
    }));
  }
  auto predicate = ConjoinAll(std::move(preds));
  if (predicate == nullptr) {
    predicate = std::make_shared<ConstantValueExpression>(Value(true));  // disconnected: cross product
  }

  auto out_schema = ConcatSchemas(left.plan_->OutputSchema(), right.plan_->OutputSchema());
  auto join = std::make_shared<NestedLoopJoinPlanNode>(out_schema, left.plan_, right.plan_, std::move(predicate),
                                                       JoinType::INNER);
  std::vector<idx_t> order = left.leaf_order_;
  order.insert(order.end(), right.leaf_order_.begin(), right.leaf_order_.end());
  return Emitted{join, std::move(order)};
}

auto Optimizer::OptimizeJoinOrder(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  // Bottom-up: optimize children first so nested regions (and barrier subtrees) are already done.
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.push_back(OptimizeJoinOrder(child));
  }
  auto node = plan->CloneWithChildren(std::move(children));

  // A reorderable region is a Filter over a chain of INNER cross-product NestedLoopJoins.
  if (node->GetType() != PlanType::Filter) {
    return node;
  }
  const auto &filter = dynamic_cast<const FilterPlanNode &>(*node);
  BUMBLEBEE_ENSURE(node->children_.size() == 1, "Filter has exactly one child");

  std::vector<AbstractPlanNodeRef> leaves;
  CollectRegionLeaves(node->children_[0], leaves);

  // A 2-leaf region has exactly one join order but still one real decision — which side BUILDS
  // (the physical INNER hash join always builds child 0, and the planner emits FROM order, so
  // `FROM big, small` would build the big side). 3+ leaves get the full reordering search. Cap the
  // region so enumeration and the 64-bit RelationSet stay in range.
  if (leaves.size() < 2 || leaves.size() > MAX_REORDER_RELATIONS) {
    return node;
  }

  // Build the join graph. Each leaf becomes a relation with its column range in the flat schema.
  JoinGraph graph;
  idx_t offset = 0;
  for (const auto &leaf : leaves) {
    JoinRelation rel;
    rel.plan_ = leaf;
    rel.base_col_offset_ = offset;
    rel.col_count_ = leaf->OutputSchema().GetColumnCount();
    // Base cardinality from the catalog for a scan; a rough default otherwise.
    double card = 1000;
    if (leaf->GetType() == PlanType::SeqScan) {
      const auto &scan = dynamic_cast<const SeqScanPlanNode &>(*leaf);
      card = static_cast<double>(EstimatedCardinality(scan.table_name_).value_or(1000));
    }
    rel.cardinality_ = card;
    graph.AddRelation(rel);
    offset += rel.col_count_;
  }
  BUMBLEBEE_ENSURE(offset == filter.OutputSchema().GetColumnCount(),
                   "region leaf widths must sum to the filter's output width");
  const auto &relations = graph.Relations();

  // Split the WHERE predicate into conjuncts; collect join edges (with equi key endpoints) and
  // single-table filters, then enter the edges annotated with their key NDV.
  std::vector<AbstractExpressionRef> conjuncts;
  FlattenConjuncts(filter.GetPredicate(), conjuncts);
  std::vector<std::vector<AbstractExpressionRef>> single_filters(leaves.size());  // per-leaf local filters
  std::vector<PendingEdge> pending;
  if (!ClassifyRegionConjuncts(conjuncts, relations, single_filters, pending)) {
    return node;  // not the clean flat-frame shape we expect — leave the region untouched
  }
  AddEdgesWithKeyNdv(graph, pending, relations, offset);

  const CardinalityEstimator estimator;
  std::unique_ptr<JoinTreeNode> tree;
  if (leaves.size() == 2) {
    // Build-side placement only: proceed just when the FROM order would build on the bigger side
    // (strictly bigger, so equal-size and unknown-size regions keep their current plan). Leaf size
    // is the catalog row count discounted by FilterPushDown's crude 0.1-per-conjunct selectivity —
    // enough to see `FROM lineitem, part WHERE part-filters...` build on the filtered part side.
    double cards[2];
    for (idx_t i = 0; i < 2; i++) {
      cards[i] = relations[i].cardinality_;
      for (idx_t k = 0; k < single_filters[i].size(); k++) {
        cards[i] *= 0.1;
      }
    }
    if (cards[0] <= cards[1]) {
      return node;
    }
    auto make_leaf = [&](idx_t id) {
      auto leaf = std::make_unique<JoinTreeNode>();
      leaf->relations_ = RelationSet::Singleton(id);
      leaf->cardinality_ = cards[id];
      leaf->leaf_id_ = id;
      return leaf;
    };
    tree = std::make_unique<JoinTreeNode>();
    tree->relations_ = RelationSet::Singleton(0).Union(RelationSet::Singleton(1));
    tree->left_ = make_leaf(0);
    tree->right_ = make_leaf(1);  // the emitter swaps the smaller side into the build slot
  } else {
    // Reorder only if the search finds a plan estimated strictly cheaper than the planner's FROM order.
    // With accurate key NDV this both fixes cross-product regions (q02) and refuses to regress
    // well-shaped ones (q05 — a low-cardinality-key reorder now costs more, so the FROM order wins).
    tree = join_order_->Order(graph, estimator);
    if (tree == nullptr || SumInternalCardinalities(tree.get()) >= LeftDeepCost(graph, estimator)) {
      return node;
    }
  }

  Emitted root = EmitJoinTree(tree.get(), graph, relations, single_filters);

  // The region reordered its columns; restore the original flat order the region's parent expects,
  // with a projection selecting each original column from its new position in the emitted output.
  const auto &orig_schema = filter.OutputSchema();
  std::vector<AbstractExpressionRef> projections;
  projections.reserve(orig_schema.GetColumnCount());
  for (uint32_t p = 0; p < orig_schema.GetColumnCount(); p++) {
    const auto new_pos = LocalPosition(root, relations, p);
    projections.push_back(std::make_shared<ColumnValueExpression>(0U, new_pos, orig_schema.GetColumn(p)));
  }
  return std::make_shared<ProjectionPlanNode>(filter.output_schema_, std::move(projections), root.plan_);
}

}  // namespace bumblebee
