//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// python_exception.cpp
//
// Identification: python/src/python_exception.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "include/python_exception.h"

#include <filesystem>
#include <memory>
#include <string>

#include "common/exception.h"

namespace py = pybind11;

namespace bumblebee::python {

struct PythonExceptionTypes {
  PyObject *bumblebee{nullptr};
  PyObject *database{nullptr};
  PyObject *database_closed{nullptr};
  PyObject *parser{nullptr};
  PyObject *binder{nullptr};
  PyObject *planner{nullptr};
  PyObject *execution{nullptr};
  PyObject *conflict{nullptr};
  PyObject *not_implemented{nullptr};
  PyObject *programming{nullptr};
  PyObject *data{nullptr};
};

static PythonExceptionTypes python_exceptions;

static auto AddException(py::module_ &module, const char *name, PyObject *base) -> PyObject * {
  const auto qualified = std::string("bumblebeedb._native.") + name;
  auto *type = PyErr_NewException(qualified.c_str(), base, nullptr);
  if (type == nullptr) {
    throw py::error_already_set();
  }
  module.attr(name) = py::reinterpret_borrow<py::object>(type);
  // Keep the creating reference for the process lifetime. Exception translators can run late during
  // interpreter teardown, after ordinary module attributes have begun to clear.
  return type;
}

void RegisterExceptions(py::module_ &module) {
  python_exceptions.bumblebee = AddException(module, "BumbleBeeError", PyExc_Exception);
  python_exceptions.database = AddException(module, "DatabaseError", python_exceptions.bumblebee);
  python_exceptions.database_closed = AddException(module, "DatabaseClosedError", python_exceptions.database);
  python_exceptions.parser = AddException(module, "ParserError", python_exceptions.database);
  python_exceptions.binder = AddException(module, "BinderError", python_exceptions.database);
  python_exceptions.planner = AddException(module, "PlannerError", python_exceptions.database);
  python_exceptions.execution = AddException(module, "ExecutionError", python_exceptions.database);
  python_exceptions.conflict = AddException(module, "ConflictError", python_exceptions.execution);
  python_exceptions.not_implemented = AddException(module, "NotImplementedError", python_exceptions.database);
  python_exceptions.programming = AddException(module, "ProgrammingError", python_exceptions.bumblebee);
  python_exceptions.data = AddException(module, "DataError", python_exceptions.bumblebee);

  py::register_exception_translator([](std::exception_ptr exception) {
    if (exception == nullptr) {
      return;
    }
    try {
      std::rethrow_exception(exception);
    } catch (py::error_already_set &error) {
      error.restore();
    } catch (const std::filesystem::filesystem_error &error) {
      PyErr_SetString(PyExc_OSError, error.what());
    } catch (const Exception &error) {
      PyObject *type = python_exceptions.database;
      switch (error.GetType()) {
        case ExceptionType::DATABASE_CLOSED:
          type = python_exceptions.database_closed;
          break;
        case ExceptionType::PARSER:
          type = python_exceptions.parser;
          break;
        case ExceptionType::BINDER:
          type = python_exceptions.binder;
          break;
        case ExceptionType::PLANNER:
          type = python_exceptions.planner;
          break;
        case ExceptionType::EXECUTION:
        case ExceptionType::OUT_OF_MEMORY:
          type = python_exceptions.execution;
          break;
        case ExceptionType::CONFLICT:
          type = python_exceptions.conflict;
          break;
        case ExceptionType::NOT_IMPLEMENTED:
          type = python_exceptions.not_implemented;
          break;
        case ExceptionType::PROGRAMMING:
          type = python_exceptions.programming;
          break;
        case ExceptionType::DATA:
        case ExceptionType::OUT_OF_RANGE:
        case ExceptionType::CONVERSION:
        case ExceptionType::UNKNOWN_TYPE:
        case ExceptionType::DECIMAL:
        case ExceptionType::MISMATCH_TYPE:
        case ExceptionType::DIVIDE_BY_ZERO:
        case ExceptionType::INCOMPATIBLE_TYPE:
          type = python_exceptions.data;
          break;
        case ExceptionType::INVALID:
          break;
      }
      PyErr_SetString(type, error.what());
    } catch (const std::invalid_argument &error) {
      PyErr_SetString(PyExc_ValueError, error.what());
    } catch (const std::bad_alloc &error) {
      PyErr_SetString(PyExc_MemoryError, error.what());
    }
  });
}

}  // namespace bumblebee::python
