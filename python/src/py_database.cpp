//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// py_database.cpp
//
// Identification: python/src/py_database.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "include/py_database.h"

#include <pybind11/stl.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/dataframe_conversion.h"
#include "include/python_utils.h"

namespace py = pybind11;

namespace bumblebee::python {

PyDatabase::PyDatabase(std::shared_ptr<DatabaseInstance> database) : database_(std::move(database)) {}

PyDatabase::~PyDatabase() {
  if (database_ == nullptr) {
    return;
  }
  try {
    if (Py_IsInitialized() != 0 && PyGILState_Check() != 0) {
      py::gil_scoped_release release;
      database_->Close();
    } else {
      database_->Close();
    }
  } catch (...) {
  }
}

auto PyDatabase::Sql(const std::string &sql) -> PyResult {
  auto connection = DatabaseInstance::CreateConnection(database_);
  return PyResult(connection->ExecuteSqlStatement(sql, false));
}

auto PyDatabase::Connect() -> std::shared_ptr<PyConnection> {
  return std::make_shared<PyConnection>(DatabaseInstance::CreateConnection(database_));
}

auto PyDatabase::ExecuteScript(const std::string &sql) -> std::vector<PyResult> {
  std::shared_ptr<PyConnection> connection;
  {
    py::gil_scoped_release release;
    connection = std::make_shared<PyConnection>(DatabaseInstance::CreateConnection(database_));
  }
  return connection->ExecuteScript(sql);
}

auto PyDatabase::RunFile(const py::object &path) -> std::vector<PyResult> {
  std::shared_ptr<PyConnection> connection;
  {
    py::gil_scoped_release release;
    connection = std::make_shared<PyConnection>(DatabaseInstance::CreateConnection(database_));
  }
  return connection->RunFile(path);
}

auto PyDatabase::GetTables() -> py::list {
  std::shared_ptr<PyConnection> connection;
  {
    py::gil_scoped_release release;
    connection = std::make_shared<PyConnection>(DatabaseInstance::CreateConnection(database_));
  }
  return connection->GetTables();
}

auto PyDatabase::DescribeTable(const std::string &name) -> py::dict {
  std::shared_ptr<PyConnection> connection;
  {
    py::gil_scoped_release release;
    connection = std::make_shared<PyConnection>(DatabaseInstance::CreateConnection(database_));
  }
  return connection->DescribeTable(name);
}

auto PyDatabase::GetTable(const std::string &name) -> PyResult {
  const auto sql = "SELECT * FROM " + QuoteIdentifier(name);
  py::gil_scoped_release release;
  auto connection = DatabaseInstance::CreateConnection(database_);
  return PyResult(connection->ExecuteSqlStatement(sql, false));
}

auto PyDatabase::RemoveTable(const std::string &name, bool if_exists) -> bool {
  const auto sql = std::string("DROP TABLE ") + (if_exists ? "IF EXISTS " : "") + QuoteIdentifier(name);
  py::gil_scoped_release release;
  auto connection = DatabaseInstance::CreateConnection(database_);
  const auto result = connection->ExecuteSqlStatement(sql, false);
  return result.AffectedRows().value_or(0) != 0;
}

auto PyDatabase::Vacuum(const std::string &name) -> size_t {
  py::gil_scoped_release release;
  auto connection = DatabaseInstance::CreateConnection(database_);
  return connection->VacuumTable(name);
}

auto PyDatabase::CollectGarbage() -> py::dict {
  std::shared_ptr<PyConnection> connection;
  {
    py::gil_scoped_release release;
    connection = std::make_shared<PyConnection>(DatabaseInstance::CreateConnection(database_));
  }
  return connection->CollectGarbage();
}

auto PyDatabase::ResourceStats() const -> py::dict {
  idx_t worker_capacity;
  idx_t active_worker_slots;
  idx_t peak_worker_slots;
  idx_t query_memory_used;
  idx_t query_memory_peak;
  size_t active_operations;
  {
    py::gil_scoped_release release;
    worker_capacity = database_->WorkerCapacity();
    active_worker_slots = database_->ActiveWorkerSlots();
    peak_worker_slots = database_->PeakWorkerSlots();
    query_memory_used = database_->QueryMemoryUsed();
    query_memory_peak = database_->QueryMemoryPeak();
    active_operations = database_->ActiveOperationCount();
  }
  py::dict result;
  result["worker_capacity"] = worker_capacity;
  result["active_worker_slots"] = active_worker_slots;
  result["peak_worker_slots"] = peak_worker_slots;
  result["query_memory_used"] = query_memory_used;
  result["query_memory_peak"] = query_memory_peak;
  result["active_operations"] = active_operations;
  return result;
}

auto PyDatabase::Explain(const std::string &query, const std::string &mode) -> std::string {
  static const std::vector<std::string> modes = {"binder", "planner", "optimizer", "physical", "pipelines", "analyze"};
  if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
    throw py::value_error("mode must be binder, planner, optimizer, physical, pipelines, or analyze");
  }
  const auto sql = "EXPLAIN (" + mode + ") " + query;
  py::gil_scoped_release release;
  auto connection = DatabaseInstance::CreateConnection(database_);
  return connection->ExecuteSqlStatement(sql, false).Status();
}

void PyDatabase::LoadDataFrame(const py::object &frame, const std::string &name, const py::object &primary_key,
                               const std::string &if_exists, bool include_index) {
  if (if_exists != "error" && if_exists != "append") {
    throw py::value_error("if_exists must be 'error' or 'append'");
  }
  auto keys = ParsePrimaryKey(primary_key);
  auto converted = ConvertDataFrame(frame, include_index);

  {
    py::gil_scoped_release release;
    auto connection = DatabaseInstance::CreateConnection(database_);
    connection->LoadDataChunks(name, std::move(converted.names_), std::move(converted.types_), std::move(keys),
                               if_exists == "append", std::move(converted.chunks_));
  }
}

void PyDatabase::Close() { database_->Close(); }

auto PyDatabase::IsClosed() const -> bool { return database_->State() == DatabaseInstance::LifecycleState::CLOSED; }

auto MakeDatabase(const py::object &path, const py::object &worker_threads, const py::object &max_memory,
                  const py::object &frames, const py::object &morsel_pages, const py::object &transaction_timeout,
                  const py::object &prefer_external) -> std::shared_ptr<PyDatabase> {
  auto native_path = ParsePath(path);
  auto config = MakeConfig(worker_threads, max_memory, frames, morsel_pages, transaction_timeout, prefer_external);

  std::shared_ptr<DatabaseInstance> database;
  {
    py::gil_scoped_release release;
    if (native_path.has_value()) {
      database = std::make_shared<DatabaseInstance>(std::move(*native_path), std::move(config));
    } else {
      database = std::make_shared<DatabaseInstance>(std::move(config));
    }
  }
  return std::make_shared<PyDatabase>(std::move(database));
}

}  // namespace bumblebee::python
