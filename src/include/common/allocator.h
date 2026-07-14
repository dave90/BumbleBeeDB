//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// allocator.h
//
// Identification: src/include/common/allocator.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdlib>
#include <memory>

#include "common/config.h"

namespace bumblebee {

class Allocator;

/** Opaque state an allocator implementation carries along with its function pointers. */
struct PrivateAllocatorData {
  virtual ~PrivateAllocatorData() = default;
};

using private_alloc_data_ptr_t = std::unique_ptr<PrivateAllocatorData>;

using allocate_function_ptr_t = data_ptr_t (*)(PrivateAllocatorData *private_data, idx_t size);
using free_function_ptr_t = void (*)(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size);
using reallocate_function_ptr_t = data_ptr_t (*)(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size);

/** An RAII handle over a block of memory obtained from an Allocator. */
class AllocatedData {
 public:
  AllocatedData(Allocator &allocator, data_ptr_t pointer, idx_t allocated_size);
  ~AllocatedData();

  /** @return A pointer to the allocated bytes. */
  auto Get() -> data_ptr_t { return pointer_; }

  /** @return A const pointer to the allocated bytes. */
  auto Get() const -> const_data_ptr_t { return pointer_; }

  /** @return The number of bytes allocated. */
  auto GetSize() const -> idx_t { return allocated_size_; }

  /** @brief Return the memory to the allocator. Idempotent. */
  void Reset();

 private:
  Allocator &allocator_;
  data_ptr_t pointer_;
  idx_t allocated_size_;
};

using alloc_data_ptr_t = std::unique_ptr<AllocatedData>;

/**
 * An indirection over malloc/free/realloc, so that a subsystem can be handed a different
 * memory source (an arena, a tracked pool) without changing its code.
 */
class Allocator {
 public:
  Allocator();
  Allocator(allocate_function_ptr_t allocate_function, free_function_ptr_t free_function,
            reallocate_function_ptr_t reallocate_function, private_alloc_data_ptr_t private_data);

  /** @brief Allocate `size` bytes. */
  auto AllocateData(idx_t size) -> data_ptr_t;

  /** @brief Free `size` bytes previously obtained from this allocator. */
  void FreeData(data_ptr_t pointer, idx_t size);

  /** @brief Resize a previous allocation to `size` bytes. */
  auto ReallocateData(data_ptr_t pointer, idx_t size) -> data_ptr_t;

  /** @brief Allocate `size` bytes, wrapped in an RAII handle. */
  auto Allocate(idx_t size) -> alloc_data_ptr_t {
    return alloc_data_ptr_t(new AllocatedData(*this, AllocateData(size), size));
  }

  static auto DefaultAllocate(PrivateAllocatorData *private_data, idx_t size) -> data_ptr_t {
    return static_cast<data_ptr_t>(malloc(size));
  }
  static void DefaultFree(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size) { free(pointer); }
  static auto DefaultReallocate(PrivateAllocatorData *private_data, data_ptr_t pointer, idx_t size) -> data_ptr_t {
    return static_cast<data_ptr_t>(realloc(pointer, size));
  }

  /** @return The implementation-private state of this allocator. */
  auto GetPrivateData() -> PrivateAllocatorData * { return private_data_.get(); }

 private:
  allocate_function_ptr_t alloc_function_;
  free_function_ptr_t free_function_;
  reallocate_function_ptr_t realloc_function_;

  private_alloc_data_ptr_t private_data_;
};

}  // namespace bumblebee
