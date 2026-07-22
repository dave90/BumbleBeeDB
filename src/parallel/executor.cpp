//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// executor.cpp
//
// Identification: src/parallel/executor.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "parallel/executor.h"

#include <algorithm>
#include <memory>
#include <mutex>  // NOLINT
#include <string>
#include <thread>  // NOLINT
#include <unordered_map>
#include <utility>
#include <vector>

#include "fmt/format.h"
#include "parallel/pipeline_builder.h"
#include "parallel/pipeline_executor.h"
#include "parallel/thread_context.h"

namespace bumblebee {

namespace {

/** @brief Indent every line of `text` by `spaces` columns. */
auto Indent(const std::string &text, int spaces) -> std::string {
  const std::string pad(static_cast<size_t>(spaces), ' ');
  std::string out;
  size_t start = 0;
  while (start < text.size()) {
    auto nl = text.find('\n', start);
    if (nl == std::string::npos) {
      out += pad + text.substr(start);
      break;
    }
    out += pad + text.substr(start, nl - start + 1);
    start = nl + 1;
  }
  return out;
}

}  // namespace

namespace {

/** @brief Number the operator tree post-order, `0..N-1`; returns the next free id (== the count). */
auto AssignIds(PhysicalOperator &op, idx_t next) -> idx_t {
  for (auto &child : op.children_) {
    next = AssignIds(*child, next);
  }
  op.id_ = next;
  return next + 1;
}

}  // namespace

void Executor::Initialize(PhysicalOperator &root) {
  num_operators_ = AssignIds(root, 0);
  sink_states_.resize(num_operators_);
  source_states_.resize(num_operators_);
  operator_states_.resize(num_operators_);
  query_profile_.assign(num_operators_, OperatorProfile{});

  // Grow the pipeline DAG downwards from the root sink.
  PipelineBuilder builder(*this);
  auto &root_pipeline = builder.CreateRootPipeline();
  root.BuildPipelines(root_pipeline, builder);
  pipelines_ = std::move(builder.pipelines_);

  for (auto &p : pipelines_) {
    // BuildPipelines pushes streaming operators sink->source as it descends; flip to source->sink order.
    std::reverse(p->operators_.begin(), p->operators_.end());

    p->sink_gstate_ = GetOrCreateSinkState(*p->sink_);
    p->source_gstate_ = GetOrCreateSourceState(*p->source_);
    p->operator_gstates_.clear();
    p->operator_gstates_.reserve(p->operators_.size());
    for (auto *op : p->operators_) {
      p->operator_gstates_.push_back(GetOrCreateOperatorState(*op));
    }
    p->dependencies_remaining_.store(p->dependencies_.size(), std::memory_order_relaxed);
  }
}

auto Executor::GetOrCreateSinkState(const PhysicalOperator &op) -> GlobalSinkState * {
  auto &slot = sink_states_[op.id_];
  if (!slot) {
    slot = op.GetGlobalSinkState(context_);
  }
  return slot.get();
}

auto Executor::GetOrCreateSourceState(const PhysicalOperator &op) -> GlobalSourceState * {
  auto &slot = source_states_[op.id_];
  if (!slot) {
    // A sink+source operator (an aggregate, a sort) reads the very structure its own sink built.
    GlobalSinkState *own = op.IsSink() ? GetOrCreateSinkState(op) : nullptr;
    slot = op.GetGlobalSourceState(context_, own);
  }
  return slot.get();
}

auto Executor::GetOrCreateOperatorState(const PhysicalOperator &op) -> GlobalOperatorState * {
  auto &slot = operator_states_[op.id_];
  if (!slot) {
    // A sink+operator (a hash-join probe) reads the hash table its own sink built.
    GlobalSinkState *own = op.IsSink() ? GetOrCreateSinkState(op) : nullptr;
    slot = op.GetGlobalOperatorState(context_, own);
  }
  return slot.get();
}

void Executor::CreateTasks(Pipeline &p, std::vector<TaskRef> &out) {
  const idx_t n = std::max<idx_t>(1, p.MaxThreads());
  // ARM THE COUNTER BEFORE ANY TASK CAN BE OBSERVED: a fast worker could otherwise drain the
  // queue and drive tasks_remaining_ to 0 while we are still enqueuing. The relaxed store is published
  // by EnqueueAll's mutex (release); it must become `release` if the queue is ever made lock-free.
  p.tasks_remaining_.store(n, std::memory_order_relaxed);
  for (idx_t i = 0; i < n; i++) {
    out.push_back(std::make_unique<PipelineTask>(*this, p, num_operators_));
  }
  active_tasks_.fetch_add(n, std::memory_order_acq_rel);  // R1: before the caller decrements its own
}

void Executor::ReleaseDependents(Pipeline &p, bool no_output_possible) {
  std::vector<TaskRef> ready;
  for (auto *dep : p.dependents_) {
    if (no_output_possible) {
      dep->dead_.store(true, std::memory_order_relaxed);  // suppress the SOURCE only, never the pipeline
    }
    // Exactly one thread gets 1 back even if two dependencies finalize concurrently — no lock, no
    // double-schedule, no visited set for the diamond case.
    if (dep->dependencies_remaining_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
      continue;
    }
    CreateTasks(*dep, ready);  // ALWAYS schedule — a dead pipeline still runs, with an empty source
  }
  scheduler_.EnqueueAll(std::move(ready));  // one lock, one notify_all
}

void Executor::RunFinalize(Pipeline &p) {
  auto verdict = p.sink_->Finalize(context_, *p.sink_gstate_, 0, 0, 1);  // fast path: last task finalizes inline
  ReleaseDependents(p, verdict == SinkFinalizeType::NO_OUTPUT_POSSIBLE);
}

void Executor::AbortPipeline(Pipeline & /*p*/) {
  // The query has errored: this pipeline is counted done but releases no dependents, so the DAG drains
  // without advancing. active_tasks_ still reaches 0 because every in-flight task reaches TaskFinished.
}

void Executor::TaskFinished() {
  if (active_tasks_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
    return;
  }
  query_done_.store(true, std::memory_order_release);
  scheduler_.NotifyAll();  // wake the client thread out of cv_.wait (not optional)
}

void Executor::ExecuteQuery() {
  // Seed: every pipeline with no dependencies is ready at t=0, so sibling builds start together.
  std::vector<TaskRef> ready;
  for (auto &p : pipelines_) {
    if (p->dependencies_remaining_.load(std::memory_order_relaxed) == 0) {
      CreateTasks(*p, ready);
    }
  }
  if (active_tasks_.load(std::memory_order_relaxed) == 0) {
    return;  // nothing to run
  }
  scheduler_.EnqueueAll(std::move(ready));

  // The client thread becomes a worker (no idle coordinator core); spawn the rest of the pool.
  const idx_t nthreads = std::clamp<idx_t>(context_.config_.max_threads_, 1, MAX_THREADS);
  std::vector<std::thread> workers;
  workers.reserve(nthreads > 0 ? nthreads - 1 : 0);
  for (idx_t i = 1; i < nthreads; i++) {
    workers.emplace_back([this] { scheduler_.WorkUntil(query_done_); });
  }
  scheduler_.WorkUntil(query_done_);
  for (auto &t : workers) {
    t.join();
  }

  if (HasError()) {
    std::rethrow_exception(first_error_);
  }
}

void Executor::PushError(std::exception_ptr e) {
  std::lock_guard lock(error_mutex_);
  if (!has_error_.load(std::memory_order_relaxed)) {
    first_error_ = std::move(e);
    has_error_.store(true, std::memory_order_release);
  }
}

void Executor::MergeThreadProfiler(const ThreadProfiler &tp) {
  std::lock_guard lock(profile_mutex_);  // N tasks merge concurrently
  if (query_profile_.size() < tp.Size()) {
    query_profile_.resize(tp.Size());
  }
  for (idx_t i = 0; i < tp.Size(); i++) {
    query_profile_[i] += tp.Get(i);
  }
}

auto Executor::PipelinesToString() const -> std::string {
  std::unordered_map<const Pipeline *, idx_t> index;
  for (idx_t i = 0; i < pipelines_.size(); i++) {
    index[pipelines_[i].get()] = i;
  }
  std::string out;
  for (idx_t i = 0; i < pipelines_.size(); i++) {
    const auto &p = *pipelines_[i];
    out += fmt::format("P{}  deps: ", i);
    if (p.dependencies_.empty()) {
      out += "—";
    } else {
      for (idx_t d = 0; d < p.dependencies_.size(); d++) {
        out += (d == 0 ? "" : ", ") + fmt::format("P{}", index.at(p.dependencies_[d]));
      }
    }
    out += fmt::format("   max_threads: {}\n", p.MaxThreads());
    out += Indent(p.ToString(), 6);
    out += "\n";
  }
  return out;
}

auto Executor::AnalyzeToString(const PhysicalOperator &op, int indent) const -> std::string {
  const auto &prof = query_profile_[op.id_];
  const auto ms = [](uint64_t ns) { return static_cast<double>(ns) / 1e6; };
  std::string out(static_cast<size_t>(indent), ' ');
  out += op.GetName();
  auto params = op.ParamsToString();
  if (!params.empty()) {
    out += " " + params;
  }
  out += fmt::format("  [rows={} exec={:.3f}ms source={:.3f}ms sink={:.3f}ms combine={:.3f}ms finalize={:.3f}ms]",
                     prof.rows_out, ms(prof.exec_ns), ms(prof.source_ns), ms(prof.sink_ns), ms(prof.combine_ns),
                     ms(prof.finalize_ns));
  out += "\n";
  for (const auto &child : op.children_) {
    out += AnalyzeToString(*child, indent + 2);
  }
  return out;
}

}  // namespace bumblebee
