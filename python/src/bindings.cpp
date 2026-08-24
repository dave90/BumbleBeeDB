//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// bindings.cpp
//
// Identification: python/src/bindings.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

// Keep this entry point declarative. Wrapper behavior belongs in the focused implementation units
// beside it, following the established BumbleBee Python binding layout.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <memory>
#include <string>
#include <vector>

#include "include/py_connection.h"
#include "include/py_database.h"
#include "include/py_result.h"
#include "include/python_exception.h"

namespace py = pybind11;

namespace bumblebee::python {

void RegisterBindings(py::module_ &module) {
  py::class_<PyResult>(module, "Result", "An immutable, owning query result with an independent cursor position.")
      .def_property_readonly("columns", &PyResult::Columns, "Output column names in result order.")
      .def_property_readonly("types", &PyResult::Types, "SQL logical type names in result order.")
      .def_property_readonly("row_count", &PyResult::RowCount, "The cached number of result rows.")
      .def_property_readonly("is_command", &PyResult::IsCommand,
                             "Whether this is command metadata rather than a row result.")
      .def_property_readonly("command_tag", &PyResult::CommandTag, "The command tag, or an empty string.")
      .def_property_readonly("status", &PyResult::Status, "Human-readable command status or plan text.")
      .def_property_readonly("affected_rows", &PyResult::AffectedRows,
                             "Affected row count when the command reports one.")
      .def("tuples", &PyResult::Tuples, "Return all rows as a new list of Python tuples.")
      .def("to_df", &PyResult::ToDataFrame, "Return all rows as a new independent pandas DataFrame.")
      .def("fetchone", &PyResult::FetchOne, "Return the next cursor row, or None at end.")
      .def("fetchmany", &PyResult::FetchMany, py::arg("size") = 1, "Return up to size rows from the mutable cursor.")
      .def("fetchall", &PyResult::FetchAll, "Return every row remaining at the mutable cursor.")
      .def("__len__", &PyResult::RowCount, "Return row_count in constant time.")
      .def(
          "__iter__", [](PyResult &self) -> PyResult & { return self; }, py::return_value_policy::reference_internal,
          "Iterate from the mutable cursor.")
      .def("__next__", &PyResult::Next, "Return the next cursor row.");

  auto connection = py::class_<PyConnection, std::shared_ptr<PyConnection>>(
      module, "Connection", "A sequential database session that owns at most one explicit transaction.");
  connection
      .def("sql", &PyConnection::Sql, py::arg("query"), py::call_guard<py::gil_scoped_release>(),
           "Execute exactly one SQL statement on this session.")
      .def("execute_script", &PyConnection::ExecuteScript, py::arg("script"),
           "Execute SQL statements in order and return one Result per statement.")
      .def("run_file", &PyConnection::RunFile, py::arg("path"), "Read a UTF-8 SQL file and execute it as one script.")
      .def("begin", &PyConnection::Begin, py::arg("isolation") = "snapshot", py::call_guard<py::gil_scoped_release>(),
           "Begin a snapshot or serializable transaction.")
      .def("commit", &PyConnection::Commit, py::call_guard<py::gil_scoped_release>(), "Commit the active transaction.")
      .def("rollback", &PyConnection::Rollback, py::call_guard<py::gil_scoped_release>(),
           "Roll back the active transaction.")
      .def("get_tables", &PyConnection::GetTables, "Return structured metadata for every table.")
      .def("describe_table", &PyConnection::DescribeTable, py::arg("name"), "Return structured metadata for one table.")
      .def("get_table", &PyConnection::GetTable, py::arg("name"), "Safely execute SELECT * for a table name.")
      .def("remove_table", &PyConnection::RemoveTable, py::arg("name"), py::kw_only(), py::arg("if_exists") = false,
           "Drop a table and return whether it existed.")
      .def("vacuum", &PyConnection::Vacuum, py::arg("name"),
           "Vacuum an external parquet table and return the number of files removed.")
      .def("collect_garbage", &PyConnection::CollectGarbage,
           "Run transaction timeout/reclamation GC and return its counters.")
      .def("explain", &PyConnection::Explain, py::arg("query"), py::kw_only(), py::arg("mode") = "physical",
           "Return binder, planner, optimizer, physical, pipeline, or analyzed plan text.")
      .def("close", &PyConnection::Close, py::call_guard<py::gil_scoped_release>(),
           "Roll back open work and close this session; idempotent.")
      .def_property_readonly("closed", &PyConnection::IsClosed, "Whether this session is closed.")
      .def_property_readonly("in_transaction", &PyConnection::HasActiveTransaction,
                             "Whether this session owns an explicit transaction.")
      .def(
          "__enter__", [](PyConnection &self) -> PyConnection & { return self; },
          py::return_value_policy::reference_internal, "Enter this connection context.")
      .def(
          "__exit__",
          [](PyConnection &self, const py::object &, const py::object &, const py::object &) {
            py::gil_scoped_release release;
            self.Close();
            return false;
          },
          "Close this connection and never suppress the active Python exception.");

  py::class_<PyTransactionContext, std::shared_ptr<PyTransactionContext>>(
      module, "_TransactionContext", "Context manager returned by Connection.transaction().")
      .def("__enter__", &PyTransactionContext::Enter, "Begin the transaction and return its Connection.")
      .def(
          "__exit__",
          [](PyTransactionContext &self, const py::object &exception_type, const py::object &, const py::object &) {
            return self.Exit(!exception_type.is_none());
          },
          "Commit on success or roll back on a Python exception.");
  connection.def(
      "transaction",
      [](const std::shared_ptr<PyConnection> &self, const std::string &isolation) {
        return PyConnection::Transaction(self, isolation);
      },
      py::arg("isolation") = "snapshot", "Return a context manager for a snapshot or serializable transaction.");

  auto database = py::class_<PyDatabase, std::shared_ptr<PyDatabase>>(
      module, "Database", "A thread-safe database owner whose sql() calls use independent sessions.");
  database
      .def(py::init(&MakeDatabase), py::arg("path") = py::none(), py::kw_only(), py::arg("worker_threads") = 0,
           py::arg("max_memory") = py::none(), py::arg("frames") = py::none(), py::arg("morsel_pages") = py::none(),
           py::arg("transaction_timeout") = py::none(), py::arg("prefer_external") = false,
           "Open an empty in-memory database or a durable database at path.")
      .def("sql", &PyDatabase::Sql, py::arg("query"), py::call_guard<py::gil_scoped_release>(),
           "Execute exactly one autocommit SQL statement on an independent session.")
      .def("connect", &PyDatabase::Connect, py::call_guard<py::gil_scoped_release>(),
           "Create an independent sequential Connection for explicit transactions.")
      .def("execute_script", &PyDatabase::ExecuteScript, py::arg("script"),
           "Execute a script on one temporary Connection and return its Results.")
      .def("run_file", &PyDatabase::RunFile, py::arg("path"),
           "Read a UTF-8 SQL file and execute it on one temporary Connection.")
      .def("get_tables", &PyDatabase::GetTables, "Return structured metadata for every table.")
      .def("describe_table", &PyDatabase::DescribeTable, py::arg("name"), "Return structured metadata for one table.")
      .def("get_table", &PyDatabase::GetTable, py::arg("name"), "Safely execute SELECT * for a table name.")
      .def("remove_table", &PyDatabase::RemoveTable, py::arg("name"), py::kw_only(), py::arg("if_exists") = false,
           "Drop a table and return whether it existed.")
      .def("vacuum", &PyDatabase::Vacuum, py::arg("name"),
           "Vacuum an external parquet table and return the number of files removed.")
      .def("collect_garbage", &PyDatabase::CollectGarbage,
           "Run transaction timeout/reclamation GC and return its counters.")
      .def("resource_stats", &PyDatabase::ResourceStats,
           "Return database-wide worker, query-memory, and active-operation counters.")
      .def("explain", &PyDatabase::Explain, py::arg("query"), py::kw_only(), py::arg("mode") = "physical",
           "Return binder, planner, optimizer, physical, pipeline, or analyzed plan text.")
      .def("load_df", &PyDatabase::LoadDataFrame, py::arg("frame"), py::arg("name"), py::kw_only(),
           py::arg("primary_key") = py::none(), py::arg("if_exists") = "error", py::arg("include_index") = false,
           "Copy a pandas DataFrame into a real row table transactionally.")
      .def("close", &PyDatabase::Close, py::call_guard<py::gil_scoped_release>(),
           "Wait for admitted work, flush durable state, and close; idempotent.")
      .def_property_readonly("closed", &PyDatabase::IsClosed, "Whether database shutdown completed.")
      .def(
          "__enter__", [](PyDatabase &self) -> PyDatabase & { return self; },
          py::return_value_policy::reference_internal, "Enter this database context.")
      .def(
          "__exit__",
          [](PyDatabase &self, const py::object &, const py::object &, const py::object &) {
            py::gil_scoped_release release;
            self.Close();
            return false;
          },
          "Close this database and never suppress the active Python exception.");

  module.def("db", &MakeDatabase, py::arg("path") = py::none(), py::kw_only(), py::arg("worker_threads") = 0,
             py::arg("max_memory") = py::none(), py::arg("frames") = py::none(), py::arg("morsel_pages") = py::none(),
             py::arg("transaction_timeout") = py::none(), py::arg("prefer_external") = false,
             "Open an empty in-memory database or a durable database at path.");
}

}  // namespace bumblebee::python

PYBIND11_MODULE(_native, module) {
  module.doc() = "Private native bindings for BumbleBeeDB";
  module.attr("__version__") = BUMBLEBEEDB_VERSION;
  bumblebee::python::RegisterExceptions(module);
  bumblebee::python::RegisterBindings(module);
}
