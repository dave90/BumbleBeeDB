You are a senior C++ software architect and performance-focused engineer.

Your task is to refactor the entire C++ codebase to improve its structure, readability, maintainability, consistency, and correctness while preserving all existing behavior, optimizations, and performance characteristics.

## Core Requirements

### 1. Preserve behavior

* Do not change business logic or externally observable behavior.
* Preserve serialization formats, configuration formats, file formats, command-line interfaces, network protocols, and database behavior.
* Do not introduce undefined behavior, implementation-defined dependencies, race conditions, lifetime issues, or regressions.

### 2. Preserve or improve performance

Performance must not be downgraded.

* Keep all existing optimizations unless they are proven unnecessary.
* Do not replace optimized code with cleaner but slower code.
* Preserve or improve algorithmic complexity.
* Avoid unnecessary:

  * heap allocations
  * memory copies
  * temporary objects
  * virtual calls
  * locking
  * atomic operations
  * system calls
  * container reallocations
  * string conversions
  * cache misses
  * branches in hot paths
* Preserve move semantics, perfect forwarding, copy elision, small-buffer optimizations, memory pooling, vectorization, and cache-friendly data layouts.
* Do not add abstractions that prevent compiler inlining, devirtualization, constant folding, SIMD vectorization, or other compiler optimizations.
* Use benchmarks or existing performance tests to verify performance-sensitive changes.
* When uncertain, prefer the implementation with established performance characteristics.

### 3. Remove anonymous namespaces

Remove all anonymous namespaces of the following form:

```cpp
namespace {
    // declarations and definitions
}
```

Use the bumblebee namespace instead and if conflicts try to put the functions as static member of a class


### 4. Improve C++ code quality

* Apply RAII consistently.
* Prefer deterministic ownership and lifetime management.
* Use smart pointers only when ownership semantics require them.
* Prefer stack allocation where appropriate.
* Avoid unnecessary shared ownership.
* Make ownership explicit.
* Eliminate resource leaks, dangling references, invalid iterators, and lifetime errors.
* Use `const` and `constexpr` correctly.
* Use `noexcept` only when the function genuinely cannot throw.
* Preserve exception guarantees.
* Prefer standard library facilities when they are equally or more efficient.
* Avoid unnecessary macros.
* Replace unsafe C-style casts with appropriate C++ casts.
* Avoid unnecessary use of raw `new` and `delete`.
* Preserve alignment, packing, aliasing, and object-layout requirements.
* Avoid changing POD, standard-layout, or trivially-copyable properties unless explicitly justified.
* Preserve template behavior and overload resolution.
* Do not introduce accidental implicit conversions.
* Mark constructors `explicit` where appropriate, provided compatibility is preserved.
* Use `[[nodiscard]]`, `[[maybe_unused]]`, and other attributes only where useful and compatible.
* Keep header dependencies minimal.
* Reduce compilation overhead where practical.
* Avoid moving implementation details into headers unless required for templates, `constexpr`, or inlining.

### 5. Improve architecture and maintainability

* Apply SOLID, DRY, KISS, and YAGNI where appropriate.
* Improve naming and code organization.
* Reduce unnecessary coupling.
* Increase cohesion.
* Remove dead code and obsolete comments.
* Eliminate duplicated logic.
* Simplify overly complex functions.
* Avoid speculative abstractions.
* Do not create interfaces, factories, inheritance hierarchies, or wrapper classes unless they solve a real problem.
* Prefer composition over inheritance where appropriate.
* Keep hot-path code direct and efficient.
* Follow the project’s existing style unless a new style is applied consistently.

## Concurrency Requirements

For concurrent or multithreaded code:

* Preserve thread-safety guarantees.
* Preserve memory-ordering semantics.
* Do not weaken or strengthen atomic memory orders without justification.
* Do not introduce additional locks or contention.
* Preserve lock ordering.
* Avoid deadlocks, livelocks, priority inversion, false sharing, and data races.
* Preserve thread-local storage behavior.
* Do not replace lock-free or wait-free algorithms with blocking implementations.
* Validate concurrency-sensitive changes with sanitizers or existing stress tests where available.

## Build and Compatibility Requirements

* Use the C++ standard configured by the project.
* Do not introduce features from a newer C++ standard unless explicitly allowed.
* Preserve support for all currently supported:

  * compilers
  * operating systems
  * architectures
  * build configurations
  * sanitizers
* Preserve Debug, Release, and other relevant build modes.
* Do not suppress compiler warnings merely to make the build pass.
* Do not disable tests, assertions, sanitizers, or static-analysis checks.

## Validation Requirements

After each logical refactoring:

1. Build the affected targets.
2. Run the relevant unit tests.
3. Run integration tests.
4. Run end-to-end tests.
7. Run benchmarks or performance tests for performance-sensitive code.
8. Check for warnings, crashes, leaks, undefined behavior, and regressions.
9. Confirm that symbol visibility and linkage remain correct.
10. Confirm that the anonymous namespaces have been removed safely.
11. Run the benchamrks and check there are no regressions.

All existing tests must pass:

* Unit tests
* End-to-end tests
* Benchamrks

Do not modify, weaken, skip, or delete tests merely to make the refactoring pass.

Tests may only be changed when:

* the existing test itself is incorrect,
* the refactoring requires a non-behavioral test infrastructure update, or
* additional coverage is needed.

Any test modification must be clearly justified.

## Completion Criteria

The task is complete only when:

* The codebase is cleaner and more maintainable.
* All anonymous namespaces have been removed safely.
* Internal-linkage requirements remain intact.
* No unintended symbols have been externally exposed.
* No ODR or symbol-collision issues have been introduced.
* Existing behavior is unchanged.
* Public APIs and ABI are preserved where required.
* Performance is equal to or better than before.
* The project builds without new warnings or errors.
* All unit, integration, regression, performance, and E2E tests pass.
* No tests or checks have been disabled.
* No known bugs or regressions remain.

If a requested change cannot be completed safely without risking behavior, ABI, correctness, or performance, do not force the change. Explain the specific risk and implement the safest alternative that satisfies the requirements as closely as possible.
