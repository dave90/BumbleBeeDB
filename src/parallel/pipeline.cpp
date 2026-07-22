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
