cmake_minimum_required(VERSION 3.31)

# When building via pip (scikit-build-core sets SKBUILD) on macOS, prefer
# Apple's system Clang over any Homebrew LLVM that may appear first in PATH.
# Must be set before the first project() call.
if(SKBUILD AND APPLE)
    if(NOT DEFINED CMAKE_C_COMPILER)
        set(CMAKE_C_COMPILER /usr/bin/clang CACHE FILEPATH "" FORCE)
    endif()
    if(NOT DEFINED CMAKE_CXX_COMPILER)
        set(CMAKE_CXX_COMPILER /usr/bin/clang++ CACHE FILEPATH "" FORCE)
    endif()
endif()

project(BumbleBee LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")

# ============================================================================
# Third-party dependencies.
#
# ALL of these are declared BEFORE the add_compile_options() calls below, and
# that ordering is load-bearing. Directory-level compile options only propagate
# to subdirectories (and FetchContent'd projects) added *after* them, so keeping
# third parties above the line builds them with their own intended flags instead
# of ours: no force-included C++20 prelude, no AddressSanitizer, no -Werror.
# Moving any of this below the line breaks the build (fmt's consteval FMT_STRING
# machinery and libpg_query's generated C++17 both fail under our settings).
# ============================================================================

# DuckDB's fork of libpg_query — the SQL parser. Vendored in-tree.
add_subdirectory(third_party/libpg_query)

include(FetchContent)

# fmt is not optional: the ToString() layer of the plan / expression / binder
# trees is built on fmt::formatter partial specializations and fmt::join.
# std::format cannot format ranges before C++23, so there is no drop-in
# standard replacement.
# 11.1.3, not 11.0.x: 11.0.2's consteval FMT_STRING machinery miscompiles on
# recent Clang ("call to consteval function ... is not a constant expression").
FetchContent_Declare(
        fmt
        URL https://github.com/fmtlib/fmt/archive/refs/tags/11.1.3.zip
)
FetchContent_MakeAvailable(fmt)

FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

# ============================================================================
# Project-wide compile options. Everything below here is OUR code.
# ============================================================================

# Force-include a small prelude of standard headers into every translation
# unit. libstdc++ (GCC) does not transitively expose as many standard headers
# as libc++, so sources that relied on those transitive includes fail to build.
# See src/include/std_prelude.hpp. (GCC/Clang flag; skip on MSVC.)
if(NOT MSVC)
    add_compile_options(-include ${CMAKE_SOURCE_DIR}/src/include/std_prelude.hpp)
endif()

# Enable AddressSanitizer only for Debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Building with AddressSanitizer in Debug mode")
    add_compile_options(-fsanitize=address -O0 -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# Third-party includes
include_directories(${CMAKE_SOURCE_DIR}/third_party)

# ----------------------------------------
# Build project code as a library
# ----------------------------------------
file(GLOB_RECURSE SRC_FILES
        ${PROJECT_SOURCE_DIR}/src/*.cpp
        ${PROJECT_SOURCE_DIR}/third_party/*.cpp
        ${PROJECT_SOURCE_DIR}/third_party/*.cc
)

# Exclude main.cpp from the library
list(FILTER SRC_FILES EXCLUDE REGEX ".*main\\.cpp")

# libpg_query has its own target (see add_subdirectory above); compiling it into
# bumblebee_lib as well would duplicate every symbol.
list(FILTER SRC_FILES EXCLUDE REGEX ".*/third_party/libpg_query/.*")

add_library(bumblebee_lib ${SRC_FILES})
target_include_directories(bumblebee_lib PUBLIC
        ${PROJECT_SOURCE_DIR}/src/include
)
target_link_libraries(bumblebee_lib PUBLIC duckdb_pg_query fmt::fmt)
if(NOT APPLE)
    target_link_libraries(bumblebee_lib PUBLIC uuid)
endif()

# Disable asserts in Release; set log level to WARNING in Release
target_compile_definitions(bumblebee_lib
        PRIVATE
        $<$<CONFIG:Release>:NDEBUG>
        $<$<CONFIG:Release>:LOG_LEVEL=2>
)

# ----------------------------------------
# Main application executable
# ----------------------------------------
add_executable(${PROJECT_NAME} src/main/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE bumblebee_lib)

# ----------------------------------------
# Unit tests
# ----------------------------------------
enable_testing()

# Unit test sources
file(GLOB_RECURSE UNIT_TEST_SOURCES
        ${PROJECT_SOURCE_DIR}/test/unit/*.cpp
)

add_executable(unit_tests ${UNIT_TEST_SOURCES} )

target_link_libraries(unit_tests
        bumblebee_lib
        gtest_main
)

target_include_directories(unit_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/test/unit/include
)

include(GoogleTest)
gtest_discover_tests(unit_tests)