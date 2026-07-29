//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// pipeline.cpp
//
// Identification: src/parallel/pipeline.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "parallel/pipeline.h"

#include <algorithm>
#include <string>

#include "parallel/executor.h"

namespace bumblebee {

auto Pipeline::MaxThreads() const -> idx_t {
  idx_t n = source_gstate_->MaxThreads();
  if (!sink_->ParallelSink()) {
    n = 1;
  }
  for (auto *op : operators_) {
    if (!op->ParallelOperator()) {
      n = 1;
    }
  }
  if (sink_->SinkOrderDependent()) {
    // An order-dependent sink reconstructs the serial order from source batch indexes; without
    // them (or through an operator that picks rows nondeterministically, e.g. a streaming LIMIT)
    // the order is unrecoverable, so the pipeline stays serial.
    if (!source_->SourceProvidesBatchIndex()) {
      n = 1;
    }
    for (auto *op : operators_) {
      if (op->OperatorOrderDependent()) {
        n = 1;
      }
    }
  }
  if (source_->IsOrderPreserving()) {
    n = 1;  // a Sort source feeding a parallel sink would shuffle the order — keep it serial
  }
  return std::clamp<idx_t>(n, 1, executor_.MaxThreads());
}

auto Pipeline::ToString() const -> std::string {
  std::string out;
  out += "source   " + source_->GetName();
  auto sp = source_->ParamsToString();
  if (!sp.empty()) {
    out += " " + sp;
  }
  out += "\n";
  for (auto *op : operators_) {
    out += "operator " + op->GetName();
    auto opp = op->ParamsToString();
    if (!opp.empty()) {
      out += " " + opp;
    }
    out += "\n";
  }
  out += "sink     " + sink_->GetName();
  auto kp = sink_->ParamsToString();
  if (!kp.empty()) {
    out += " " + kp;
  }
  return out;
}

}  // namespace bumblebee
