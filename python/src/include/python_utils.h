//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// python_utils.h
//
// Identification: python/src/include/python_utils.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <pybind11/pybind11.h>

#include <filesystem>
#include <optional>
#include <string>

#include "main/connection.h"
#include "main/database_config.h"

namespace bumblebee::python {

auto ParsePath(const pybind11::object &path) -> std::optional<std::filesystem::path>;
auto ParseRequiredPath(const pybind11::object &path) -> std::filesystem::path;
auto ReadFileBytes(const std::filesystem::path &path) -> std::string;
auto ValidateUtf8(std::string bytes) -> std::string;
auto QuoteIdentifier(const std::string &name) -> std::string;
auto ParseIsolation(const std::string &isolation) -> IsolationLevel;
auto MakeConfig(const pybind11::object &worker_threads, const pybind11::object &max_memory,
                const pybind11::object &frames, const pybind11::object &morsel_pages,
                const pybind11::object &transaction_timeout, const pybind11::object &prefer_external) -> DatabaseConfig;

}  // namespace bumblebee::python
