//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// py_database.h
//
// Identification: python/src/include/py_database.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <pybind11/pybind11.h>

#include <memory>
#include <string>
#include <vector>

#include "main/database_instance.h"
#include "py_connection.h"
#include "py_result.h"

namespace bumblebee::python {

class PyDatabase {
 public:
  explicit PyDatabase(std::shared_ptr<DatabaseInstance> database);
  ~PyDatabase();

  auto Sql(const std::string &sql) -> PyResult;
  auto Connect() -> std::shared_ptr<PyConnection>;
  auto ExecuteScript(const std::string &sql) -> std::vector<PyResult>;
  auto RunFile(const pybind11::object &path) -> std::vector<PyResult>;
  auto GetTables() -> pybind11::list;
  auto DescribeTable(const std::string &name) -> pybind11::dict;
  auto GetTable(const std::string &name) -> PyResult;
  auto RemoveTable(const std::string &name, bool if_exists) -> bool;
  auto Vacuum(const std::string &name) -> size_t;
  auto CollectGarbage() -> pybind11::dict;
  auto ResourceStats() const -> pybind11::dict;
  auto Explain(const std::string &query, const std::string &mode) -> std::string;
  void LoadDataFrame(const pybind11::object &frame, const std::string &name, const pybind11::object &primary_key,
                     const std::string &if_exists, bool include_index);
  void Close();
  auto IsClosed() const -> bool;

 private:
  std::shared_ptr<DatabaseInstance> database_;
};

auto MakeDatabase(const pybind11::object &path, const pybind11::object &worker_threads,
                  const pybind11::object &max_memory, const pybind11::object &frames,
                  const pybind11::object &morsel_pages, const pybind11::object &transaction_timeout,
                  const pybind11::object &prefer_external) -> std::shared_ptr<PyDatabase>;

}  // namespace bumblebee::python
