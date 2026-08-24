//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// schema_lease_manager.cpp
//
// Identification: src/main/schema_lease_manager.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "main/schema_lease_manager.h"

#include <utility>

#include "common/macros.h"

namespace bumblebee {

SchemaLeaseManager::SharedLease::~SharedLease() { unlock(); }

SchemaLeaseManager::SharedLease::SharedLease(SharedLease &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

auto SchemaLeaseManager::SharedLease::operator=(SharedLease &&other) noexcept -> SharedLease & {
  if (this != &other) {
    unlock();
    owner_ = std::exchange(other.owner_, nullptr);
  }
  return *this;
}

void SchemaLeaseManager::SharedLease::unlock() {
  if (owner_ == nullptr) {
    return;
  }
  auto *owner = std::exchange(owner_, nullptr);
  owner->ReleaseShared();
}

SchemaLeaseManager::ExclusiveLease::~ExclusiveLease() { unlock(); }

SchemaLeaseManager::ExclusiveLease::ExclusiveLease(ExclusiveLease &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

auto SchemaLeaseManager::ExclusiveLease::operator=(ExclusiveLease &&other) noexcept -> ExclusiveLease & {
  if (this != &other) {
    unlock();
    owner_ = std::exchange(other.owner_, nullptr);
  }
  return *this;
}

void SchemaLeaseManager::ExclusiveLease::unlock() {
  if (owner_ == nullptr) {
    return;
  }
  auto *owner = std::exchange(owner_, nullptr);
  owner->ReleaseExclusive();
}

auto SchemaLeaseManager::AcquireShared() -> SharedLease {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [&] { return !exclusive_lease_ && waiting_exclusive_ == 0; });
  shared_leases_++;
  return SharedLease(this);
}

auto SchemaLeaseManager::AcquireExclusive() -> ExclusiveLease {
  std::unique_lock lock(mutex_);
  waiting_exclusive_++;
  cv_.wait(lock, [&] { return !exclusive_lease_ && shared_leases_ == 0; });
  waiting_exclusive_--;
  exclusive_lease_ = true;
  return ExclusiveLease(this);
}

void SchemaLeaseManager::ReleaseShared() {
  std::lock_guard lock(mutex_);
  BUMBLEBEE_ASSERT(shared_leases_ > 0, "releasing a schema lease that is not held");
  shared_leases_--;
  if (shared_leases_ == 0) {
    cv_.notify_all();
  }
}

void SchemaLeaseManager::ReleaseExclusive() {
  std::lock_guard lock(mutex_);
  BUMBLEBEE_ASSERT(exclusive_lease_, "releasing an exclusive schema lease that is not held");
  exclusive_lease_ = false;
  cv_.notify_all();
}

}  // namespace bumblebee
