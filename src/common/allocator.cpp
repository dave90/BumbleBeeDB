//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// allocator.cpp
//
// Identification: src/common/allocator.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "common/allocator.h"

namespace bumblebee {

AllocatedData::AllocatedData(Allocator &allocator, data_ptr_t pointer, idx_t allocated_size)
    : allocator_(allocator), pointer_(pointer), allocated_size_(allocated_size) {}

AllocatedData::~AllocatedData() {
  // Free the underlying allocation. Without this, every owner that relies on destruction
  // leaks its memory until process exit.
  Reset();
}

void AllocatedData::Reset() {
  if (pointer_ == nullptr) {
    return;
  }
  allocator_.FreeData(pointer_, allocated_size_);
  pointer_ = nullptr;
}

Allocator::Allocator()
    : alloc_function_(DefaultAllocate), free_function_(DefaultFree), realloc_function_(DefaultReallocate) {}

Allocator::Allocator(allocate_function_ptr_t allocate_function, free_function_ptr_t free_function,
                     reallocate_function_ptr_t reallocate_function, private_alloc_data_ptr_t private_data)
    : alloc_function_(allocate_function),
      free_function_(free_function),
      realloc_function_(reallocate_function),
      private_data_(std::move(private_data)) {}

auto Allocator::AllocateData(idx_t size) -> data_ptr_t { return alloc_function_(private_data_.get(), size); }

void Allocator::FreeData(data_ptr_t pointer, idx_t size) { free_function_(private_data_.get(), pointer, size); }

auto Allocator::ReallocateData(data_ptr_t pointer, idx_t size) -> data_ptr_t {
  return realloc_function_(private_data_.get(), pointer, size);
}

}  // namespace bumblebee
