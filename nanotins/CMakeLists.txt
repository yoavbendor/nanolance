# nanotins — a small, reusable C++20 library for parsing pcap/pcapng + L2/L3/L4 (sequential AND bulk)
# whose killer feature is *overlay-able, self-describing wire structs*: one BOOST_DESCRIBE_STRUCT line on
# a packed be<>/le<>/bits<> struct gives you (a) a zero-copy overlay on raw bytes, (b) a flattened column
# list, (c) an SoA store, and (d) a nanoarrow ArrowSchema + ArrowArray — so any backend (Lance, Parquet,
# IPC) can persist it. Depends ONLY on nanoarrow + header-only boost (describe/mp11) + stdexec; it knows
# nothing about Lance. See docs/nanotins.html for the ideas and how to extend it (e.g. gPTP).
#
# Consumed in-tree via add_subdirectory() (the pcapng2lance example does this). Also configures
# standalone: if nanoarrow isn't already a target, it is FetchContent'd.

cmake_minimum_required(VERSION 3.20)
project(nanotins CXX)

include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)  # allow single-arg FetchContent_Populate for header-only deps
endif()

option(NANOTINS_BUILD_TESTS "Build nanotins unit tests" ${PROJECT_IS_TOP_LEVEL})
option(NANOTINS_ENABLE_CUDA "Build nanotins/stdexec using CUDA-capable settings" OFF)

# --- header-only boost (only what boost::describe needs) -------------------------------------------
set(_NT_BOOST_TAG boost-1.86.0)
function(_nt_fetch_boost_header name out_include_dir)
    FetchContent_Declare(${name}
        GIT_REPOSITORY https://github.com/boostorg/${name}.git
        GIT_TAG ${_NT_BOOST_TAG}
        GIT_SHALLOW TRUE)
    FetchContent_GetProperties(${name})
    if(NOT ${name}_POPULATED)
        FetchContent_Populate(${name})
    endif()
    set(${out_include_dir} "${${name}_SOURCE_DIR}/include" PARENT_SCOPE)
endfunction()
_nt_fetch_boost_header(config _nt_inc_config)
_nt_fetch_boost_header(assert _nt_inc_assert)
_nt_fetch_boost_header(preprocessor _nt_inc_preprocessor)
_nt_fetch_boost_header(mp11 _nt_inc_mp11)
_nt_fetch_boost_header(describe _nt_inc_describe)

add_library(nanotins_boost INTERFACE)
add_library(nanotins::boost ALIAS nanotins_boost)
target_include_directories(nanotins_boost INTERFACE
    "${_nt_inc_config}" "${_nt_inc_assert}" "${_nt_inc_preprocessor}" "${_nt_inc_mp11}" "${_nt_inc_describe}")

# --- stdexec (header-only) for the scheduler-agnostic ex::bulk path --------------------------------
# CPU today via exec::static_thread_pool; on a CUDA host the same bulk_for_each takes nvexec::stream_context.
set(_NT_STDEXEC_TAG_DEFAULT main)
if(NANOTINS_ENABLE_CUDA)
    set(_NT_STDEXEC_TAG_DEFAULT nvhpc-24.09)
endif()
set(NANOTINS_STDEXEC_TAG "${_NT_STDEXEC_TAG_DEFAULT}" CACHE STRING
    "stdexec git tag/branch for nanotins (use nvhpc-24.09 with clang-cuda + nvexec)")
if(NANOTINS_ENABLE_CUDA AND NANOTINS_STDEXEC_TAG STREQUAL "main")
    set(NANOTINS_STDEXEC_TAG nvhpc-24.09 CACHE STRING
        "stdexec git tag/branch for nanotins (use nvhpc-24.09 with clang-cuda + nvexec)" FORCE)
endif()
FetchContent_Declare(stdexec
    GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
    GIT_TAG ${NANOTINS_STDEXEC_TAG}
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(stdexec)
if(NOT stdexec_POPULATED)
    FetchContent_Populate(stdexec)
endif()
add_library(nanotins_stdexec INTERFACE)
add_library(nanotins::stdexec ALIAS nanotins_stdexec)
target_include_directories(nanotins_stdexec INTERFACE "${stdexec_SOURCE_DIR}/include")
target_compile_features(nanotins_stdexec INTERFACE cxx_std_20)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(nanotins_stdexec INTERFACE Threads::Threads)
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

# --- the library targets --------------------------------------------------------------------------
# nanotins_core: header-only reflection/overlay/SoA/arrow + endian/bits/bulk. The reusable nucleus.
add_library(nanotins_core INTERFACE)
add_library(nanotins::core ALIAS nanotins_core)
target_include_directories(nanotins_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(nanotins_core INTERFACE cxx_std_20)
target_link_libraries(nanotins_core INTERFACE nanotins_boost nanotins_stdexec nanoarrow_static)

# nanotins_pcap: the pcap/pcapng block scanner (sequential boundary walk + streaming scan_window) and the
# pure per-block parse. Header-only: the Phase-B parsers are device-callable (NANOTINS_HD), so they must be
# visible to device code — hence inline in the header, no compiled TU. INTERFACE = just the headers.
add_library(nanotins_pcap INTERFACE)
add_library(nanotins::pcap ALIAS nanotins_pcap)
target_link_libraries(nanotins_pcap INTERFACE nanotins_core)

# nanotins_protocols: L2/L3/L4 wire structs + the layered decode (serial + scheduler-agnostic bulk) and
# the gPTP extension. Header-only (overlay+store kernels), so an INTERFACE target.
add_library(nanotins_protocols INTERFACE)
add_library(nanotins::protocols ALIAS nanotins_protocols)
target_link_libraries(nanotins_protocols INTERFACE nanotins_core)

# Umbrella: link this to get the whole stack (core + pcap + protocols).
add_library(nanotins INTERFACE)
add_library(nanotins::nanotins ALIAS nanotins)
target_link_libraries(nanotins INTERFACE nanotins_core nanotins_pcap nanotins_protocols)

if(NANOTINS_BUILD_TESTS)
    enable_testing()
    add_executable(nanotins_reflect_smoke tests/test_reflect_smoke.cpp)
    target_link_libraries(nanotins_reflect_smoke PRIVATE nanotins_core)
    add_test(NAME nanotins_reflect_smoke COMMAND $<TARGET_FILE:nanotins_reflect_smoke>)
    set_tests_properties(nanotins_reflect_smoke PROPERTIES LABELS "smoke")

    add_executable(nanotins_protocols_test tests/test_protocols.cpp)
    target_link_libraries(nanotins_protocols_test PRIVATE nanotins_core nanotins_protocols)
    add_test(NAME nanotins_protocols COMMAND $<TARGET_FILE:nanotins_protocols_test>)
    set_tests_properties(nanotins_protocols PROPERTIES LABELS "smoke")

    add_executable(nanotins_pcap_blocks_test tests/test_pcap_blocks.cpp)
    target_link_libraries(nanotins_pcap_blocks_test PRIVATE nanotins_pcap)
    add_test(NAME nanotins_pcap_blocks COMMAND $<TARGET_FILE:nanotins_pcap_blocks_test>)
    set_tests_properties(nanotins_pcap_blocks PROPERTIES LABELS "smoke")

    add_executable(nanotins_bulk_smoke tests/test_bulk_smoke.cpp)
    target_link_libraries(nanotins_bulk_smoke PRIVATE nanotins_core)
    add_test(NAME nanotins_bulk_smoke COMMAND $<TARGET_FILE:nanotins_bulk_smoke>)
    set_tests_properties(nanotins_bulk_smoke PROPERTIES LABELS "smoke")

    add_executable(nanotins_soa_scatter tests/test_soa_scatter.cpp)
    target_link_libraries(nanotins_soa_scatter PRIVATE nanotins_core)
    add_test(NAME nanotins_soa_scatter COMMAND $<TARGET_FILE:nanotins_soa_scatter>)
    set_tests_properties(nanotins_soa_scatter PROPERTIES LABELS "smoke")

    add_executable(nanotins_gptp_test tests/test_gptp.cpp)
    target_link_libraries(nanotins_gptp_test PRIVATE nanotins_core nanotins_protocols)
    add_test(NAME nanotins_gptp COMMAND $<TARGET_FILE:nanotins_gptp_test>)
    set_tests_properties(nanotins_gptp PROPERTIES LABELS "smoke")
endif()
