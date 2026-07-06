# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor

cmake_minimum_required(VERSION 3.24)
project(nanolance_python LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(NL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

if(NOT DEFINED FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${NL_ROOT}/.deps" CACHE PATH "Fetched dependency sources")
endif()

include(FetchContent)

# --- nanoarrow with IPC (required by nanolance) ---
set(NANOARROW_IPC ON CACHE BOOL "" FORCE)
set(NANOARROW_IPC_WITH_ZSTD OFF CACHE BOOL "" FORCE)
set(NANOARROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NANOARROW_BUILD_APPS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nanoarrow
    GIT_REPOSITORY https://github.com/apache/arrow-nanoarrow.git
    GIT_TAG ffe61d3cd2d02da9e60cfc405cd6c50c512f64b9
    GIT_SHALLOW FALSE
)
FetchContent_MakeAvailable(nanoarrow)

# --- zstd ---
if(NOT TARGET libzstd_static)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        _nanolance_py_zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        SOURCE_SUBDIR build/cmake
    )
    FetchContent_MakeAvailable(_nanolance_py_zstd)
endif()

# --- nanolance library (in-tree when building from this repo) ---
set(NANOLANCE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NANOLANCE_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(NANOLANCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NANOLANCE_BUILD_FUSE_EXAMPLE OFF CACHE BOOL "" FORCE)
set(NANOLANCE_ENABLE_S3 OFF CACHE BOOL "" FORCE)
add_subdirectory(${NL_ROOT} ${CMAKE_BINARY_DIR}/nanolance_lib EXCLUDE_FROM_ALL)

# --- nanobind extension ---
find_package(Python 3.9 REQUIRED COMPONENTS Interpreter Development.Module)
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE nanobind_ROOT
)
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_nanolance STABLE_ABI NB_STATIC src/nanolance_bindings.cpp)
target_include_directories(_nanolance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(_nanolance PRIVATE nanolance::nanolance)

install(TARGETS _nanolance LIBRARY DESTINATION nanolance)
