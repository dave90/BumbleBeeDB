//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// python_utils.cpp
//
// Identification: python/src/python_utils.cpp
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#include "include/python_utils.h"

#include <cerrno>
#include <chrono>  // NOLINT
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

#include "common/config.h"

namespace py = pybind11;

namespace bumblebee::python {

static auto ParseUnsignedOption(const py::object &value, const char *name, uint64_t default_value, bool allow_zero)
    -> uint64_t {
  if (value.is_none()) {
    return default_value;
  }
  if (PyBool_Check(value.ptr()) != 0 || PyLong_Check(value.ptr()) == 0) {
    throw py::value_error(std::string(name) + " must be an integer");
  }
  const auto parsed = PyLong_AsUnsignedLongLong(value.ptr());
  if (PyErr_Occurred() != nullptr) {
    PyErr_Clear();
    throw py::value_error(std::string(name) + " must be a non-negative integer in range");
  }
  if (!allow_zero && parsed == 0) {
    throw py::value_error(std::string(name) + " must be greater than zero");
  }
  return parsed;
}

static auto ParseTimeout(const py::object &value) -> duration_t {
  if (value.is_none()) {
    return DEFAULT_TXN_TIMEOUT;
  }
  if (PyBool_Check(value.ptr()) != 0 || (PyLong_Check(value.ptr()) == 0 && PyFloat_Check(value.ptr()) == 0)) {
    throw py::value_error("transaction_timeout must be a positive number of seconds");
  }
  const double seconds = PyFloat_AsDouble(value.ptr());
  if (PyErr_Occurred() != nullptr || !(seconds > 0.0) || !std::isfinite(seconds)) {
    PyErr_Clear();
    throw py::value_error("transaction_timeout must be a finite positive number of seconds");
  }
  return std::chrono::duration_cast<duration_t>(std::chrono::duration<double>(seconds));
}

auto ParsePath(const py::object &path) -> std::optional<std::filesystem::path> {
  if (path.is_none()) {
    return std::nullopt;
  }
  auto fspath = py::module_::import("os").attr("fspath")(path);
  if (!py::isinstance<py::str>(fspath)) {
    throw py::type_error("path must resolve to a text filesystem path");
  }
  auto text = fspath.cast<std::string>();
  if (text.empty()) {
    throw py::value_error("path must not be empty");
  }
  return std::filesystem::path(std::move(text));
}

auto ParseRequiredPath(const py::object &path) -> std::filesystem::path {
  auto parsed = ParsePath(path);
  if (!parsed.has_value()) {
    throw py::type_error("path must be a string or os.PathLike, not None");
  }
  return std::move(*parsed);
}

auto ReadFileBytes(const std::filesystem::path &path) -> std::string {
  std::error_code status_error;
  const auto status = std::filesystem::status(path, status_error);
  if (status_error) {
    throw std::filesystem::filesystem_error("cannot inspect SQL file", path, status_error);
  }
  if (!std::filesystem::exists(status)) {
    throw std::filesystem::filesystem_error("cannot open SQL file", path,
                                            std::make_error_code(std::errc::no_such_file_or_directory));
  }
  if (!std::filesystem::is_regular_file(status)) {
    throw std::filesystem::filesystem_error("SQL path is not a regular file", path,
                                            std::make_error_code(std::errc::is_a_directory));
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    auto code = std::error_code(errno == 0 ? EIO : errno, std::generic_category());
    throw std::filesystem::filesystem_error("cannot open SQL file", path, code);
  }
  std::ostringstream contents;
  contents << stream.rdbuf();
  if (stream.bad()) {
    auto code = std::error_code(errno == 0 ? EIO : errno, std::generic_category());
    throw std::filesystem::filesystem_error("cannot read SQL file", path, code);
  }
  return std::move(contents).str();
}

auto ValidateUtf8(std::string bytes) -> std::string {
  auto *decoded = PyUnicode_DecodeUTF8(bytes.data(), static_cast<Py_ssize_t>(bytes.size()), "strict");
  if (decoded == nullptr) {
    throw py::error_already_set();
  }
  auto unicode = py::reinterpret_steal<py::str>(decoded);
  return unicode.cast<std::string>();
}

auto QuoteIdentifier(const std::string &name) -> std::string {
  if (name.empty() || name.find('\0') != std::string::npos) {
    throw std::invalid_argument("table name must be a non-empty string without NUL bytes");
  }
  std::string quoted;
  quoted.reserve(name.size() + 2);
  quoted.push_back('"');
  for (const auto character : name) {
    if (character == '"') {
      quoted.push_back('"');
    }
    quoted.push_back(character);
  }
  quoted.push_back('"');
  return quoted;
}

auto ParseIsolation(const std::string &isolation) -> IsolationLevel {
  if (isolation == "snapshot") {
    return IsolationLevel::SNAPSHOT_ISOLATION;
  }
  if (isolation == "serializable") {
    return IsolationLevel::SERIALIZABLE;
  }
  throw std::invalid_argument("isolation must be 'snapshot' or 'serializable'");
}

auto MakeConfig(const py::object &worker_threads, const py::object &max_memory, const py::object &frames,
                const py::object &morsel_pages, const py::object &transaction_timeout,
                const py::object &prefer_external) -> DatabaseConfig {
  DatabaseConfig config;
  config.worker_threads_ = ParseUnsignedOption(worker_threads, "worker_threads", 0, true);
  if (config.worker_threads_ > MAX_THREADS) {
    throw py::value_error("worker_threads exceeds BumbleBeeDB's native maximum");
  }
  config.max_memory_ = ParseUnsignedOption(max_memory, "max_memory", MAX_MEMORY, false);
  const auto parsed_frames = ParseUnsignedOption(frames, "frames", BUFFER_POOL_SIZE, false);
  if (parsed_frames > std::numeric_limits<size_t>::max()) {
    throw py::value_error("frames is too large");
  }
  config.frames_ = static_cast<size_t>(parsed_frames);
  config.morsel_pages_ = ParseUnsignedOption(morsel_pages, "morsel_pages", MORSEL_PAGES, false);
  config.transaction_timeout_ = ParseTimeout(transaction_timeout);
  if (PyBool_Check(prefer_external.ptr()) == 0) {
    throw py::value_error("prefer_external must be a bool");
  }
  config.prefer_external_ = prefer_external.cast<bool>();
  return config;
}

}  // namespace bumblebee::python
