//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// database_instance.h
//
// Identification: src/include/main/database_instance.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <condition_variable>  // NOLINT
#include <filesystem>
#include <memory>
#include <mutex>  // NOLINT
#include <vector>

#include "catalog/catalog.h"
#include "concurrency/transaction_manager.h"
#include "database.h"
#include "main/database_config.h"
#include "main/resource_manager.h"
#include "main/schema_lease_manager.h"
#include "storage/buffer/buffer_pool_manager.h"
#include "storage/disk/memory_disk_manager.h"

namespace bumblebee {

class Connection;
class DurablePathLock;

/**
 * @brief Shared owner of one BumbleBeeDB engine and all database-wide native facilities.
 *
 * A `DatabaseInstance` owns either durable storage or the complete in-memory backing stack. It has
 * no session or transaction state; each `Connection` owns those independently while sharing this
 * object. Lifecycle admission and schema, worker, and memory coordination are layered onto this owner.
 */
class DatabaseInstance {
 public:
  enum class LifecycleState { OPEN, CLOSING, CLOSED };

  /** @brief RAII admission token held for the complete duration of one public database operation. */
  class OperationToken {
   public:
    OperationToken() = default;
    ~OperationToken();
    OperationToken(const OperationToken &) = delete;
    auto operator=(const OperationToken &) -> OperationToken & = delete;
    OperationToken(OperationToken &&other) noexcept;
    auto operator=(OperationToken &&other) noexcept -> OperationToken &;

   private:
    friend class DatabaseInstance;
    explicit OperationToken(std::shared_ptr<DatabaseInstance> owner) : owner_(std::move(owner)) {}
    void Reset();
    std::shared_ptr<DatabaseInstance> owner_;
  };

  using SharedSchemaLease = SchemaLeaseManager::SharedLease;
  using ExclusiveSchemaLease = SchemaLeaseManager::ExclusiveLease;

  /** @brief Create an empty in-memory database with `config`. */
  explicit DatabaseInstance(DatabaseConfig config = {});
  /** @brief Open or create a durable database file with `config`. */
  DatabaseInstance(std::filesystem::path path, DatabaseConfig config = {});
  ~DatabaseInstance();

  DatabaseInstance(const DatabaseInstance &) = delete;
  auto operator=(const DatabaseInstance &) -> DatabaseInstance & = delete;
  DatabaseInstance(DatabaseInstance &&) = delete;
  auto operator=(DatabaseInstance &&) -> DatabaseInstance & = delete;

  /**
   * @brief Create an independent sequential session that retains an explicit database owner.
   * @param database The existing shared owner; must not be null.
   */
  static auto CreateConnection(const std::shared_ptr<DatabaseInstance> &database) -> std::shared_ptr<Connection>;

  /** @return The shared catalog. */
  auto GetCatalog() -> Catalog & { return *catalog_; }
  /** @return The shared transaction manager. */
  auto GetTransactionManager() -> TransactionManager & { return *txn_manager_; }
  /** @return The shared buffer pool. */
  auto GetBufferPool() -> BufferPoolManager & { return *buffer_pool_; }
  /** @return The immutable database configuration. */
  auto Config() const -> const DatabaseConfig & { return config_; }
  /** @return Whether this database has durable file backing. */
  auto IsDurable() const -> bool { return durable_ != nullptr; }
  /** @return The durable path, or an empty path for an in-memory database. */
  auto Path() const -> const std::filesystem::path & { return path_; }

  /**
   * @brief Admit one operation and retain its explicit database owner until the operation finishes.
   * @param database The existing shared owner; must not be null.
   */
  static auto AcquireOperation(const std::shared_ptr<DatabaseInstance> &database) -> OperationToken;
  /** @brief Keep catalog/storage objects alive against DDL reclamation. */
  auto AcquireSharedSchemaLease() -> SharedSchemaLease { return schema_leases_.AcquireShared(); }
  /** @brief Serialize DDL, clear/vacuum, persistence, and close against all query leases. */
  auto AcquireExclusiveSchemaLease() -> ExclusiveSchemaLease { return schema_leases_.AcquireExclusive(); }
  /** @brief Admit a query to the database-wide native worker budget. */
  auto AcquireWorkerSlot() -> WorkerSlotManager::Token { return worker_slots_.Acquire(); }
  /** @brief Non-blockingly borrow additional idle slots for one admitted query. */
  auto TryAcquireWorkerSlots(idx_t requested) -> WorkerSlotManager::Token {
    return worker_slots_.TryAcquire(requested);
  }
  auto GlobalMemoryManager() -> GlobalQueryMemoryManager & { return query_memory_; }

  auto WorkerCapacity() const -> idx_t { return worker_slots_.Capacity(); }
  auto ActiveWorkerSlots() const -> idx_t { return worker_slots_.Used(); }
  auto PeakWorkerSlots() const -> idx_t { return worker_slots_.Peak(); }
  auto QueryMemoryUsed() const -> idx_t { return query_memory_.Used(); }
  auto QueryMemoryPeak() const -> idx_t { return query_memory_.Peak(); }
  auto State() const -> LifecycleState;
  auto ActiveOperationCount() const -> size_t;

  /** @brief Roll back open work and flush durable state. Idempotent. */
  void Close();

 private:
  friend class OperationToken;
  friend class Connection;

  static constexpr size_t kInMemoryDiskPages = 4096;
  static auto ResolveWorkerCapacity(idx_t configured) -> idx_t;
  auto GarbageCollectTransactions() -> TransactionManager::GcStats;
  void ReleaseOperation();

  const DatabaseConfig config_;
  WorkerSlotManager worker_slots_;
  GlobalQueryMemoryManager query_memory_;
  std::filesystem::path path_;
  std::unique_ptr<DurablePathLock> durable_lock_;
  std::unique_ptr<Database> durable_;
  std::unique_ptr<MemoryDiskManager> memory_disk_;
  std::unique_ptr<BufferPoolManager> owned_buffer_pool_;
  std::unique_ptr<Catalog> owned_catalog_;
  std::unique_ptr<TransactionManager> owned_transaction_manager_;
  Catalog *catalog_{nullptr};
  BufferPoolManager *buffer_pool_{nullptr};
  TransactionManager *txn_manager_{nullptr};

  mutable std::mutex lifecycle_mutex_;
  std::condition_variable lifecycle_cv_;
  LifecycleState state_{LifecycleState::OPEN};
  size_t active_operations_{0};
  std::vector<std::weak_ptr<Connection>> connections_;
  SchemaLeaseManager schema_leases_;
};

}  // namespace bumblebee
