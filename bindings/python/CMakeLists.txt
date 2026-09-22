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

# nanoarrow and zstd are brought in with add_subdirectory(... EXCLUDE_FROM_ALL), NOT
# FetchContent_MakeAvailable, and the reason is the WHEEL. MakeAvailable runs a dependency's own
# install() rules as part of ours, and scikit-build-core packages whatever `cmake --install` writes.
# The result shipped 2 MB of nanoarrow/zstd HEADERS into site-packages/include/, plus
# lib/cmake/*Config.cmake, lib/pkgconfig/*.pc, and two libnanoarrow*_shared.so that nothing loads --
# the extension links the static libraries. EXCLUDE_FROM_ALL suppresses those install rules (and
# stops the unused shared libraries being built at all), leaving a wheel that is just the extension
# and the Python package.
#
# --- nanoarrow with IPC (required by nanolance) ---
set(NANOARROW_IPC ON CACHE BOOL "" FORCE)
set(NANOARROW_IPC_WITH_ZSTD OFF CACHE BOOL "" FORCE)
set(NANOARROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NANOARROW_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nanoarrow
    GIT_REPOSITORY https://github.com/apache/arrow-nanoarrow.git
    GIT_TAG ffe61d3cd2d02da9e60cfc405cd6c50c512f64b9
    GIT_SHALLOW FALSE
)
FetchContent_Populate(nanoarrow)
add_subdirectory("${nanoarrow_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_nanolance_nanoarrow_build" EXCLUDE_FROM_ALL)

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
        GIT_SHALLOW TRUE
    )
    FetchContent_Populate(_nanolance_py_zstd)
    add_subdirectory("${_nanolance_py_zstd_SOURCE_DIR}/build/cmake"
                     "${CMAKE_BINARY_DIR}/_nanolance_zstd_build" EXCLUDE_FROM_ALL)
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

# COMPONENT + install.components in pyproject.toml: a second guard on the same problem as the
# EXCLUDE_FROM_ALL above. If a dependency ever does install unconditionally, naming the component
# keeps it out of the wheel rather than quietly adding megabytes to it.
install(TARGETS _nanolance LIBRARY DESTINATION nanolance COMPONENT nanolance_python)
