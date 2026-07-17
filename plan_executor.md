# BumbleBeeDB — the execution layer

> Companion to `plan.md` (milestone 2, the vector layer). This document is **design only**: it describes
> what milestones 3–5 build, and names honestly what must land first. No build sequence.

## Context

Milestone 1 gave us a plan tree (`AbstractPlanNode`, 13 `PlanType`s). Milestone 2 is giving us the
columnar runtime (`Vector`, `DataChunk`). Neither can *run* a query. The question is what the engine
between them should look like.

The obvious move is to lift BumbleBee-datalog's `Scheduler` / `Task` / `PhysicalRuleExecutor`, which
already push `DataChunk`s through operators on a thread pool. But that machinery has two properties we
cannot live with in a SQL engine, and both are structural rather than incidental:

**1. Only the source and the sink may own global state.** `PhysicalAtom` gives a
`GlobalPhysicalAtomState`, a `combine()` and a `finalize()` to `isSource()` and `isSink()` operators —
nobody else. So a hash-join build, which needs a shared hash table and a finalize, *cannot be an
intermediate operator*. It has to be lifted out into its own `PhysicalRule` terminated by a
`PhysicalNopeOutput` sink. A separate rule means a separate scheduling unit, which means a separate
barrier, which means the intermediate result is fully materialized into `PredicateTables`.

**2. Dependencies are priority buckets with a hard barrier.** `Scheduler::schedulePriorityRules` runs
priority 0 to completion, then priority 1, discovering completion by *polling* a semaphore with a 100 ms
timeout and rescanning the whole bucket. Two independent build sides of two different joins land in
different buckets and therefore run **sequentially**, not concurrently.

Together these produce exactly the symptom: *rule by rule, storing the entire intermediate result* — up
to three copies of every row (sink → thread-local `ChunkCollection` → global `ChunkCollection` →
`PredicateTables`), plus a fourth when the next rule scans it back.

The fix is not a better scheduler. It is to make the *operator* the unit that owns state, and the
*pipeline* the unit that gets scheduled:

> **Give every operator all three roles — source, streaming operator, sink — each with its own local and
> global state. Then a pipeline is `source → streaming ops → sink`, the only thing that ever
> materializes is a sink, and "pipeline breaker" and "sink" become the same word.**

Dependencies between pipelines become a DAG with atomic counters instead of priority barriers, so
sibling build pipelines run concurrently. This is the DuckDB / Leis morsel-driven model, and it is what
BumbleBee's `PhysicalAtom` was *almost* shaped for — it has `execute` / `getData` / `sink` / `combine` /
`finalize` already. It simply never let intermediate operators use them.

Below, `[D-n]` marks a deliberate divergence from BumbleBee (collected in a table at the end); `[Bn]`
marks a prerequisite that does not exist yet.

---

## 1. `PhysicalOperator`

`src/include/execution/physical_operator.h`. All methods `const`; **every** mutation goes through a
state object. This much BumbleBee got right and we keep it.

```cpp
enum class SourceResultType : uint8_t { HAVE_MORE_OUTPUT, FINISHED };
enum class OperatorResultType : uint8_t { NEED_MORE_INPUT, HAVE_MORE_OUTPUT, FINISHED };
enum class SinkResultType : uint8_t { NEED_MORE_INPUT, FINISHED };
enum class SinkFinalizeType : uint8_t { READY, NO_OUTPUT_POSSIBLE };

class PhysicalOperator {
 public:
  PhysicalOperator(PhysicalOperatorType type, SchemaRef output_schema, idx_t estimated_cardinality);
  virtual ~PhysicalOperator() = default;

  // ---- streaming operator role ------------------------------------------------
  virtual auto IsOperator() const -> bool { return false; }
  virtual auto GetGlobalOperatorState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalOperatorState>;
  virtual auto GetLocalOperatorState(ExecutionContext &context) const
      -> std::unique_ptr<LocalOperatorState>;
  virtual auto Execute(ExecutionContext &context, DataChunk &input, DataChunk &output,
                       GlobalOperatorState &gstate, LocalOperatorState &lstate) const
      -> OperatorResultType;
  virtual auto ParallelOperator() const -> bool { return true; }

  // ---- source role ------------------------------------------------------------
  virtual auto IsSource() const -> bool { return false; }
  virtual auto GetGlobalSourceState(ClientContext &context, GlobalSinkState *own_sink_state) const
      -> std::unique_ptr<GlobalSourceState>;
  virtual auto GetLocalSourceState(ExecutionContext &context, GlobalSourceState &gstate) const
      -> std::unique_ptr<LocalSourceState>;
  virtual auto GetData(ExecutionContext &context, DataChunk &output,
                       GlobalSourceState &gstate, LocalSourceState &lstate) const
      -> SourceResultType;
  virtual auto IsOrderPreserving() const -> bool { return false; }

  // ---- sink role (== pipeline breaker) ----------------------------------------
  virtual auto IsSink() const -> bool { return false; }
  virtual auto GetGlobalSinkState(ClientContext &context) const -> std::unique_ptr<GlobalSinkState>;
  virtual auto GetLocalSinkState(ExecutionContext &context) const -> std::unique_ptr<LocalSinkState>;
  virtual auto Sink(ExecutionContext &context, DataChunk &input,
                    GlobalSinkState &gstate, LocalSinkState &lstate) const -> SinkResultType;
  virtual void Combine(ExecutionContext &context, GlobalSinkState &gstate,
                       LocalSinkState &lstate) const;
  virtual auto Finalize(ClientContext &context, GlobalSinkState &gstate,
                        idx_t stage, idx_t task_idx, idx_t task_count) const -> SinkFinalizeType;
  virtual auto ParallelSink() const -> bool { return true; }
  virtual auto FinalizeStageCount(GlobalSinkState &gstate) const -> idx_t { return 1; }
  virtual auto FinalizeMaxThreads(GlobalSinkState &gstate, idx_t stage) const -> idx_t { return 1; }

  virtual auto BuildPipelines(Pipeline &current, PipelineBuilder &builder) const -> void;

  PhysicalOperatorType type_;
  SchemaRef output_schema_;
  std::vector<std::unique_ptr<PhysicalOperator>> children_;
  idx_t estimated_cardinality_;
  idx_t id_;  /**< dense, assigned by PhysicalPlanGenerator; indexes the Executor's state registry (§2). */
};
```

**Four result enums, not one `AtomResultType`. [D-1]** In BumbleBee, `FINISHED` means three different
things depending on which method returned it — *source exhausted*, *operator refuses further input*,
*sink is full (LIMIT satisfied)* — and each propagates differently. `NEED_MORE_INPUT` is meaningless for
a source and `HAVE_MORE_OUTPUT` is meaningless for a sink, yet both are representable. Splitting the enum
makes the illegal states unrepresentable and lets the push loop switch on each without a comment
explaining which `FINISHED` this is.

**`Finalize` returns `SinkFinalizeType`, not `void`. [D-2] — and this is an *optimization*, not a
requirement.** It is worth being precise about what it is for, because it is easy to over-sell (an
earlier draft of this document did).

The **scheduler does not need it.** `dependencies_remaining_` already answers the only question the
scheduler asks — *when* may the next pipeline start — and it would answer it just as well if `Finalize`
returned `void`. The verdict answers a different question, which nothing else asks: *is it worth starting
the next pipeline at all?*

The one operator that ever returns anything but `READY` is an **inner** `PhysicalHashJoin` whose build
side came up empty. It then knows something no one could know at plan time: no probe row can ever match,
so the probe pipeline is about to scan (say) a billion rows of `b` to produce nothing.
`NO_OUTPUT_POSSIBLE` says *"I am empty, and nothing downstream of me can ever produce a row — skip the
work."* It is permanent and final; there is no "come back later" state. (`READY` is therefore not really a
value, it is the absence of news: "nothing special to report, proceed as normal.")

Delete the enum entirely and **the engine is still correct** — it just scans all of `b`, probes an empty
table, matches nothing, and returns the same answer more slowly. So this is droppable from v1: the
mechanism is one flag on `Pipeline` and one branch in `FetchFromSource` (§5.4), and it can be added later
with no scheduler changes. The only part worth keeping from day one is the *return type* in the signature,
since widening a signature later ripples through every operator, whereas a `void` → enum change does not.

§5.4 has the mechanism, and the correctness trap inside it.

**`Finalize` takes `(stage, task_idx, task_count)` from day one. [D-7]** Today every caller passes
`(0, 0, 1)` and the last task runs it inline. But the signature and the two counters on `Pipeline` are
already there, so a parallel 64-partition aggregate finalize is a drop-in later (§5.6) rather than a
redesign. BumbleBee's `finalize` is a single `FinalizeTask` enqueued by the polling coordinator and can
never be anything else.

---

## 2. `Pipeline`

```cpp
class Pipeline {
 public:
  auto MaxThreads() const -> idx_t;

  Executor &executor_;
  const PhysicalOperator *source_;
  std::vector<const PhysicalOperator *> operators_;   // streaming; never materialize
  const PhysicalOperator *sink_;

  GlobalSourceState *source_gstate_;                  // NOT owned — see below
  std::vector<GlobalOperatorState *> operator_gstates_;
  GlobalSinkState *sink_gstate_;

  std::vector<Pipeline *> dependencies_;              // must finish before we start
  std::vector<Pipeline *> dependents_;                // we release these when we finish

  std::atomic<idx_t> dependencies_remaining_;
  std::atomic<idx_t> tasks_remaining_;
  std::atomic<idx_t> finalize_tasks_remaining_;       // §5.6 seam, armed to 1 today
  std::atomic<idx_t> finalize_stage_;
  std::atomic<bool> dead_;                            // upstream said NO_OUTPUT_POSSIBLE: run with an
                                                      //   empty source. NOT "do not run" — see §5.4
  std::atomic<bool> stop_;                            // LIMIT satisfied: all tasks bail
};
```

**The load-bearing fact: a `Pipeline` does not own any global state. [D-4]**

One `PhysicalHashJoin` is the **sink** of the build pipeline *and* a **streaming operator** in the probe
pipeline. Those are two different `Pipeline` objects, and the hash table they share is *the same object*.
One `PhysicalHashAggregate` is the **sink** of its child pipeline and the **source** of its parent
pipeline — same partitioned hash table, two pipelines.

So global state is keyed by **operator**, not by pipeline, and lives in a registry on the `Executor`.

### The registry is three flat vectors indexed by a dense operator id — not a hash map

`PhysicalPlanGenerator` builds the tree once and numbers it `0..N-1` in a post-order walk, so
`PhysicalOperator` carries an `idx_t id_`:

```cpp
std::vector<std::unique_ptr<GlobalSinkState>>     sink_states_;      // indexed by op.id_, sized N
std::vector<std::unique_ptr<GlobalSourceState>>   source_states_;
std::vector<std::unique_ptr<GlobalOperatorState>> operator_states_;

auto Executor::GetOrCreateSinkState(const PhysicalOperator &op) -> GlobalSinkState * {
  auto &slot = sink_states_[op.id_];
  if (!slot) { slot = op.GetGlobalSinkState(context_); }   // memoized: one state, every pipeline
  return slot.get();
}
```

The registry is touched **only during `Executor::Initialize()`**, on the client thread, while building
pipelines — once a `Pipeline` has cached its raw `sink_gstate_` / `source_gstate_` / `operator_gstates_`
pointers, nothing on the hot path ever looks anything up again. So the container was never buying lookup
speed; it was doing two duller jobs: *owning* the state, and *memoizing* it so that the build pipeline
and the probe pipeline resolve to the **same** `GlobalSinkState` object. A dense id does both with no
hashing and no pointer-keyed indirection, and makes "exactly one global state per operator, shared by
every pipeline that touches it" a **structural** invariant — it is one slot — rather than a convention a
map happens to uphold. A null slot means "this operator never plays that role", which is true of most of
them.

**The state stays on the `Executor`, not on the operator.** DuckDB puts a
`mutable unique_ptr<GlobalSinkState> sink_state` on `PhysicalOperator` itself, which is shorter — but
then *executing* a plan *mutates* it, so the physical tree cannot be re-executed or shared across
concurrent queries, which forecloses prepared statements and any plan cache. One `idx_t` on the operator
buys an immutable, re-executable plan.

This is also why `GetGlobalOperatorState` / `GetGlobalSourceState` take a `GlobalSinkState *own_sink_state`:
the hash join's *probe* must read the hash table that its own *sink* built. The `Executor` knows both and
wires the pointer once, at initialization — no lookup at execution time, and the operator tree stays
`const`.

BumbleBee puts `sourceGlobalState_` and `sinkGlobalState_` on `PhysicalRule` — which is precisely *why*
an atom cannot appear in two rules, and therefore why the build has to become a rule of its own. Moving
the state into an operator-indexed registry is the single change that kills flaw #1 at the root.

---

## 3. Pipeline construction — the "nested pipelines"

```cpp
virtual auto BuildPipelines(Pipeline &current, PipelineBuilder &builder) const -> void;
```

The builder starts at the root sink (the `PhysicalResultCollector`) and recurses **downwards**. Each
operator kind does exactly one thing:

| operator kind | `BuildPipelines(current, builder)` |
|---|---|
| **source only** (`TableScan`, `ParquetScan`, `Values`) | `current.source_ = this;` — the pipeline is closed. |
| **streaming** (`Filter`, `Projection`, `Limit`) | `current.operators_.push_back(this);`<br>`children_[0]->BuildPipelines(current, builder);` |
| **sink + operator** (`HashJoin`, `NestedLoopJoin`) | `current.operators_.push_back(this);` — the *probe* is a streaming op of the current pipeline<br>`children_[1]->BuildPipelines(current, builder);` — the probe side continues this pipeline<br>`auto &build = builder.CreateChildPipeline(current, *this);` — a **new** pipeline whose sink is `this`<br>`children_[0]->BuildPipelines(build, builder);` |
| **sink + source** (`HashAggregate`, `Sort`, `TopN`) | `current.source_ = this;` — the parent pipeline *reads from* this operator; the pipeline **breaks** here<br>`auto &build = builder.CreateChildPipeline(current, *this);`<br>`children_[0]->BuildPipelines(build, builder);` |

`CreateChildPipeline(parent, sink_op)` allocates a `Pipeline` with `sink_ = &sink_op` and registers
`parent.dependencies_.push_back(child)` / `child.dependents_.push_back(&parent)`.

That is the whole of "nested pipelines". Nothing nests *inside* the push loop; the nesting is in the
**DAG between pipelines**, and each edge is exactly one materialization point.

### Worked example

```sql
SELECT a.x, SUM(b.y) FROM a JOIN b ON a.k = b.k
WHERE a.x > 5 GROUP BY a.x ORDER BY 2 DESC LIMIT 10;
```

After the existing optimizer (`FilterPushDown` + `MergeFilterScan` fold the predicate into the scan;
`SortLimitAsTopN` collapses the sort+limit), lowering (§9) gives:

```
PhysicalResultCollector                 sink
└── PhysicalTopN(n=10)                  sink + source
    └── PhysicalHashAggregate           sink + source
        └── PhysicalHashJoin(a.k=b.k)   sink (build) + operator (probe)
            ├── child 0 = build: PhysicalTableScan(a, filter x>5)
            └── child 1 = probe: PhysicalTableScan(b)
```

and `BuildPipelines` produces four pipelines:

```
P0  src TableScan(a)+filter  ─ ops []                ─ sink HashJoin(build)      deps: —
P1  src TableScan(b)         ─ ops [HashJoin(probe)] ─ sink HashAggregate        deps: P0
P2  src HashAggregate        ─ ops []                ─ sink TopN                 deps: P1
P3  src TopN                 ─ ops []                ─ sink ResultCollector      deps: P2
```

Four pipelines, four sinks, four materialization points — the hash table, the aggregate's partitioned
table, the top-n heap, and the result. **`P1` streams `b` through the probe and straight into the
aggregate's sink with zero intermediate storage** — which is exactly what BumbleBee cannot do: there, the
join's output lands in a `PredicateTables` before the aggregate rule starts.

### Why siblings run concurrently [D-6]

`SELECT * FROM a JOIN b ON a.k=b.k JOIN c ON a.j=c.j` has two build pipelines (`b`'s hash table and `c`'s
hash table). **Both have `dependencies_remaining_ == 0`, so both are scheduled at t=0** and their morsels
interleave across the whole worker pool. In BumbleBee these are two rules in two priority buckets and
they run one after the other, each stalling the pool at its tail.

### Do we need DuckDB's `MetaPipeline`?

**No — not for the current 13 `PlanType`s.** `MetaPipeline` exists to group several pipelines that *share
one sink*, so the sink's `Finalize` runs only after **all** of them finish. Every breaker we have
(`HashJoin`, `HashAggregate`, `Sort`, `TopN`, `ResultCollector`) is fed by exactly **one** pipeline, so
`dependencies_remaining_` on a plain `Pipeline` says the same thing with less machinery.

**What breaks when UNION / CTE / recursion arrive:** a `UNION ALL` feeding one aggregate is two pipelines
with the same `sink_`. With today's model each would independently drive `tasks_remaining_ → 0` and each
would run `Finalize`. The fix is a `MetaPipeline` grouping them behind one shared `tasks_remaining_`, and
the seam is small: `Pipeline::sink_` is already a *shared, non-owning* pointer into the operator-indexed
registry, so the sink state is already correct — only the *completion counter* moves up a level.
Recursive CTEs additionally need `Pipeline::Reset()` to re-arm the counters per iteration, which is the
one place the "no ABA on `tasks_remaining_`" argument in §5.3 must be re-checked.

---

## 4. `PipelineExecutor` — the push loop

One instance per **task** (i.e. per thread, per pipeline). It owns every piece of thread-local state:

```cpp
class PipelineExecutor {
  Pipeline &pipeline_;
  ExecutionContext context_;

  std::unique_ptr<LocalSourceState> local_source_state_;
  std::vector<std::unique_ptr<LocalOperatorState>> intermediate_states_;
  std::unique_ptr<LocalSinkState> local_sink_state_;

  /** One cached chunk PER OPERATOR BOUNDARY. [0] = source output, [i] = output of operators_[i-1]. */
  std::vector<std::unique_ptr<DataChunk>> intermediate_chunks_;
  DataChunk final_chunk_;                  // the chunk handed to the sink

  /** LIFO of operators that still hold buffered output. Invariant: strictly increasing bottom→top. */
  std::vector<idx_t> in_process_operators_;
  bool exhausted_source_{false};
  bool finished_{false};
};
```

Chunks and states are allocated **once**, at task start, and `Reset()` in the loop. There is no allocation
on the hot path.

```cpp
void PipelineExecutor::Run() {
  auto &source_chunk = *intermediate_chunks_[0];
  while (!finished_) {
    if (pipeline_.stop_.load(std::memory_order_relaxed) || pipeline_.executor_.HasError()) {
      break;                                   // cancellation latency is bounded by ONE chunk
    }
    if (in_process_operators_.empty()) {
      if (exhausted_source_) { break; }
      source_chunk.Reset();
      if (FetchFromSource(source_chunk) == SourceResultType::FINISHED) { exhausted_source_ = true; }
      if (source_chunk.GetSize() == 0) { continue; }   // the loop head breaks if also exhausted
    }
    if (ExecutePushInternal(source_chunk) == OperatorResultType::FINISHED) { finished_ = true; }
  }
  PushFinalize();                              // Combine — runs even on an early LIMIT exit
}

auto PipelineExecutor::ExecutePushInternal(DataChunk &input, idx_t initial_idx) -> OperatorResultType {
  if (pipeline_.operators_.empty()) {
    return Sink(input) == SinkResultType::FINISHED ? OperatorResultType::FINISHED
                                                   : OperatorResultType::NEED_MORE_INPUT;
  }
  while (true) {
    final_chunk_.Reset();
    auto result = Execute(input, final_chunk_, initial_idx);
    if (result == OperatorResultType::FINISHED) { return OperatorResultType::FINISHED; }
    if (final_chunk_.GetSize() > 0 && Sink(final_chunk_) == SinkResultType::FINISHED) {
      return OperatorResultType::FINISHED;
    }
    if (result == OperatorResultType::NEED_MORE_INPUT) { return OperatorResultType::NEED_MORE_INPUT; }
    // HAVE_MORE_OUTPUT: the chain still owes output for THIS input. Re-enter; do not refetch.
  }
}

auto PipelineExecutor::Execute(DataChunk &input, DataChunk &result, idx_t initial_idx)
    -> OperatorResultType {
  idx_t current_idx;
  GoToSource(current_idx, initial_idx);        // pop the LIFO if non-empty, else start at initial_idx
  if (current_idx == initial_idx) { current_idx++; }

  while (current_idx <= pipeline_.operators_.size()) {
    auto &prev_chunk    = (current_idx == initial_idx + 1) ? input
                                                           : *intermediate_chunks_[current_idx - 1];
    auto &current_op    = *pipeline_.operators_[current_idx - 1];
    auto &current_chunk = (current_idx == pipeline_.operators_.size())
                              ? result : *intermediate_chunks_[current_idx];

    current_chunk.Reset();                     // [D-12] BumbleBee never does this
    auto res = current_op.Execute(context_, prev_chunk, current_chunk,
                                  *pipeline_.operator_gstates_[current_idx - 1],
                                  *intermediate_states_[current_idx - 1]);

    if (res == OperatorResultType::FINISHED) { return OperatorResultType::FINISHED; }
    if (res == OperatorResultType::HAVE_MORE_OUTPUT) {
      BUMBLEBEE_ASSERT(current_chunk.GetSize() > 0, "HAVE_MORE_OUTPUT with an empty chunk loops forever");
      in_process_operators_.push_back(current_idx);
    }
    if (current_chunk.GetSize() == 0) {
      // This operator swallowed everything (a filter that matched nothing). Nothing to push down.
      if (in_process_operators_.empty()) { return OperatorResultType::NEED_MORE_INPUT; }
      GoToSource(current_idx, initial_idx);    // resume whoever still owes output
      continue;
    }
    current_idx++;
  }
  return in_process_operators_.empty() ? OperatorResultType::NEED_MORE_INPUT
                                       : OperatorResultType::HAVE_MORE_OUTPUT;
}
```

### Why one cached chunk *per boundary*, and why the LIFO pops the highest index first

A hash-join probe can match more than `STANDARD_VECTOR_SIZE` build rows for a single probe chunk, so it
returns `HAVE_MORE_OUTPUT` and parks a cursor in its `LocalOperatorState`. That cursor is a
`SelectionVector` **indexing into its input chunk** — `intermediate_chunks_[i-1]`. When we re-enter
operator *i*, that input chunk **must still be intact**. It is, because chunk *j* is only ever `Reset()`
when we are about to write into it as the output of operator *j*. Sharing one chunk across boundaries
would corrupt the parked cursor. That is the whole reason for the per-boundary allocation.

And the LIFO must pop the **highest** index first: if operators *i* and *i+1* both owe output, draining
*i+1* first is mandatory, because re-entering *i* overwrites `intermediate_chunks_[i]` — which is exactly
*i+1*'s input.

### What we fix relative to `PhysicalRuleExecutor::executePush` [D-12]

BumbleBee's loop is under-specified rather than plainly wrong, but the gaps are real: the output chunk is
never `Reset()` before `execute()` (every operator has to remember to do it); a zero-row intermediate
chunk is still pushed all the way down to the sink; re-entry only ever happens *after* a full traverse to
the sink, never as a mid-chain short-circuit; `executeCombine()` is declared and never defined (combine
ends up inlined and called on the **sink only**, so no intermediate operator can have one); and
`atomResults_` is written and never read. We restate the loop with explicit invariants and assert them.

### `FINISHED` propagation and early exit

A satisfied `LIMIT` returns `FINISHED` from `Execute`. That stops **this** task. The other tasks on the
same pipeline are stopped by the sink setting `Pipeline::stop_`, which `Run()` polls once per chunk.
`PushFinalize()` (i.e. `Combine`) still runs for **every** task — skipping it on an early exit is a
silent-corruption bug, because the sink's global state would then miss that thread's contribution.

---

## 5. The scheduler: a counter-based pipeline DAG

`src/include/parallel/`. No polling, no barriers, no coordinator thread. Roughly 250 lines.

### 5.1 `TaskScheduler`

A fixed pool of workers plus a `std::mutex` + `std::condition_variable` deque. Tasks are *coarse* — a task
is one thread's entire drain of one pipeline, pulling morsels internally — so the queue is touched once
per task, not once per chunk, and a lock-free queue (moodycamel, as BumbleBee vendors) buys nothing yet.
It can be swapped in later; §5.2 flags the one memory-ordering line that would then change.

```cpp
void TaskScheduler::WorkUntil(std::atomic<bool> &done) {
  while (true) {
    TaskRef task;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return !queue_.empty() || done.load(std::memory_order_acquire) ||
                                  shutdown_.load(std::memory_order_acquire); });
      if (queue_.empty()) { return; }
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    task->Execute();   // <-- the lock is RELEASED first. Load-bearing: see hazard H2.
  }
}
```

The **client thread calls `WorkUntil(query_done_)` and becomes a worker**, so we do not idle a core on a
coordinator. BumbleBee blocks its coordinator in a 100 ms poll loop: N workers plus one wasted thread.

### 5.2 The task protocol

```cpp
void PipelineTask::Execute() {
  auto &ex = pipeline_.executor_;
  try {
    if (!ex.HasError()) { PipelineExecutor(pipeline_, thread_).Run(); }
  } catch (...) {
    ex.PushError(std::current_exception());     // latches the FIRST exception, sets has_error_ (release)
  }
  // (A) LAST-TASK-WINS. Must come before (B) — rule R1.
  if (pipeline_.tasks_remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    if (ex.HasError()) { ex.AbortPipeline(pipeline_); }   // count it done; release NO dependents
    else               { ex.RunFinalize(pipeline_); }     // may enqueue dependents' tasks
  }
  // (B) global bookkeeping
  ex.TaskFinished();
}

void Executor::CreateTasks(Pipeline &p, std::vector<TaskRef> &out) {
  const idx_t n = std::max<idx_t>(1, p.MaxThreads());
  // ARM THE COUNTER BEFORE ANY TASK CAN BE OBSERVED. Enqueue-then-arm lets a fast worker drain the
  // queue and drive tasks_remaining_ to 0 while we are still enqueueing task #2 — Finalize would then
  // run on a half-built GlobalSinkState. This ordering is the only defence.
  p.tasks_remaining_.store(n, std::memory_order_relaxed);
  for (idx_t i = 0; i < n; ++i) { out.push_back(MakeTask(p)); }
  active_tasks_.fetch_add(n, std::memory_order_acq_rel);   // R1: before the caller decrements its own
}
```

The `relaxed` store of `tasks_remaining_` is safe **only** because tasks are published through
`EnqueueAll`, which takes the scheduler mutex, and that release/acquire pair carries the store. **If the
queue is ever made lock-free, this store must become `release`.** That comment goes in the code.

### 5.3 Why the last task may run `Finalize` with no extra synchronization

1. Every task writes its contribution into the `GlobalSinkState` inside `Combine()`.
2. Every task then does `tasks_remaining_.fetch_sub(acq_rel)`.
3. The **release** half publishes everything that task wrote beforehand.
4. RMWs on one atomic form a **release sequence**: each `fetch_sub` reads the last value in the
   modification order and joins the sequence headed by every preceding release on that object.
5. The **last** task's `fetch_sub` has **acquire** semantics and reads from that sequence, so it
   synchronizes-with every prior `fetch_sub` and transitively happens-after every `Combine`.
6. Therefore `Finalize` sees every thread's contribution. **No mutex, no extra fence.**

`acq_rel` is the *minimum* that works; `relaxed` breaks step 3 and the partitioned aggregate silently
loses whole partitions. **No ABA:** the counter only decreases within one arming, is never incremented
after its tasks are enqueued, and is only re-armed by `Pipeline::Reset()` — which runs on the client
thread with zero tasks in flight (the recursive-CTE seam).

### 5.4 Dependency release, and what `NO_OUTPUT_POSSIBLE` must *not* do

```cpp
void Executor::ReleaseDependents(Pipeline &p, bool no_output_possible) {
  std::vector<TaskRef> ready;
  for (auto *dep : p.dependents_) {
    if (no_output_possible) { dep->dead_.store(true, std::memory_order_relaxed); }
    // Exactly one thread gets 1 back, even if two dependencies finalize concurrently.
    // No lock, no double-schedule, no visited-set for the diamond case.
    if (dep->dependencies_remaining_.fetch_sub(1, std::memory_order_acq_rel) != 1) { continue; }
    CreateTasks(*dep, ready);        // ALWAYS schedule. dead_ only suppresses the SOURCE — see below.
  }
  scheduler_.EnqueueAll(std::move(ready));           // one lock, one notify_all
}
```

…and the entire effect of `dead_` is one branch at the top of the push loop:

```cpp
auto PipelineExecutor::FetchFromSource(DataChunk &result) -> SourceResultType {
  if (pipeline_.dead_.load(std::memory_order_relaxed)) {
    return SourceResultType::FINISHED;   // the source is never opened. THIS is the whole optimization.
  }
  return pipeline_.source_->GetData(context_, result, *pipeline_.source_gstate_, *local_source_state_);
}
```

A dead pipeline is scheduled with **one** task, that task's source yields nothing, and everything else
happens exactly as normal: `Combine` runs, the last (only) task runs `Finalize`, the dependents are
released. The `dead_` store may be `relaxed` *here and only here*, because the `fetch_sub` immediately
following is `acq_rel` on the same atomic the last decrementer acquires — so both interleavings agree.

**The trap: `dead_` must suppress the *source*, not the *pipeline*.** The tempting reading of
`NO_OUTPUT_POSSIBLE` is "skip the dependent pipelines entirely, mark them complete without running, and
cascade the kill downstream". That is **wrong**, and it produces wrong answers, not merely slow ones:

```sql
SELECT COUNT(*) FROM a JOIN b ON a.k = b.k;   -- a is EMPTY
```

```
P0:  Scan(a) ─────────────────────────▶ HashJoin (build)       deps: —
P1:  Scan(b) ─▶ [HashJoin (probe)] ───▶ UngroupedAggregate     deps: P0
P2:  UngroupedAggregate ──────────────▶ ResultCollector        deps: P1
```

The correct answer is **one row containing `0`**. Under "cascade the kill", P1 *and* P2 are marked dead
and never run — so the aggregate's `Combine` and `Finalize` never happen, it never emits its zero-input
row, and the query returns an **empty result set**. Under "suppress the source": P1 runs with one task,
`b` is **never scanned** (the optimization is fully preserved — that was the billion rows we wanted to
skip), the aggregate combines a zero-input state, finalizes to `0`, and returns **`READY`** — because an
ungrouped aggregate always has output, even on empty input. So the deadness **stops there**, P2 runs
normally, and the client gets its `0`.

That is the general rule: **deadness travels one hop, and every sink re-judges it from its own
`Finalize`.** An aggregate above an empty join resurrects the query (`READY`); a *hash-join build* fed by
a dead pipeline finds its own table empty and returns `NO_OUTPUT_POSSIBLE` again, so the signal keeps
climbing, correctly; a `ResultCollector` fed by a dead pipeline yields an empty result, which for
`SELECT * FROM a JOIN b` is exactly right.

Note this is the *same* argument as the `std::max<idx_t>(1, ...)` rule in §6 — a zero-row source still
gets one task precisely so `Combine` → `Finalize` runs and `COUNT(*)` on an empty table returns `0`. An
empty **source** and a dead **dependency** are the same situation and must get the same rule.

**And the deadlock disappears with the cascade.** The reason an earlier draft needed a kill-cascade *plus*
a rule that dead pipelines still increment `pipelines_completed_` was that a never-scheduled pipeline
never increments anything, so the client waits forever. Here **every pipeline runs**, so every pipeline
increments the counter on its own. No cascade, no completed-without-running bookkeeping, no hang.

### 5.5 Completion, exceptions, cancellation

**Rule R1:** *a task that will cause more tasks to be enqueued must enqueue them — and so increment
`active_tasks_` — strictly before decrementing its own.* That is why step (A) precedes step (B) in
`PipelineTask::Execute`. Reverse them and `active_tasks_` transiently hits zero between pipelines, the
client thread wakes, and the query returns after one pipeline. Because **every** pipeline runs — a dead
one included, it just has an empty source (§5.4) — and every task therefore reaches step (B), R1 alone
makes `active_tasks_ == 0` *equivalent to* "the query is genuinely over, one way or the other":

```cpp
void Executor::TaskFinished() {
  if (active_tasks_.fetch_sub(1, std::memory_order_acq_rel) != 1) { return; }
  query_done_.store(true, std::memory_order_release);
  scheduler_.NotifyAll();          // wakes the CLIENT thread out of cv_.wait. Not optional.
}
```

**Exceptions.** A worker catches everything, latches the first `exception_ptr`, sets `has_error_`, and
**still** completes steps (A) and (B) — the counters must reach their terminal values or the client hangs
holding a perfectly good exception. In-flight tasks poll `HasError()` once per chunk and bail. The last
task of a pipeline **skips `Finalize`** on error (the sink state is half-built) and releases no
dependents, so the DAG drains without advancing. The client rethrows. **Cancellation** is the same
mechanism with a different trigger. Note `ExceptionType::EXECUTION` exists today but `ExecutionException`
does not — it needs adding.

**Three hazards from the client-as-worker, named so they are not rediscovered:**

- **H1 — the client blocks forever.** The last task sets `query_done_` but the queue is empty, so nothing
  would notify it. Hence the explicit `NotifyAll()` and the three-way wait predicate. Miss this and
  *every* query hangs at the end.
- **H2 — re-entering the scheduler mutex.** A task's `Finalize` calls `EnqueueAll`, which takes `mutex_`.
  Safe **only** because `WorkUntil` releases the lock before `Execute()`. This is the single most likely
  way to write this wrong.
- **H3 — the client picks up a foreign query's task.** Unreachable today (one `Executor` at a time), but
  in a multi-session server it is head-of-line blocking and a potential deadlock. Leave an
  `Executor *owner_` on `Task` so a filter can be added without touching anything else. Do not build it.

### 5.6 The seam for a parallel / multi-phase `Finalize` [D-7]

```cpp
void Executor::RunFinalize(Pipeline &p) {
  auto &sink = *p.sink_;
  // <<-- THE SEAM. Today both sides are 1, so we take the fast path and the last task finalizes
  //      INLINE: no queue round-trip, no extra thread.
  if (sink.FinalizeStageCount(*p.sink_gstate_) == 1 &&
      sink.FinalizeMaxThreads(*p.sink_gstate_, 0) == 1) {
    OnPipelineFinished(p, sink.Finalize(context_, *p.sink_gstate_, 0, 0, 1));
    return;
  }
  ScheduleFinalizeStage(p, 0);   // later: N tasks, same last-task-wins shape on finalize_tasks_remaining_
}
```

Because "last task wins → advance" is a *counter protocol on the `Pipeline`* and not a barrier in the
scheduler, `ScheduleFinalizeStage` needs **no new machinery**: `finalize_tasks_remaining_` and
`finalize_stage_` are already fields. The first user is waiting — `PartitionedAggHT::finalize()` is 64
independent partitions, so `FinalizeMaxThreads() = RADIX_PARTITIONS` and `Finalize(stage=0, task_idx=p)`
does partition *p*. A parallel hash-join build after a parallel collect is two stages. **None of this
touches `Pipeline`, `PipelineExecutor`, `TaskScheduler`, or the completion protocol.**

---

## 6. Morsels, and the two scan sources

### The `GlobalSourceState` contract is deliberately tiny

1. `MaxThreads()` — a hint, read **once**, by one thread, at `CreateTasks` time.
2. `GetData(...)` — callable from N threads concurrently, each with its own `LocalSourceState`,
   **self-synchronizing**, eventually returning `FINISHED` to every task.

It says **nothing about morsels**. A morsel is a private detail of a particular source's global state,
which is why `Values` (one chunk, one thread), `ParquetScan` (row groups) and `TableScan` (page ranges)
fit under one interface with no common base.

### `PhysicalTableScan` — row-format pages

Morsel = **a page range, not a tuple range**. A page is the unit of buffer-pool pinning and of I/O; a
tuple range straddles pages and forces two tasks to pin the same page — extra pin/unpin traffic and a
contended latch on the boundary page of *every* morsel. Size `MORSEL_PAGES` so a morsel ≈ 100k rows.

```cpp
auto NextMorsel(idx_t &begin_page, idx_t &end_page) -> bool {
  const idx_t start = next_page_.fetch_add(MORSEL_PAGES, std::memory_order_relaxed);
  if (start >= page_count_) { return false; }
  begin_page = start;
  end_page = std::min(start + MORSEL_PAGES, page_count_);
  return true;
}
```

A monotonic range needs **no mutex**. BumbleBee takes a `lock_guard` per morsel handout — not a throughput
problem at that granularity, but a needless serialization point that forces a mutex into the state. **[D-8]**

**The row→vector bridge happens *inside* `GetData`, not in a separate operator:**

```
1. Walk pages [current_page_, end_page_), collecting up to STANDARD_VECTOR_SIZE row pointers into a
   POINTER Vector (skipping deleted slots via the page's tuple bitmap).
2. For each projected column c:  RowOperations::Gather(row_ptrs, sel, chunk.data_[c], count, layout_, c)
3. If filter_predicate_:  ExpressionExecutor::Select(chunk, sel) -> count;  chunk.Slice(sel, count)  // zero-copy
4. Return HAVE_MORE_OUTPUT, or FINISHED when the morsel space is exhausted.
```

Two reasons it must live here rather than in a `PhysicalGather` operator: a separate operator would need
its own chunk boundary (one more copy for nothing), and — the real one — **only here can the gather see
the filter.** That enables late materialization: gather *only* the filter's column, evaluate, get a
`SelectionVector`, gather the remaining columns *through it*. A separate operator forfeits that permanently.

**The honest cost.** A row of width *W* costs ~`ceil(W/64)` cache lines *regardless of how many columns you
project* — the whole row sits on the lines you must fetch anyway. `PhysicalTableScan` is bandwidth-bound in
*W*; `PhysicalParquetScan` is bandwidth-bound in the number of columns actually read. That asymmetry is
exactly why the design carries both. It also means **a pushed-down filter on a row table does not reduce
the gather** (you already touched the line) — it only reduces everything above the scan. On Parquet the
same `filter_predicate_` field additionally enables **row-group skipping via statistics**, a qualitatively
bigger win. Same field, two very different physical meanings; lowering (§9) hands it to both, for
different purposes.

### `PhysicalParquetScan` — row groups

Morsel = **one row group** (`next_row_group_.fetch_add(1)`). They are already 100k–1M rows and already the
unit of decompression and of statistics; a row group cannot be subdivided without decoding it twice. The
`LocalSourceState` holds the per-column readers, decompression buffers and dictionary pages — expensive to
construct, which is precisely why they are local state and are not rebuilt per chunk. It decodes
**straight into `Vector`s**: no gather, no transpose, no bridge. That is the point of the columnar path.

### Task count

```cpp
auto Pipeline::MaxThreads() const -> idx_t {
  idx_t n = source_gstate_->MaxThreads();
  if (!sink_->ParallelSink())       { n = 1; }
  for (auto *op : operators_) { if (!op->ParallelOperator()) { n = 1; } }
  if (source_->IsOrderPreserving()) { n = 1; }   // a Sort source feeding a parallel sink = garbage order
  return std::clamp<idx_t>(n, 1, executor_.context_.config_.max_threads_);
}
```

**Over-subscribing is fine; under-subscribing is unrecoverable.** Tasks *pull* morsels, so a surplus task
that finds the morsel space drained gets `FINISHED` on its first `GetData`, runs an empty `Combine`, and
exits — one queue round-trip wasted, and in exchange we get free load balancing under skew. But the task
count is chosen **once**, `tasks_remaining_` is armed to it, and *adding a task later is exactly the race
§5.2 forbids*. So a pipeline scheduled with one task stays single-threaded for its whole life no matter how
idle the pool becomes. **A source must never under-report `MaxThreads()`.** The clamp to `max_threads_`
belongs at the scheduler, not in the source.

**The honest limit:** parallelism is bounded below by morsel granularity. A Parquet file with one row group
runs single-threaded however many workers exist. Morsel-driven-without-stealing buys load balance, not
parallelism out of nothing.

**`std::max<idx_t>(1, ...)` is load-bearing:** a zero-row source still gets one task, whose `Combine` runs,
so it is the last task, so `Finalize` runs — which is exactly what `SELECT COUNT(*) FROM empty` (one row,
`0`) needs. A "skip the pipeline if the source is empty" optimization silently breaks it.

---

## 7. What materializes — and nothing else

**`IsSink() == true` ⟺ the operator is a pipeline breaker.** For our `PlanType` set that is exactly:

| sink | local state (per task, **lock-free**) | `Combine` | `Finalize` |
|---|---|---|---|
| `PhysicalHashJoin` (build) | thread-local `JoinPRLHashTable` | radix-partition the local table; merge partition *p* under `partition_mutex_[p]` | build the pointer directory. Empty build + INNER ⇒ `NO_OUTPUT_POSSIBLE` ⇒ the probe pipeline runs with an empty source, so `b` is never scanned (§5.4) |
| `PhysicalNestedLoopJoin` (build) | thread-local `ChunkCollection` | **move** the chunks into the global collection under one mutex | — |
| `PhysicalHashAggregate` | thread-local `AggregatePRLHashTable` | `PartitionedAggHT::combineLocalHt()` — radix-partition on the high hash bits, merge bucket *p* under `partitionsMutex_[p]` | aggregate each partition. **First user of the §5.6 seam** |
| `PhysicalUngroupedAggregate` | one partial state per aggregate | fold partials under one mutex (N locks *per query*) | — |
| `PhysicalSort` | thread-local `RowDataCollection` of sort-key-encoded rows, sorted at Combine | append the local *run* to the global run list | k-way merge. **Second user of the seam** |
| `PhysicalTopN` | thread-local `TopNHeap(n)` | merge the ≤ *n* local entries under one mutex | sort the final ≤ *n* rows |
| `PhysicalResultCollector` / `Insert` / `Update` / `Delete` | thread-local `ChunkCollection` | **`std::move` the `unique_ptr<DataChunk>`s** into the global collection — zero row copies | — |

Everything else is **strictly streaming** — it never allocates output storage that outlives one `Execute()`
call:

- **`Filter`** — `Select(input, sel) -> count`, then `output.Reference(input); output.Slice(sel, count);`.
  **Zero copies.** `DataChunk::Reference` and `Vector::Slice` already exist and are exactly this; the
  output vectors become `DICTIONARY` over the input's storage.
- **`Projection`** — one `ExpressionExecutor::Execute`. A bare `ColumnValueExpression` degenerates to
  `output.data_[i].Reference(input.data_[j])`: **zero copy** for the common `SELECT a, b`.
- **`Limit`** — `output.Reference(input)` plus a cardinality, with an atomic counter in global state.
- **`HashJoin` (probe)** — `output.Slice(input, probe_sel, count)` for the probe columns (**zero copy**)
  plus one `RowOperations::Gather` for the build columns (unavoidable — the build side *is* a row layout).
  Re-entrant via `HAVE_MORE_OUTPUT`; **this is the operator that makes §4's per-boundary chunk rule
  non-negotiable.**

### Copies per output row — the headline [D-13]

| | copies |
|---|---|
| BumbleBee, between two rules | **3** (sink → local `ChunkCollection` → global `ChunkCollection` → `PredicateTables`), plus a **4th** when the next rule scans it back |
| ours, streaming (`Filter` → `Projection` → join probe → …) | **0** — `Reference` / `Slice` all the way |
| ours, at a breaker | **1** (scatter into the thread-local structure) + the `Combine`. For the aggregate, `Combine` re-inserts *distinct groups*, not input rows. For the result collector, `Combine` is a `std::move`: **zero** additional row copies |

### Why thread-local + `Combine` beats one shared locked structure

1. **Contention.** A shared hash table locks or CASes **per row** — at N threads that synchronization *is*
   the cost of the operator. Thread-local tables take **zero** locks during `Sink`; the only
   synchronization in the whole query is `N × RADIX_PARTITIONS` lock acquisitions at `Combine`.
2. **Coherence traffic.** N threads CAS-ing into one open-addressed table share cache lines by
   construction; the hot lines ping-pong between cores. Private tables stay in each core's L2.
3. **Resizing.** A shared table resizes under a global lock while everyone stalls. Local tables resize
   independently, and the final global size is known *exactly* at `Combine` time.

Because a row's partition is a pure function of its hash, partition *p* of every thread's table merges into
global partition *p* with **no cross-partition synchronization** — which gives 64-way parallelism in the
merge *and*, the part people forget, in the **readout**: the source hands out one partition per task,
lock-free. **BumbleBee's `PartitionedAggHT` already does exactly this, and it is right — we preserve it
conceptually verbatim.** The one change: its `finalize()` runs single-threaded on the coordinator's
`FinalizeTask`; here it becomes the first override of `FinalizeMaxThreads()`. **[D-9]**

---

## 8. `DatabaseInstance` / `ClientContext` / `ThreadContext` / `ExecutionContext` [D-10]

BumbleBee's `ThreadContext` holds a profiler and a `ClientContext &` — a *thread* owning a *client*, which
is backwards — and there is no `ExecutionContext` at all (`PhysicalRuleExecutor` carries an explicit
`// TODO Execution context with a profiler`). Split on **lifetime and sharing**:

| class | lifetime | shared by | holds | concurrently mutable? |
|---|---|---|---|---|
| `DatabaseInstance` | process | everything | `Catalog`, `BufferPoolManager`, `TaskScheduler`, global config | yes — internally synchronized |
| `ClientContext` | one session | every query of that session, **sequentially** | `DatabaseInstance &`, `max_threads_`, the `Allocator`, the active `Executor` | one query at a time |
| `ThreadContext` | **one task** | exactly one worker thread | the per-operator profiler; a thread-local arena | **no — single-threaded by construction. That is the entire reason it exists: anything here needs no lock.** |
| `ExecutionContext` | one `Execute` / `GetData` / `Sink` call | passed by reference, never stored | `ClientContext &`, `ThreadContext &`, `Pipeline *` | it *is* a bundle of references |

`ExecutionContext` exists so that adding a per-task facility later — a memory budget, an interrupt token, a
spill directory — is **a field on `ThreadContext`, not a signature change rippling through fifteen
operators**. `Executor` owns one `ThreadContext` per task and merges their profilers after the query (the
same shape as `Scheduler::getAtomProfiler()`, which is a good idea BumbleBee already has).

---

## 9. Logical → physical lowering

`src/include/execution/physical_plan_generator.h`, mirroring the existing `Planner` / `Optimizer` shape: a
class holding a `ClientContext &` and a `const Catalog &`, with one private method per `PlanType`.

```cpp
auto CreatePlan(const AbstractPlanNodeRef &plan) -> std::unique_ptr<PhysicalOperator>;
```

This is the milestone the `abstract_plan.h` header comment promised — *"a later milestone lowers this tree
into physical operators by pattern-matching on `PlanType`; keep it that way and do not smuggle execution
strategy in here"* — and it vindicates milestone 1's two abstinences: `AggregationPlanNode` carries no
`AggregateKey` / `AggregateValue` (that is a hash table's business, decided **here**), and
`AbstractExpression` carries no `Evaluate` (the batch-vs-tuple decision is made **here**, by compiling the
tree into an `ExpressionExecutor`).

**Ownership diverges from the plan tree, correctly [D-11]:** the physical tree owns children by
`unique_ptr`, and `Pipeline` holds **raw `const PhysicalOperator *`**. A *plan* node is aliased across
optimizer rewrites and wants `shared_ptr`; a *physical* operator is created once, owned once by the
`Executor`, and referenced from many pipelines. `PhysicalRule` owning a flat `vector<unique_ptr>` of atoms
is precisely why one atom cannot appear in two rules — the structural form of fatal flaw #1.

**What lowering decides** (the payoff for keeping strategy out of `PlanType`):

1. **Which scan.** `SeqScanPlanNode` → `PhysicalTableScan` if the `TableInfo` has row storage, or
   `PhysicalParquetScan` if it points at a Parquet file. **The plan node cannot know — it holds only a
   `table_oid_t`.** The generator resolves it against the catalog. This alone justifies the lowering step
   existing.
2. **Which join, and which side builds.** `HashJoinPlanNode` → `PhysicalHashJoin`; later a
   `PhysicalPieceWiseSortMergeJoin` for range predicates (BumbleBee has one). *Stated honestly:* a
   build-side swap must swap `left_key_expressions_` / `right_key_expressions_` **and** invert the output
   column order, because `HashJoinPlanNode`'s contract is "output = left schema then right schema". Do that
   once, in `PhysicalHashJoin`'s output map.
3. **Whether the aggregate is partitioned.** `group_bys_.empty()` → `PhysicalUngroupedAggregate`
   (`MaxThreads() == 1` as a source); otherwise `PhysicalHashAggregate` over a `PartitionedAggHT`.
4. **What the pushed-down filter means** — a post-gather `Select` on a row scan; *also* a row-group
   statistics pruner on Parquet (§6).
5. **The root.** `SELECT` is wrapped in a `PhysicalResultCollector`. Nothing in `PlanType` corresponds to
   one, and that is correct — it is a purely physical concept.

**What it needs and does not have: cardinality estimates.** The only signal in the tree today is
`Optimizer::EstimatedCardinality`, which reads a `_1k` / `_10k` suffix off the table name. So the
build-side swap and the join choice are **heuristic**, and `estimated_cardinality_` is a guess used to size
the initial hash tables. Say so; do not dress it up as a cost model.

**Expressions** stay as the `AbstractExpressionRef`s the plan gave us, and are *compiled* into an
`ExpressionExecutor` **inside the `LocalOperatorState`** — because an `ExpressionExecutor` owns
intermediate `Vector`s and is therefore per-thread by construction.

---

## 10. File layout

CMake globs `src/*.cpp` recursively, so **no CMake change is needed.**

```
src/include/execution/
  physical_operator.h            # the base class, 4 result enums, 6 state base classes   (§1)
  physical_plan_generator.h      # PlanType tree -> PhysicalOperator tree                 (§9)
  execution_context.h                                                                     (§8)
  expression_executor.h                                                                  [B2]
  operator/
    scan/{physical_table_scan,physical_parquet_scan,physical_values}.h                    (§6)
    filter/physical_filter.h   projection/physical_projection.h   order/physical_limit.h
    join/{physical_hash_join,physical_nested_loop_join}.h                 # sink + operator
    aggregate/{physical_hash_aggregate,physical_ungrouped_aggregate}.h    # sink + source
    order/{physical_sort,physical_top_n}.h                                # sink + source
    persistent/{physical_insert,physical_update,physical_delete}.h
    helper/physical_result_collector.h                                    # the root sink
  join/join_hash_table.h  aggregate/partitioned_agg_ht.h  sort/{sort_key_encoding,top_n_heap}.h   [B5]

src/include/parallel/
  task.h  task_scheduler.h  pipeline.h  pipeline_builder.h  pipeline_executor.h
  executor.h  thread_context.h                                                        (§2–§5)

src/include/main/     client_context.h  database_instance.h  query_result.h              [B8]
src/include/storage/  table/{table_heap,tuple,rid}.h  buffer/buffer_pool_manager.h
                      row_layout.h  row_operations.h                                 [B3, B4]

src/execution/ , src/parallel/ , src/storage/     # mirroring .cpp files
test/unit/execution/ , test/unit/parallel/
```

`common/config.h` gains `MORSEL_SIZE`, `MORSEL_PAGES`, `RADIX_PARTITIONS`, `DEFAULT_THREAD_COUNT`.
`common/exception.h` gains an `ExecutionException` (the `ExceptionType::EXECUTION` enum value already
exists with no class behind it).

---

## 11. Making it debuggable: printing the physical plan and the pipelines

This is not a nicety — it is the difference between a tractable engine and an untractable one. Two of the
three trees in this design are **invisible at runtime**, and both fail *silently*:

- A **lowering** bug (wrong build side, aggregate not partitioned, a scan lowered to the wrong source)
  produces a physical tree nobody ever looks at.
- A **pipeline-construction** bug is worse. If `BuildPipelines` attaches an operator to the wrong pipeline,
  or forgets a dependency edge, the symptom is a *wrong answer* or a *hang* — a probe pipeline that ran
  before its hash table was built reads an empty table and silently returns zero rows. There is no crash
  and no assertion to catch it. **You cannot debug that without seeing the DAG.**

So both get printed, and both reuse machinery that already exists.

### The physical plan → `EXPLAIN`

`ExplainOptions` in `src/include/binder/statement/explain_statement.h` is already a bitmask
(`BINDER=1, PLANNER=2, OPTIMIZER=4, SCHEMA=8`). Add two values:

```cpp
enum ExplainOptions : uint8_t {
  INVALID = 0, BINDER = 1, PLANNER = 2, OPTIMIZER = 4, SCHEMA = 8,
  /** Print the physical operator tree (post-lowering). */
  PHYSICAL = 16,
  /** Print the pipeline DAG (post-BuildPipelines). */
  PIPELINES = 32,
};
```

and two option keywords in `Binder::BindExplain` (`src/binder/bind_select.cpp:800`), alongside the
existing `p` / `b` / `o` / `s`:

```cpp
if (strcmp(temp->defname, "physical")  == 0 || strcmp(temp->defname, "x") == 0) { opts |= PHYSICAL;  }
if (strcmp(temp->defname, "pipelines") == 0 || strcmp(temp->defname, "l") == 0) { opts |= PIPELINES; }
```

`PhysicalOperator` gets the **same `ToString` shape the plan tree already uses** — `AbstractPlanNode`
has a public `ToString(bool with_schema)` plus a protected `PlanNodeToString()` that each node overrides,
and `ChildrenToString(indent)` to recurse. Mirror it exactly (`ToString` / `ParamsToString` /
`ChildrenToString`), so the two trees print in the same idiom and the `fmt::formatter` specializations at
the bottom of `abstract_plan.h` can be copied across. `HandleExplainStatement`
(`src/bumblebee_instance.cpp:91`) grows one more `if` block, exactly like the four already there.

```
> EXPLAIN (o, x) SELECT a.x, SUM(b.y) FROM a JOIN b ON a.k=b.k WHERE a.x>5 GROUP BY a.x ORDER BY 2 DESC LIMIT 10;

=== OPTIMIZER ===
TopN { n=10, order_by=[#0.1 desc] }
  Agg { group_by=[#0.0], aggregates=[sum(#0.3)] }
    HashJoin { left_key=[#0.1], right_key=[#1.0], type=Inner }
      SeqScan { table=a, filter=(#0.1 > 5) }
      SeqScan { table=b }

=== PHYSICAL ===
ResultCollector
  TopN { n=10, order_by=[#0.1 desc], sink+source }
    HashAggregate { group_by=[#0.0], aggregates=[sum(#0.3)], partitions=64, sink+source }
      HashJoin { keys=[#0.1 = #1.0], type=Inner, build=child[0], sink+operator }
        TableScan { table=a, storage=row, filter=(#0.1 > 5), cols=[k,x] }      <-- row pages
        ParquetScan { file=b.parquet, cols=[k,y], row_groups=12 }              <-- columnar
```

Two things that view alone would have caught: **which** scan each table lowered to (row pages vs Parquet —
invisible in the logical plan, which holds only a `table_oid_t`), and **which side builds** after a
cardinality-driven swap.

### The pipeline DAG → `\pipelines`

The shell already dispatches backslash meta-commands (`src/main/main.cpp:67`), so:

```
\pipelines <sql>
```

is a thin alias that rewrites to `EXPLAIN (pipelines) <sql>` and prints only that block. That gives the
distinct command without a second statement type, a second parse path, or any new binder work — the
pipeline dump *needs* the query bound, planned, optimized and lowered anyway, which is precisely what the
`EXPLAIN` path already does. It stops short of `Executor::ExecuteQuery`, so nothing runs.

`Pipeline::ToString()` prints one pipeline; `Executor::PipelinesToString()` prints them in dependency
order with their edges:

```
> \pipelines SELECT a.x, SUM(b.y) FROM a JOIN b ON a.k=b.k WHERE a.x>5 GROUP BY a.x ORDER BY 2 DESC LIMIT 10;

=== PIPELINES ===
P0  deps: —          max_threads: 8
      source    TableScan(a) [filter #0.1 > 5]
      sink      HashJoin(build)

P1  deps: [P0]       max_threads: 12
      source    ParquetScan(b)
      operator  HashJoin(probe)
      sink      HashAggregate

P2  deps: [P1]       max_threads: 64
      source    HashAggregate
      sink      TopN

P3  deps: [P2]       max_threads: 1     (TopN is order-preserving)
      source    TopN
      sink      ResultCollector
```

Everything a pipeline bug shows up in is on that page: the **dependency edges** (a missing one is a race,
an extra one is a lost parallelism), which operators are **streaming** vs which are **sinks** (an operator
that should be streaming appearing as a sink is a spurious materialization), and **`max_threads`** per
pipeline — where an unexpected `1` immediately tells you which `ParallelSink()` / `ParallelOperator()` /
`IsOrderPreserving()` hint collapsed your parallelism, which is otherwise a pure-mystery performance bug.

### Later: `EXPLAIN ANALYZE`

The same tree, annotated with what actually happened. The data is already being collected — `Executor`
owns one `ThreadContext` per task, each carrying a per-operator profiler, and merges them after the query
(§8). So `EXPLAIN ANALYZE` is a *formatting* job on top of the `PHYSICAL` printer: rows in / rows out /
wall time per operator, plus per-pipeline task counts and morsel counts, which is how you find skew (one
task doing 90% of the morsels). Not v1, but the plumbing exists from day one and should not be
retrofitted.

---

## Every deliberate divergence from BumbleBee

| # | BumbleBee | BumbleBeeDB | why |
|---|---|---|---|
| D-1 | one `AtomResultType` | four result enums | `FINISHED` has three distinct meanings and three distinct propagation rules |
| D-2 | `void finalize()` | `SinkFinalizeType Finalize()` | an empty inner-join build lets the probe pipeline skip its scan entirely. **An optimization, not a requirement** — the scheduler does not need it, and v1 may ship without it (§1, §5.4) |
| D-3 | global state, `combine`, `finalize` **only** for source and sink | **every** operator, in every role | removes the need to lift a build into its own rule with a `NopeOutput` sink — **kills fatal flaw #1 at the root** |
| D-4 | global state lives on `PhysicalRule` | lives on the `Executor`, in flat vectors indexed by a dense `PhysicalOperator::id_` — not a hash map, and not a `mutable` member on the operator as DuckDB does | one operator, two pipelines, **one** shared sink state — structurally, because it is one slot. And the physical plan stays immutable, so it can be re-executed |
| D-5 | priority buckets, hard barriers, a coordinator polling a 100 ms semaphore and rescanning the bucket | counter DAG; the last task finalizes **inline** and enqueues its own dependents; the client thread is a worker | zero coordination round-trips, no idle thread |
| D-6 | two independent builds → different priority buckets → **sequential** | siblings both have zero dependencies → both scheduled at t=0 | there is no global barrier anywhere |
| D-7 | `finalize` is one task, single-threaded, single-stage, forever | `finalize_tasks_remaining_` / `finalize_stage_` exist from day one, sized 1×1; one `if` is the seam | a parallel 64-partition agg finalize is a drop-in |
| D-8 | `lock_guard` per morsel handout | `fetch_add` on a monotonic index | a mutex on a monotonic range is a needless serialization point |
| D-9 | `PartitionedAggHT::finalize()` single-threaded | the first override of `FinalizeMaxThreads()` | the *design* was right; only the *scheduling* was wrong |
| D-10 | `ThreadContext` holds a `ClientContext &`; no `ExecutionContext` | `ExecutionContext{ClientContext&, ThreadContext&, Pipeline*}` | a thread does not own a client; operator signatures never grow again |
| D-11 | `PhysicalRule` owns a flat `vector<unique_ptr>` → an atom cannot be in two rules | the tree owns by `unique_ptr`; `Pipeline` holds raw `const` pointers | the structural form of fatal flaw #1 |
| D-12 | output chunk never `Reset()`; empty chunks pushed to the sink; re-entry only after a full traverse; `executeCombine()` declared-never-defined; `atomResults_` written-never-read | explicit LIFO invariant, per-boundary chunks, `Reset()` before every call, empty-chunk short-circuit, asserted `HAVE_MORE_OUTPUT ⇒ size > 0` | §4 |
| D-13 | 3 copies of every row between rules (+ a 4th on read-back) | 0 between streaming operators; 1 at a breaker; the collector's `Combine` is a `std::move` | §7 |

---

## Honest prerequisites — none of this can run without them

| | what | why it blocks |
|---|---|---|
| **B1** | vector kernels: comparison / arith / hash / cast + `UnaryExecution` / `BinaryExecution` (`plan.md` steps 6–7, never landed) | `DataChunk::Hash()` throws today ⇒ **no hash join, no GROUP BY, no DISTINCT** |
| **B2** | `ExpressionExecutor` (`AbstractExpression` → `Vector`, plus a `Select` returning a `SelectionVector`) | `Filter`, `Projection`, join conditions, group keys — everything |
| **B3** | row storage: pages, `Tuple`, `RID`, `TableHeap`, buffer pool | `TableInfo` is `(oid, name, Schema)`; **a table can hold zero rows** |
| **B4** | `RowLayout` + `RowOperations::{Gather,Scatter,Match}` — the row↔vector bridge (port from BumbleBee) | `PhysicalTableScan`, and every row-layout hash table |
| **B5** | port `PRLHashTable`, `PartitionedAggHT`, `TopNHeap`, `SortKeyEncoding` | the sinks |
| **B6** | Parquet reader (BumbleBee has one) | `PhysicalParquetScan` |
| **B7** | vectorized aggregate functions (the `AggregationType` enum exists; the state functions do not) | `PhysicalHashAggregate` |
| **B8** | `ClientContext` / `DatabaseInstance` (today: `bumblebee_instance.h`) | everything takes a `ClientContext &` |

A sane ordering among them: **B1 → B2** before any operator; **B3 + B4** before `PhysicalTableScan`;
**B5 + B7** before the breakers; **B6** last.