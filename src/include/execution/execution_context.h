//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// execution_context.h
//
// Identification: src/include/execution/execution_context.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include "main/client_context.h"
#include "parallel/thread_context.h"

namespace bumblebee {

class Pipeline;

/**
 * @brief The reference bundle passed to every `Execute` / `GetData` / `Sink` call — never stored.
 *
 * It exists so that adding a per-task facility later (a memory reservation, an interrupt token, a spill
 * directory) is a new field on `ClientContext` / `ThreadContext`, not a signature change rippling
 * through every operator. Operators read the transaction as `context.client_.txn_`.
 */
class ExecutionContext {
 public:
  ExecutionContext(ClientContext &client, ThreadContext &thread, Pipeline *pipeline)
      : client_(client), thread_(thread), pipeline_(pipeline) {}

  ClientContext &client_;
  ThreadContext &thread_;
  Pipeline *pipeline_;
};

}  // namespace bumblebee
