# soatins — the reusable "describe a struct once → SoA + Arrow" nucleus, split out of nanotins so it can
# be vendored on its own. One BOOST_DESCRIBE_STRUCT on a packed be<>/le<>/bits<> struct gives you (a) a
# zero-copy overlay on raw bytes, (b) a flattened column list, (c) an SoA store (+ a device-fillable
# soa_ptrs view), and (d) a nanoarrow ArrowSchema/ArrowArray. Depends ONLY on nanoarrow + header-only
# boost (describe/mp11). It knows nothing about packets, stdexec, CUDA, or Lance — those live one layer up
# (nanotins / gputins). Namespace: `soatins`. Include prefix: `soatins/`.
#
# Consumed in-tree via add_subdirectory() (nanotins does this when soatins_core isn't already a target).
# Standalone: if nanoarrow isn't already a target, it is FetchContent'd.

cmake_minimum_required(VERSION 3.20)
project(soatins CXX)

include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)  # allow single-arg FetchContent_Populate for header-only deps
endif()

option(SOATINS_BUILD_TESTS "Build soatins unit tests" ${PROJECT_IS_TOP_LEVEL})

# --- header-only boost (only what boost::describe needs) -------------------------------------------
set(_SOA_BOOST_TAG boost-1.86.0)
function(_soa_fetch_boost_header name out_include_dir)
    FetchContent_Declare(${name}
        GIT_REPOSITORY https://github.com/boostorg/${name}.git
        GIT_TAG ${_SOA_BOOST_TAG}
        GIT_SHALLOW TRUE)
    FetchContent_GetProperties(${name})
    if(NOT ${name}_POPULATED)
        FetchContent_Populate(${name})
    endif()
    set(${out_include_dir} "${${name}_SOURCE_DIR}/include" PARENT_SCOPE)
endfunction()
if(NOT TARGET soatins_boost)
    _soa_fetch_boost_header(config _soa_inc_config)
    _soa_fetch_boost_header(assert _soa_inc_assert)
    _soa_fetch_boost_header(preprocessor _soa_inc_preprocessor)
    _soa_fetch_boost_header(mp11 _soa_inc_mp11)
    _soa_fetch_boost_header(describe _soa_inc_describe)

    add_library(soatins_boost INTERFACE)
    add_library(soatins::boost ALIAS soatins_boost)
    target_include_directories(soatins_boost INTERFACE
        "${_soa_inc_config}" "${_soa_inc_assert}" "${_soa_inc_preprocessor}" "${_soa_inc_mp11}" "${_soa_inc_describe}")
endif()

# --- nanoarrow (for the ArrowSchema/ArrowArray output) --------------------------------------------
# Reuse the parent build's target when present; otherwise fetch it (standalone builds).
if(NOT TARGET nanoarrow_static)
    set(NANOARROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NANOARROW_BUILD_APPS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nanoarrow
        GIT_REPOSITORY https://github.com/apache/arrow-nanoarrow.git
        GIT_TAG ffe61d3cd2d02da9e60cfc405cd6c50c512f64b9
        GIT_SHALLOW TRUE)
    FetchContent_Populate(nanoarrow)
    add_subdirectory(${nanoarrow_SOURCE_DIR} ${nanoarrow_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# --- the library target ---------------------------------------------------------------------------
# soatins_core: header-only reflection/overlay/SoA/arrow + endian/bits/fixed_string. The reusable nucleus.
if(NOT TARGET soatins_core)
    add_library(soatins_core INTERFACE)
    add_library(soatins::core ALIAS soatins_core)
    target_include_directories(soatins_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_features(soatins_core INTERFACE cxx_std_20)
    target_link_libraries(soatins_core INTERFACE soatins_boost nanoarrow_static)
endif()

if(SOATINS_BUILD_TESTS)
    enable_testing()
    add_executable(soatins_reflect_smoke tests/test_reflect_smoke.cpp)
    target_link_libraries(soatins_reflect_smoke PRIVATE soatins_core)
    add_test(NAME soatins_reflect_smoke COMMAND $<TARGET_FILE:soatins_reflect_smoke>)
    set_tests_properties(soatins_reflect_smoke PROPERTIES LABELS "smoke")

    add_executable(soatins_soa_scatter tests/test_soa_scatter.cpp)
    target_link_libraries(soatins_soa_scatter PRIVATE soatins_core)
    add_test(NAME soatins_soa_scatter COMMAND $<TARGET_FILE:soatins_soa_scatter>)
    set_tests_properties(soatins_soa_scatter PROPERTIES LABELS "smoke")

    add_executable(soatins_soa_fixed tests/test_soa_fixed.cpp)
    target_link_libraries(soatins_soa_fixed PRIVATE soatins_core)
    add_test(NAME soatins_soa_fixed COMMAND $<TARGET_FILE:soatins_soa_fixed>)
    set_tests_properties(soatins_soa_fixed PROPERTIES LABELS "smoke")

    add_executable(soatins_soa_fixed_arrow tests/test_soa_fixed_arrow.cpp)
    target_link_libraries(soatins_soa_fixed_arrow PRIVATE soatins_core)
    add_test(NAME soatins_soa_fixed_arrow COMMAND $<TARGET_FILE:soatins_soa_fixed_arrow>)
    set_tests_properties(soatins_soa_fixed_arrow PROPERTIES LABELS "smoke")
endif()
