//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// py_connection.cpp
//
// Identification: python/src/py_connection.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "include/py_connection.h"

#include <pybind11/stl.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/exception.h"
#include "include/python_conversion.h"
#include "include/python_utils.h"

namespace py = pybind11;

namespace bumblebee::python {

PyConnection::PyConnection(std::shared_ptr<Connection> connection) : connection_(std::move(connection)) {}

PyConnection::~PyConnection() {
  if (connection_ == nullptr) {
    return;
  }
  try {
    if (Py_IsInitialized() != 0 && PyGILState_Check() != 0) {
      py::gil_scoped_release release;
      connection_->Close();
    } else {
      connection_->Close();
    }
  } catch (...) {
  }
}

auto PyConnection::Sql(const std::string &sql) -> PyResult { return PyResult(connection_->ExecuteSqlStatement(sql)); }

auto PyConnection::ExecuteScript(const std::string &sql) -> std::vector<PyResult> {
  std::vector<PyResult> output;
  {
    py::gil_scoped_release release;
    auto results = connection_->ExecuteSqlScript(sql);
    output.reserve(results.size());
    for (auto &result : results) {
      output.emplace_back(std::move(result));
    }
  }
  return output;
}

auto PyConnection::RunFile(const py::object &path) -> std::vector<PyResult> {
  const auto native_path = ParseRequiredPath(path);
  std::string sql;
  {
    py::gil_scoped_release release;
    sql = ReadFileBytes(native_path);
  }
  sql = ValidateUtf8(std::move(sql));
  return ExecuteScript(sql);
}

void PyConnection::Begin(const std::string &isolation) { connection_->BeginTransaction(ParseIsolation(isolation)); }

void PyConnection::Commit() { connection_->CommitTransaction(); }

void PyConnection::Rollback() { connection_->RollbackTransaction(); }

auto PyConnection::Transaction(const std::shared_ptr<PyConnection> &connection, const std::string &isolation)
    -> std::shared_ptr<PyTransactionContext> {
  if (connection == nullptr) {
    throw ProgrammingException("transaction context requires a connection owner");
  }
  return std::make_shared<PyTransactionContext>(connection, ParseIsolation(isolation));
}

auto PyConnection::GetTables() -> py::list {
  std::vector<TableMetadata> metadata;
  {
    py::gil_scoped_release release;
    metadata = connection_->ListTables();
  }
  return TableMetadataListToPython(metadata);
}

auto PyConnection::DescribeTable(const std::string &name) -> py::dict {
  TableMetadata metadata;
  {
    py::gil_scoped_release release;
    metadata = connection_->DescribeTable(name);
  }
  return TableMetadataToPython(metadata);
}

auto PyConnection::GetTable(const std::string &name) -> PyResult {
  const auto sql = "SELECT * FROM " + QuoteIdentifier(name);
  py::gil_scoped_release release;
  return PyResult(connection_->ExecuteSqlStatement(sql));
}

auto PyConnection::RemoveTable(const std::string &name, bool if_exists) -> bool {
  const auto sql = std::string("DROP TABLE ") + (if_exists ? "IF EXISTS " : "") + QuoteIdentifier(name);
  py::gil_scoped_release release;
  const auto result = connection_->ExecuteSqlStatement(sql);
  return result.AffectedRows().value_or(0) != 0;
}

auto PyConnection::Vacuum(const std::string &name) -> size_t {
  py::gil_scoped_release release;
  return connection_->VacuumTable(name);
}

auto PyConnection::CollectGarbage() -> py::dict {
  TransactionManager::GcStats stats;
  {
    py::gil_scoped_release release;
    stats = connection_->GarbageCollect();
  }
  py::dict result;
  result["timed_out"] = stats.timed_out_;
  result["reclaimed"] = stats.reclaimed_;
  return result;
}

auto PyConnection::Explain(const std::string &query, const std::string &mode) -> std::string {
  static const std::vector<std::string> modes = {"binder", "planner", "optimizer", "physical", "pipelines", "analyze"};
  if (std::find(modes.begin(), modes.end(), mode) == modes.end()) {
    throw py::value_error("mode must be binder, planner, optimizer, physical, pipelines, or analyze");
  }
  const auto sql = "EXPLAIN (" + mode + ") " + query;
  py::gil_scoped_release release;
  return connection_->ExecuteSqlStatement(sql).Status();
}

void PyConnection::Close() { connection_->Close(); }

auto PyConnection::IsClosed() const -> bool { return connection_->IsClosed(); }

auto PyConnection::HasActiveTransaction() const -> bool { return connection_->HasActiveTransaction(); }

PyTransactionContext::PyTransactionContext(std::shared_ptr<PyConnection> connection, IsolationLevel isolation)
    : connection_(std::move(connection)), isolation_(isolation) {}

PyTransactionContext::~PyTransactionContext() {
  if (!entered_ || finished_ || connection_ == nullptr || !connection_->HasActiveTransaction()) {
    return;
  }
  try {
    if (Py_IsInitialized() != 0 && PyGILState_Check() != 0) {
      py::gil_scoped_release release;
      connection_->Rollback();
    } else {
      connection_->Rollback();
    }
  } catch (...) {
  }
}

auto PyTransactionContext::Enter() -> std::shared_ptr<PyConnection> {
  if (entered_) {
    throw ProgrammingException("transaction context cannot be entered more than once");
  }
  {
    py::gil_scoped_release release;
    connection_->Begin(isolation_ == IsolationLevel::SERIALIZABLE ? "serializable" : "snapshot");
  }
  entered_ = true;
  return connection_;
}

auto PyTransactionContext::Exit(bool has_python_exception) -> bool {
  if (!entered_) {
    throw ProgrammingException("transaction context has not been entered");
  }
  if (finished_) {
    throw ProgrammingException("transaction context has already finished");
  }
  try {
    if (connection_->HasActiveTransaction()) {
      py::gil_scoped_release release;
      if (has_python_exception) {
        connection_->Rollback();
      } else {
        connection_->Commit();
      }
    }
    finished_ = true;
  } catch (...) {
    finished_ = true;
    throw;
  }
  return false;
}

}  // namespace bumblebee::python
