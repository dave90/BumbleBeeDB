# Coding Style Guide

## Naming

| Entity | Case | Example |
|---|---|---|
| Class | `CamelCase` | `BufferPoolManager` |
| Struct | `CamelCase` | `FrameHeader` |
| Enum | `CamelCase` | `AccessType` |
| Union | `CamelCase` | `ValueUnion` |
| Function / method | `CamelCase` | `GetData()`, `FetchPage()` |
| Namespace | `lower_case` | `bustub` |
| Variable | `lower_case` | `frame_id` |
| Member variable | `lower_case` with trailing `_` | `frame_id_`, `data_` |
| Global constant | `UPPER_CASE` | `BUSTUB_PAGE_SIZE` |

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
