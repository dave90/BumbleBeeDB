//===----------------------------------------------------------------------===//
//
//                         BumbleBee
//
// logger.h
//
// Identification: src/include/common/logger.h
//
// Copyright (C) 2025 Davide Fuscà
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdio>
#include <ctime>

namespace bumblebee {

#define LOG_LEVEL_OFF 1000
#define LOG_LEVEL_ERROR 500
#define LOG_LEVEL_WARN 400
#define LOG_LEVEL_INFO 300
#define LOG_LEVEL_DEBUG 200
#define LOG_LEVEL_TRACE 100
#define LOG_LEVEL_ALL 0

#define LOG_OUTPUT_STREAM stdout

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

#define LOG_LOG_TIME_FORMAT "%Y-%m-%d %H:%M:%S"
#define LOG_OUTPUT_STREAM stdout

/** @brief Print the current timestamp to the log stream, without a trailing newline. */
inline void OutputLogHeader(const char *file, int line, const char *func, const char *level) {
  time_t t = ::time(nullptr);
  tm *cur_time = ::localtime(&t);  // NOLINT
  char time_str[32];
  ::strftime(time_str, 32, LOG_LOG_TIME_FORMAT, cur_time);
  ::fprintf(LOG_OUTPUT_STREAM, "%s [%s:%d:%s] %s - ", time_str, file, line, func, level);
}

#define LOG_INTERNAL(level_name, ...)                                          \
  do {                                                                         \
    ::bumblebee::OutputLogHeader(__FILE__, __LINE__, __func__, (level_name));  \
    ::fprintf(LOG_OUTPUT_STREAM, __VA_ARGS__);                                 \
    ::fprintf(LOG_OUTPUT_STREAM, "\n");                                        \
    ::fflush(LOG_OUTPUT_STREAM);                                               \
  } while (0)

#if LOG_LEVEL <= LOG_LEVEL_ERROR
#define LOG_ERROR(...) LOG_INTERNAL("ERROR", __VA_ARGS__)
#else
#define LOG_ERROR(...) ((void)0)
#endif

#if LOG_LEVEL <= LOG_LEVEL_WARN
#define LOG_WARN(...) LOG_INTERNAL("WARN", __VA_ARGS__)
#else
#define LOG_WARN(...) ((void)0)
#endif

#if LOG_LEVEL <= LOG_LEVEL_INFO
#define LOG_INFO(...) LOG_INTERNAL("INFO", __VA_ARGS__)
#else
#define LOG_INFO(...) ((void)0)
#endif

#if LOG_LEVEL <= LOG_LEVEL_DEBUG
#define LOG_DEBUG(...) LOG_INTERNAL("DEBUG", __VA_ARGS__)
#else
#define LOG_DEBUG(...) ((void)0)
#endif

}  // namespace bumblebee
