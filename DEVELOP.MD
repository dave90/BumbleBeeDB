# Coding Style Guide

## Naming

| Entity | Case | Example                    |
|---|---|----------------------------|
| Class | `CamelCase` | `BufferPoolManager`        |
| Struct | `CamelCase` | `FrameHeader`              |
| Enum | `CamelCase` | `AccessType`               |
| Union | `CamelCase` | `ValueUnion`               |
| Function / method | `CamelCase` | `GetData()`, `FetchPage()` |
| Namespace | `lower_case` | `bumblebee`                |
| Variable | `lower_case` | `frame_id`                 |
| Member variable | `lower_case` with trailing `_` | `frame_id_`, `data_`       |
| Global constant | `UPPER_CASE` | `PAGE_SIZE`         |

## Includes

- Use `#pragma once` for header guards (not `#ifndef` guards).
- Include blocks are preserved as written — group standard library headers,
  then project headers, and let the ordering stand.
- Project headers use quoted, path-relative includes:
  `#include "buffer/buffer_pool_manager.h"`.

## File header

Every source and header file begins with the BumbleBee banner comment:

## Functions

- Use **trailing return type** syntax (enforced by `modernize-use-trailing-return-type`):

  ```cpp
  auto FrameHeader::GetData() const -> const char * { return data_.data(); }
  ```

- Prefer modern C++ (`modernize-*` checks are on): `auto`, range-based loops,
  `nullptr`, `using` aliases over `typedef`, etc.

## File-local helpers

Everything lives in `namespace bumblebee`. A helper that a `.cpp` does not export is marked
`static` rather than wrapped in an anonymous namespace:

```cpp
namespace bumblebee {

/** @brief Round up to the next multiple of 8, so the next row starts aligned. */
static constexpr auto AlignValue(idx_t n) -> idx_t { return (n + 7) & ~idx_t(7); }
```

`static` gives the same internal linkage, so two `.cpp` files may each define their own
`MakeSchema` or `PartitionOf` with no collision. Namespace-scope `const` and `constexpr` objects
are already internal and need no `static`.

Types are the exception. A type has no `static` spelling for internal linkage ([basic.link]), and
nesting it inside a class does not help — a nested type of an externally linked class is itself
externally linked. So a file-local **type** does get external linkage, and its name must therefore
be unique across the whole library: prefer `HashAggGlobalSinkState` over `GlobalSinkState`,
`ConcurrentTreeFixture` over `TreeFixture`. Two same-named-but-different structs in two `.cpp`
files are an ODR violation the linker resolves silently by picking one.

The single case where an anonymous namespace stays is a type used as a **template policy
argument** in a file that instantiates over a large type cross-product — currently only
`vector_arith.cpp` and `vector_comparison.cpp`. Promoting those functors flips every instantiation
naming them from internal to weak external linkage, which forces an out-of-line body per
instantiation instead of inlining into the single call site: measured at +56% / +54% object code
and ~29,000 extra exported symbols for those two files. Each such block carries a comment saying
so. Everywhere else, promoting a type costs nothing measurable.

## Documentation comments

Use Doxygen-style `/** ... */` blocks with `@brief`, `@param`, and `@return`:

```cpp
/**
 * @brief Get a raw const pointer to the frame's data.
 *
 * @return const char* A pointer to immutable data that the frame stores.
 */
auto FrameHeader::GetData() const -> const char * { return data_.data(); }
```
