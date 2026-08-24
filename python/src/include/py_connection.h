//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// py_connection.h
//
// Identification: python/src/include/py_connection.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <pybind11/pybind11.h>

#include <memory>
#include <string>
#include <vector>

#include "main/connection.h"
#include "py_result.h"

namespace bumblebee::python {

class PyTransactionContext;

class PyConnection {
 public:
  explicit PyConnection(std::shared_ptr<Connection> connection);
  ~PyConnection();

  auto Sql(const std::string &sql) -> PyResult;
  auto ExecuteScript(const std::string &sql) -> std::vector<PyResult>;
  auto RunFile(const pybind11::object &path) -> std::vector<PyResult>;
  void Begin(const std::string &isolation);
  void Commit();
  void Rollback();
  static auto Transaction(const std::shared_ptr<PyConnection> &connection, const std::string &isolation)
      -> std::shared_ptr<PyTransactionContext>;
  auto GetTables() -> pybind11::list;
  auto DescribeTable(const std::string &name) -> pybind11::dict;
  auto GetTable(const std::string &name) -> PyResult;
  auto RemoveTable(const std::string &name, bool if_exists) -> bool;
  auto Vacuum(const std::string &name) -> size_t;
  auto CollectGarbage() -> pybind11::dict;
  auto Explain(const std::string &query, const std::string &mode) -> std::string;
  void Close();
  auto IsClosed() const -> bool;
  auto HasActiveTransaction() const -> bool;

 private:
  std::shared_ptr<Connection> connection_;
};

class PyTransactionContext {
 public:
  PyTransactionContext(std::shared_ptr<PyConnection> connection, IsolationLevel isolation);
  ~PyTransactionContext();

  auto Enter() -> std::shared_ptr<PyConnection>;
  auto Exit(bool has_python_exception) -> bool;

 private:
  std::shared_ptr<PyConnection> connection_;
  IsolationLevel isolation_;
  bool entered_{false};
  bool finished_{false};
};

}  // namespace bumblebee::python
