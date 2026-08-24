//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// database_instance.cpp
//
// Identification: src/main/database_instance.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "main/database_instance.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <thread>  // NOLINT
#include <unordered_set>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "common/exception.h"
#include "fmt/format.h"
#include "main/connection.h"

namespace bumblebee {

namespace {

auto CanonicalDurablePath(const std::filesystem::path &path) -> std::filesystem::path {
  std::error_code ec;
  auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(path, ec), ec);
  if (ec) {
    ec.clear();
    canonical = std::filesystem::absolute(path, ec).lexically_normal();
  }
  return canonical;
}

auto OpenPathsMutex() -> std::mutex & {
  static std::mutex mutex;
  return mutex;
}

auto OpenPaths() -> std::unordered_set<std::string> & {
  static std::unordered_set<std::string> paths;
  return paths;
}

}  // namespace

/**
 * @brief Process-local path registration plus a non-blocking advisory OS lock on the database file.
 *
 * The process-local registry gives deterministic alias handling and diagnostics. On POSIX, flock also
 * rejects an independent BumbleBeeDB process. The lock is intentionally held until clean close has
 * finished; this does not claim crash recovery, it only prevents unsupported simultaneous writers.
 */
class DurablePathLock {
 public:
  explicit DurablePathLock(const std::filesystem::path &path) : canonical_(CanonicalDurablePath(path)) {
    const auto key = canonical_.string();
    {
      std::lock_guard lock(OpenPathsMutex());
      if (!OpenPaths().insert(key).second) {
        throw ExecutionException(fmt::format("database file '{}' is already open in this process", key));
      }
      registered_ = true;
    }

#ifndef _WIN32
    fd_ = ::open(canonical_.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ < 0) {
      const auto message = std::strerror(errno);
      ReleaseRegistration();
      throw ExecutionException(fmt::format("cannot open database file '{}': {}", key, message));
    }
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      const auto message = std::strerror(errno);
      ::close(fd_);
      fd_ = -1;
      ReleaseRegistration();
      throw ExecutionException(fmt::format("database file '{}' is locked by another process: {}", key, message));
    }
#endif
  }

  ~DurablePathLock() {
#ifndef _WIN32
    if (fd_ >= 0) {
      static_cast<void>(::flock(fd_, LOCK_UN));
      static_cast<void>(::close(fd_));
    }
#endif
    ReleaseRegistration();
  }

  DurablePathLock(const DurablePathLock &) = delete;
  auto operator=(const DurablePathLock &) -> DurablePathLock & = delete;

 private:
  void ReleaseRegistration() {
    if (!registered_) {
      return;
    }
    std::lock_guard lock(OpenPathsMutex());
    OpenPaths().erase(canonical_.string());
    registered_ = false;
  }

  std::filesystem::path canonical_;
  bool registered_{false};
#ifndef _WIN32
  int fd_{-1};
#endif
};

auto DatabaseInstance::ResolveWorkerCapacity(idx_t configured) -> idx_t {
  if (configured > 0) {
    return std::clamp<idx_t>(configured, 1, MAX_THREADS);
  }
  const auto hardware = std::thread::hardware_concurrency();
  const auto detected = hardware == 0 ? DEFAULT_THREAD_COUNT : static_cast<idx_t>(hardware);
  return std::clamp<idx_t>(detected, 1, MAX_THREADS);
}

DatabaseInstance::DatabaseInstance(DatabaseConfig config)
    : config_(std::move(config)),
      worker_slots_(ResolveWorkerCapacity(config_.worker_threads_)),
      query_memory_(config_.max_memory_),
      memory_disk_(std::make_unique<MemoryDiskManager>(kInMemoryDiskPages)),
      owned_buffer_pool_(std::make_unique<BufferPoolManager>(config_.frames_, memory_disk_.get())),
      owned_catalog_(std::make_unique<Catalog>(owned_buffer_pool_.get())),
      owned_transaction_manager_(
          std::make_unique<TransactionManager>(owned_catalog_.get(), config_.transaction_timeout_)) {
  catalog_ = owned_catalog_.get();
  buffer_pool_ = owned_buffer_pool_.get();
  txn_manager_ = owned_transaction_manager_.get();
}

DatabaseInstance::DatabaseInstance(std::filesystem::path path, DatabaseConfig config)
    : config_(std::move(config)),
      worker_slots_(ResolveWorkerCapacity(config_.worker_threads_)),
      query_memory_(config_.max_memory_),
      path_(CanonicalDurablePath(path)) {
  durable_lock_ = std::make_unique<DurablePathLock>(path_);
  try {
    durable_ = std::make_unique<Database>(path_, config_.frames_, config_.transaction_timeout_);
  } catch (...) {
    durable_lock_.reset();
    throw;
  }
  catalog_ = &durable_->GetCatalog();
  buffer_pool_ = &durable_->GetBufferPool();
  txn_manager_ = &durable_->GetTransactionManager();
}

DatabaseInstance::~DatabaseInstance() {
  try {
    Close();
  } catch (...) {
    // Destruction is the last-resort cleanup path and cannot report a close failure. Explicit Close()
    // retains the exception. Native facilities still destruct immediately after this body.
  }
}

DatabaseInstance::OperationToken::~OperationToken() { Reset(); }

DatabaseInstance::OperationToken::OperationToken(OperationToken &&other) noexcept : owner_(std::move(other.owner_)) {}

auto DatabaseInstance::OperationToken::operator=(OperationToken &&other) noexcept -> OperationToken & {
  if (this != &other) {
    Reset();
    owner_ = std::move(other.owner_);
  }
  return *this;
}

void DatabaseInstance::OperationToken::Reset() {
  if (owner_ != nullptr) {
    auto owner = std::move(owner_);
    owner->ReleaseOperation();
  }
}

auto DatabaseInstance::AcquireOperation(const std::shared_ptr<DatabaseInstance> &database) -> OperationToken {
  if (database == nullptr) {
    throw DatabaseClosedException("cannot acquire an operation without a database owner");
  }
  std::lock_guard lock(database->lifecycle_mutex_);
  if (database->state_ != LifecycleState::OPEN) {
    throw DatabaseClosedException("database is closing or closed");
  }
  database->active_operations_++;
  return OperationToken(database);
}

void DatabaseInstance::ReleaseOperation() {
  std::lock_guard lock(lifecycle_mutex_);
  if (--active_operations_ == 0) {
    lifecycle_cv_.notify_all();
  }
}

auto DatabaseInstance::CreateConnection(const std::shared_ptr<DatabaseInstance> &database)
    -> std::shared_ptr<Connection> {
  if (database == nullptr) {
    throw DatabaseClosedException("cannot connect without a database owner");
  }
  std::lock_guard lock(database->lifecycle_mutex_);
  if (database->state_ != LifecycleState::OPEN) {
    throw DatabaseClosedException("cannot connect: database is closing or closed");
  }
  auto connection = std::shared_ptr<Connection>(new Connection(database));
  database->connections_.push_back(connection);
  return connection;
}

auto DatabaseInstance::State() const -> LifecycleState {
  std::lock_guard lock(lifecycle_mutex_);
  return state_;
}

auto DatabaseInstance::ActiveOperationCount() const -> size_t {
  std::lock_guard lock(lifecycle_mutex_);
  return active_operations_;
}

auto DatabaseInstance::GarbageCollectTransactions() -> TransactionManager::GcStats {
  const auto stats = txn_manager_->GarbageCollection();

  std::vector<std::shared_ptr<Connection>> connections;
  {
    std::lock_guard lock(lifecycle_mutex_);
    connections.reserve(connections_.size());
    for (auto &weak : connections_) {
      if (auto connection = weak.lock()) {
        connections.push_back(std::move(connection));
      }
    }
  }
  for (auto &connection : connections) {
    connection->ReconcileTransactionAfterGc();
  }
  return stats;
}

void DatabaseInstance::Close() {
  std::vector<std::shared_ptr<Connection>> connections;
  {
    std::unique_lock lock(lifecycle_mutex_);
    if (state_ == LifecycleState::CLOSED) {
      return;
    }
    if (state_ == LifecycleState::CLOSING) {
      lifecycle_cv_.wait(lock, [&] { return state_ == LifecycleState::CLOSED; });
      return;
    }
    state_ = LifecycleState::CLOSING;
    connections.reserve(connections_.size());
    for (auto &weak : connections_) {
      if (auto connection = weak.lock()) {
        connections.push_back(std::move(connection));
      }
    }
    connections_.clear();
  }

  // First reject new work on every session, then close every idle session. A busy connection finishes
  // its close from StatementGuard before releasing its operation token. This ordering releases an idle
  // explicit transaction's shared schema lease while an already-admitted DDL operation is waiting for
  // exclusive ownership, so shutdown never waits on the far side of that lease dependency.
  for (auto &connection : connections) {
    connection->RequestCloseFromDatabase();
  }
  for (auto &connection : connections) {
    connection->CloseIdleFromDatabase();
  }

  {
    std::unique_lock lock(lifecycle_mutex_);
    lifecycle_cv_.wait(lock, [&] { return active_operations_ == 0; });
  }

  std::exception_ptr close_error;
  for (auto &connection : connections) {
    try {
      connection->CloseFromDatabase();
    } catch (...) {
      if (close_error == nullptr) {
        close_error = std::current_exception();
      }
    }
  }

  try {
    auto schema_lease = AcquireExclusiveSchemaLease();
    txn_manager_->AbortAllRunning();
    if (durable_ != nullptr) {
      durable_->Close();
    }
  } catch (...) {
    if (close_error == nullptr) {
      close_error = std::current_exception();
    }
  }

  // Release duplicate-open protection only after persistence/flush has returned (successfully or not).
  durable_lock_.reset();
  {
    std::lock_guard lock(lifecycle_mutex_);
    state_ = LifecycleState::CLOSED;
    lifecycle_cv_.notify_all();
  }
  if (close_error != nullptr) {
    std::rethrow_exception(close_error);
  }
}

}  // namespace bumblebee
