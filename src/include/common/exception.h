//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// exception.h
//
// Identification: src/include/common/exception.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>

namespace bumblebee {

/** ExceptionType is all the types of exceptions that we expect to throw in our system. */
enum class ExceptionType {
  /** Invalid exception type. */
  INVALID = 0,
  /** Value out of range. */
  OUT_OF_RANGE = 1,
  /** Conversion / casting error. */
  CONVERSION = 2,
  /** Unknown type in the type subsystem. */
  UNKNOWN_TYPE = 3,
  /** Decimal-related errors. */
  DECIMAL = 4,
  /** Type mismatch. */
  MISMATCH_TYPE = 5,
  /** Division by zero. */
  DIVIDE_BY_ZERO = 6,
  /** Incompatible type. */
  INCOMPATIBLE_TYPE = 7,
  /** Out of memory. */
  OUT_OF_MEMORY = 8,
  /** Method not implemented. */
  NOT_IMPLEMENTED = 9,
  /** The SQL text could not be parsed. */
  PARSER = 10,
  /** The statement could not be bound against the catalog. */
  BINDER = 11,
  /** The bound statement could not be planned. */
  PLANNER = 12,
  /** Execution error. */
  EXECUTION = 13,
};

/** Set to true to silence the exception messages the constructors print in debug builds. */
extern std::atomic<bool> global_disable_exception_print;

/**
 * @brief The base class of every exception BumbleBee throws.
 */
class Exception : public std::runtime_error {
 public:
  /**
   * @brief Construct a new Exception with no specific type.
   *
   * @param message The exception message.
   */
  explicit Exception(const std::string &message) : Exception(ExceptionType::INVALID, message) {}

  /**
   * @brief Construct a new Exception of the given type.
   *
   * @param exception_type The exception type.
   * @param message The exception message.
   */
  Exception(ExceptionType exception_type, const std::string &message)
      : std::runtime_error(message), type_(exception_type) {
#ifndef NDEBUG
    if (!global_disable_exception_print.load()) {
      std::cerr << "\nException Type :: " << ExceptionTypeToString(type_) << ", Message :: " << message << "\n\n";
    }
#endif
  }

  /** @return The type of this exception. */
  auto GetType() const -> ExceptionType { return type_; }

  /**
   * @brief Render an exception type as a human-readable string.
   *
   * @param type The exception type.
   * @return std::string The human-readable name.
   */
  static auto ExceptionTypeToString(ExceptionType type) -> std::string {
    switch (type) {
      case ExceptionType::INVALID:
        return "Invalid";
      case ExceptionType::OUT_OF_RANGE:
        return "Out of Range";
      case ExceptionType::CONVERSION:
        return "Conversion";
      case ExceptionType::UNKNOWN_TYPE:
        return "Unknown Type";
      case ExceptionType::DECIMAL:
        return "Decimal";
      case ExceptionType::MISMATCH_TYPE:
        return "Mismatch Type";
      case ExceptionType::DIVIDE_BY_ZERO:
        return "Divide by Zero";
      case ExceptionType::INCOMPATIBLE_TYPE:
        return "Incompatible Type";
      case ExceptionType::OUT_OF_MEMORY:
        return "Out of Memory";
      case ExceptionType::NOT_IMPLEMENTED:
        return "Not Implemented";
      case ExceptionType::PARSER:
        return "Parser";
      case ExceptionType::BINDER:
        return "Binder";
      case ExceptionType::PLANNER:
        return "Planner";
      case ExceptionType::EXECUTION:
        return "Execution";
      default:
        return "Unknown";
    }
  }

 private:
  ExceptionType type_;
};

/** @brief Thrown when the SQL text cannot be parsed. */
class ParserException : public Exception {
 public:
  explicit ParserException(const std::string &message) : Exception(ExceptionType::PARSER, message) {}
};

/** @brief Thrown when a statement cannot be bound (unknown table, unknown column, ambiguity, ...). */
class BinderException : public Exception {
 public:
  explicit BinderException(const std::string &message) : Exception(ExceptionType::BINDER, message) {}
};

/** @brief Thrown when a bound statement cannot be turned into a plan. */
class PlannerException : public Exception {
 public:
  explicit PlannerException(const std::string &message) : Exception(ExceptionType::PLANNER, message) {}
};

/** @brief Thrown when a code path has not been implemented yet. */
class NotImplementedException : public Exception {
 public:
  explicit NotImplementedException(const std::string &message)
      : Exception(ExceptionType::NOT_IMPLEMENTED, message) {}
};

}  // namespace bumblebee
