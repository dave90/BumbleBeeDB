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

if(SKBUILD)
    set(BUMBLEBEE_DEFAULT_SHELL OFF)
    set(BUMBLEBEE_DEFAULT_TESTS OFF)
    set(BUMBLEBEE_DEFAULT_PYTHON ON)
else()
    set(BUMBLEBEE_DEFAULT_SHELL ON)
    set(BUMBLEBEE_DEFAULT_TESTS ON)
    set(BUMBLEBEE_DEFAULT_PYTHON OFF)
endif()
option(BUMBLEBEE_BUILD_SHELL "Build the BumbleBee interactive shell" ${BUMBLEBEE_DEFAULT_SHELL})
option(BUMBLEBEE_BUILD_TESTS "Build the native GoogleTest suite" ${BUMBLEBEE_DEFAULT_TESTS})
option(BUMBLEBEE_BUILD_PYTHON "Build the private Python extension" ${BUMBLEBEE_DEFAULT_PYTHON})

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
# A wheel must not inherit fmt's install rules: doing so bundles its static library,
# headers, pkg-config file, and CMake package metadata under site-packages/lib and
# site-packages/include. BumbleBeeDB links fmt into its private extension instead.
if(SKBUILD)
    set(FMT_INSTALL OFF CACHE BOOL "Do not install bundled fmt into Python wheels" FORCE)
endif()
FetchContent_MakeAvailable(fmt)

# The Python extension is a shared module.  Every static archive linked into it must be
# position-independent on ELF platforms; fmt does not enable PIC for static builds by default.
if(BUMBLEBEE_BUILD_PYTHON)
    set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

if(BUMBLEBEE_BUILD_TESTS)
    FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    FetchContent_MakeAvailable(googletest)
endif()

# ============================================================================
# Project-wide compile options. Everything below here is OUR code.
# ============================================================================

# Force-include a small prelude of standard headers into every translation
# unit. libstdc++ (GCC) does not transitively expose as many standard headers
# as libc++, so sources that relied on those transitive includes fail to build.
# See src/include/std_prelude.hpp. (GCC/Clang flag; skip on MSVC.)
# CXX-only: the prelude is a C++ header, so it must not be force-included into C sources
# (e.g. the vendored third_party/linenoise/linenoise.c).
if(NOT MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-include>
                        $<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/src/include/std_prelude.hpp>)
endif()

# Enable a sanitizer in Debug builds. Select with -DSANITIZER=address (default) or -DSANITIZER=thread.
# AddressSanitizer and ThreadSanitizer are mutually exclusive, so pick one: ASan catches memory bugs,
# TSan catches data races / deadlocks (the gate for the concurrency milestone). Release has none.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(NOT DEFINED SANITIZER)
        set(SANITIZER address)
    endif()
    message(STATUS "Building in Debug mode with -fsanitize=${SANITIZER}")
    # Full C++ template debug information makes GCC's UBSan compilation exceed the memory budget
    # of GitHub's hosted runners. Level 1 keeps function and source-line information for sanitizer
    # reports without emitting local-variable/type debug data for every template instantiation.
    if(SANITIZER STREQUAL "undefined")
        set(SANITIZER_DEBUG_FLAG -g1)
    else()
        set(SANITIZER_DEBUG_FLAG -g)
    endif()
    add_compile_options(-fsanitize=${SANITIZER} -O0 ${SANITIZER_DEBUG_FLAG} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${SANITIZER})
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
set_target_properties(bumblebee_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
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

# STANDARD_VECTOR_SIZE is a compile-time constant (it sizes fixed-extent arrays), so it can only be
# lowered by a rebuild. Configure a separate build tree with -DBBDB_VECTOR_SIZE=4 to produce a
# "small-vector" binary that exercises the multi-chunk paths on a handful of rows; the e2e harness
# points BBDB_SLT_BIN at it. Default 1024 = the production value.
set(BBDB_VECTOR_SIZE 1024 CACHE STRING "STANDARD_VECTOR_SIZE for this build (lower it for multi-chunk tests)")
target_compile_definitions(bumblebee_lib PUBLIC BUMBLEBEE_STANDARD_VECTOR_SIZE=${BBDB_VECTOR_SIZE})

if(BUMBLEBEE_BUILD_SHELL)
    # linenoise is a C target so the C++ force-include prelude never reaches it.
    add_library(linenoise STATIC third_party/linenoise/linenoise.c)
    target_include_directories(linenoise PUBLIC ${PROJECT_SOURCE_DIR}/third_party/linenoise)

    add_executable(${PROJECT_NAME} src/main/main.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE bumblebee_lib linenoise)
endif()

# ----------------------------------------
# Optional private Python extension (wheel builds enable this and disable shell/tests).
# ----------------------------------------
if(BUMBLEBEE_BUILD_PYTHON)
    find_package(pybind11 CONFIG REQUIRED)

    # scikit-build-core resolves the setuptools-scm version before configuring CMake. Keep a
    # deterministic fallback for direct CMake builds of the private extension.
    if(DEFINED SKBUILD_PROJECT_VERSION_FULL)
        set(BUMBLEBEEDB_VERSION "${SKBUILD_PROJECT_VERSION_FULL}")
    elseif(NOT DEFINED BUMBLEBEEDB_VERSION)
        set(BUMBLEBEEDB_VERSION "0.0.0")
    endif()

    file(GLOB_RECURSE PYTHON_SRC_FILES CONFIGURE_DEPENDS
            ${PROJECT_SOURCE_DIR}/python/src/*.cpp
    )
    pybind11_add_module(_native MODULE ${PYTHON_SRC_FILES})
    target_link_libraries(_native PRIVATE bumblebee_lib)
    target_include_directories(_native PRIVATE
            ${PROJECT_SOURCE_DIR}/src/include
            ${PROJECT_SOURCE_DIR}/python/src
    )
    target_compile_definitions(_native PRIVATE BUMBLEBEEDB_VERSION="${BUMBLEBEEDB_VERSION}")
    install(TARGETS _native
            LIBRARY DESTINATION bumblebeedb
            RUNTIME DESTINATION bumblebeedb)
endif()

# ----------------------------------------
# Unit tests
# ----------------------------------------
if(BUMBLEBEE_BUILD_TESTS)
    enable_testing()

    file(GLOB_RECURSE UNIT_TEST_SOURCES
            ${PROJECT_SOURCE_DIR}/test/unit/*.cpp
    )

    add_executable(unit_tests ${UNIT_TEST_SOURCES})
    target_link_libraries(unit_tests bumblebee_lib gtest_main)
    target_include_directories(unit_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/include
            ${PROJECT_SOURCE_DIR}/test/unit/include)

    include(GoogleTest)
    gtest_discover_tests(unit_tests)
endif()
