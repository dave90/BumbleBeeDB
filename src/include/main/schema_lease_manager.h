//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// schema_lease_manager.h
//
// Identification: src/include/main/schema_lease_manager.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <condition_variable>  // NOLINT
#include <cstddef>
#include <mutex>  // NOLINT

namespace bumblebee {

/**
 * @brief Database-wide reader/writer gate for catalog and storage lifetime.
 *
 * Unlike a thread-owned shared mutex, these move-only leases may be released by the lifecycle or GC
 * path that closes an idle connection. Waiting exclusive leases take priority over new shared leases.
 */
class SchemaLeaseManager {
 public:
  class SharedLease {
   public:
    SharedLease() = default;
    ~SharedLease();

    SharedLease(const SharedLease &) = delete;
    auto operator=(const SharedLease &) -> SharedLease & = delete;
    SharedLease(SharedLease &&other) noexcept;
    auto operator=(SharedLease &&other) noexcept -> SharedLease &;

    void unlock();
    explicit operator bool() const { return owner_ != nullptr; }

   private:
    friend class SchemaLeaseManager;
    explicit SharedLease(SchemaLeaseManager *owner) : owner_(owner) {}

    SchemaLeaseManager *owner_{nullptr};
  };

  class ExclusiveLease {
   public:
    ExclusiveLease() = default;
    ~ExclusiveLease();

    ExclusiveLease(const ExclusiveLease &) = delete;
    auto operator=(const ExclusiveLease &) -> ExclusiveLease & = delete;
    ExclusiveLease(ExclusiveLease &&other) noexcept;
    auto operator=(ExclusiveLease &&other) noexcept -> ExclusiveLease &;

    void unlock();
    explicit operator bool() const { return owner_ != nullptr; }

   private:
    friend class SchemaLeaseManager;
    explicit ExclusiveLease(SchemaLeaseManager *owner) : owner_(owner) {}

    SchemaLeaseManager *owner_{nullptr};
  };

  auto AcquireShared() -> SharedLease;
  auto AcquireExclusive() -> ExclusiveLease;

 private:
  void ReleaseShared();
  void ReleaseExclusive();

  std::mutex mutex_;
  std::condition_variable cv_;
  size_t shared_leases_{0};
  size_t waiting_exclusive_{0};
  bool exclusive_lease_{false};
};

}  // namespace bumblebee
