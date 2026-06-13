# nanotins — pcap/pcapng + L2/L3/L4 parsing (sequential AND a scheduler-agnostic bulk path). Built on the
# reusable `soatins` reflection nucleus (be<>/le<>/bits<>/SoA/arrow), which lives one layer down so it can
# be vendored independently. nanotins adds: the pcap/pcapng block scanner (pcapblocks), the L2/L3/L4 wire
# structs + layered decode (protocols), the gPTP extension, and the scheduler-agnostic bulk_for_each
# (nanotins) over stdexec. Depends on soatins + header-only stdexec. Knows nothing about Lance or CUDA
# (the CUDA executors live one layer up in `gputins`).
#
# Consumed in-tree via add_subdirectory() (the pcapng2lance example does this). Also configures
# standalone: it pulls in soatins (../soatins) and FetchContent's stdexec when they aren't already targets.

cmake_minimum_required(VERSION 3.20)
project(nanotins CXX)

include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)  # allow single-arg FetchContent_Populate for header-only deps
endif()

option(NANOTINS_BUILD_TESTS "Build nanotins unit tests" ${PROJECT_IS_TOP_LEVEL})

# --- soatins (the reflection nucleus) -------------------------------------------------------------
# Reuse the parent build's target when present; otherwise add the sibling source tree.
if(NOT TARGET soatins_core)
    set(SOATINS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../soatins" "${CMAKE_CURRENT_BINARY_DIR}/soatins")
endif()

# --- stdexec (header-only) for the scheduler-agnostic ex::bulk path --------------------------------
# CPU today via exec::static_thread_pool; on a CUDA host the same bulk_for_each takes nvexec::stream_context.
if(NOT TARGET nanotins_stdexec)
    FetchContent_Declare(stdexec
        GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
        GIT_TAG main
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
endif()

# --- the library targets --------------------------------------------------------------------------
# nanotins_pcap: the pcap/pcapng block scanner (sequential boundary walk + streaming scan_window) and the
# pure per-block parse. Header-only: the Phase-B parsers are device-callable (NANOTINS_HD), so they must be
# visible to device code — hence inline in the header, no compiled TU. INTERFACE = just the headers.
add_library(nanotins_pcap INTERFACE)
add_library(nanotins::pcap ALIAS nanotins_pcap)
target_include_directories(nanotins_pcap INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(nanotins_pcap INTERFACE cxx_std_20)
target_link_libraries(nanotins_pcap INTERFACE soatins_core)

# nanotins_protocols: L2/L3/L4 wire structs + the layered decode (serial + scheduler-agnostic bulk) and
# the gPTP extension + bulk_for_each. Header-only (overlay+store kernels), so an INTERFACE target. The bulk
# path needs stdexec.
add_library(nanotins_protocols INTERFACE)
add_library(nanotins::protocols ALIAS nanotins_protocols)
target_include_directories(nanotins_protocols INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(nanotins_protocols INTERFACE cxx_std_20)
target_link_libraries(nanotins_protocols INTERFACE soatins_core nanotins_stdexec)

# Umbrella: link this to get the whole stack (soatins + pcap + protocols).
add_library(nanotins INTERFACE)
add_library(nanotins::nanotins ALIAS nanotins)
target_link_libraries(nanotins INTERFACE nanotins_pcap nanotins_protocols)

if(NANOTINS_BUILD_TESTS)
    enable_testing()
    add_executable(nanotins_protocols_test tests/test_protocols.cpp)
    target_link_libraries(nanotins_protocols_test PRIVATE nanotins_protocols)
    add_test(NAME nanotins_protocols COMMAND $<TARGET_FILE:nanotins_protocols_test>)
    set_tests_properties(nanotins_protocols PROPERTIES LABELS "smoke")

    add_executable(nanotins_pcap_blocks_test tests/test_pcap_blocks.cpp)
    target_link_libraries(nanotins_pcap_blocks_test PRIVATE nanotins_pcap)
    add_test(NAME nanotins_pcap_blocks COMMAND $<TARGET_FILE:nanotins_pcap_blocks_test>)
    set_tests_properties(nanotins_pcap_blocks PROPERTIES LABELS "smoke")

    add_executable(nanotins_bulk_smoke tests/test_bulk_smoke.cpp)
    target_link_libraries(nanotins_bulk_smoke PRIVATE nanotins_protocols)
    add_test(NAME nanotins_bulk_smoke COMMAND $<TARGET_FILE:nanotins_bulk_smoke>)
    set_tests_properties(nanotins_bulk_smoke PROPERTIES LABELS "smoke")

    add_executable(nanotins_gptp_test tests/test_gptp.cpp)
    target_link_libraries(nanotins_gptp_test PRIVATE nanotins_protocols)
    add_test(NAME nanotins_gptp COMMAND $<TARGET_FILE:nanotins_gptp_test>)
    set_tests_properties(nanotins_gptp PROPERTIES LABELS "smoke")

    # wire_spec M1: the UDP header (4x u16) wire spec -> contiguous/device read -> bulk SoA -> Arrow.
    add_executable(nanotins_wire_spec_udp tests/test_wire_spec_udp.cpp)
    target_link_libraries(nanotins_wire_spec_udp PRIVATE nanotins_protocols)
    add_test(NAME nanotins_wire_spec_udp COMMAND $<TARGET_FILE:nanotins_wire_spec_udp>)
    set_tests_properties(nanotins_wire_spec_udp PROPERTIES LABELS "smoke")

    # wire_spec M2: the IPv4 header (named_bit_field + IHL-driven extent), cross-checked vs soatins bits<>.
    add_executable(nanotins_wire_spec_ipv4 tests/test_wire_spec_ipv4.cpp)
    target_link_libraries(nanotins_wire_spec_ipv4 PRIVATE nanotins_protocols)
    add_test(NAME nanotins_wire_spec_ipv4 COMMAND $<TARGET_FILE:nanotins_wire_spec_ipv4>)
    set_tests_properties(nanotins_wire_spec_ipv4 PROPERTIES LABELS "smoke")

    # wire_spec M3: IPv6 (byte-crossing bit-field + fixed-size-binary addresses + next_header chaining).
    add_executable(nanotins_wire_spec_ipv6 tests/test_wire_spec_ipv6.cpp)
    target_link_libraries(nanotins_wire_spec_ipv6 PRIVATE nanotins_protocols)
    add_test(NAME nanotins_wire_spec_ipv6 COMMAND $<TARGET_FILE:nanotins_wire_spec_ipv6>)
    set_tests_properties(nanotins_wire_spec_ipv6 PROPERTIES LABELS "smoke")

    # M4a: the backbone protocol_specs (eth/vlan/ipv4/ipv6/tcp/udp) read identically to protocols:: structs.
    add_executable(nanotins_protocol_specs tests/test_protocol_specs.cpp)
    target_link_libraries(nanotins_protocol_specs PRIVATE nanotins_protocols)
    add_test(NAME nanotins_protocol_specs COMMAND $<TARGET_FILE:nanotins_protocol_specs>)
    set_tests_properties(nanotins_protocol_specs PROPERTIES LABELS "smoke")

    # M4b: PTPv2 via embed<> spec composition (header parity vs Gptp; embed offsets + name prefixes).
    add_executable(nanotins_wire_spec_ptp tests/test_wire_spec_ptp.cpp)
    target_link_libraries(nanotins_wire_spec_ptp PRIVATE nanotins_protocols)
    add_test(NAME nanotins_wire_spec_ptp COMMAND $<TARGET_FILE:nanotins_wire_spec_ptp>)
    set_tests_properties(nanotins_wire_spec_ptp PROPERTIES LABELS "smoke")

    # M5a: the spec-driven DAG/FSM reproduces walk_packet's L2/L3/L4 traversal (determinism/parity oracle).
    add_executable(nanotins_spec_dag tests/test_spec_dag.cpp)
    target_link_libraries(nanotins_spec_dag PRIVATE nanotins_protocols)
    add_test(NAME nanotins_spec_dag COMMAND $<TARGET_FILE:nanotins_spec_dag>)
    set_tests_properties(nanotins_spec_dag PROPERTIES LABELS "smoke")

    # M5b: DAG-driven columnar decode (per-node SoA + packet_id) == struct_view; counts == walk_packet.
    add_executable(nanotins_dag_decode tests/test_dag_decode.cpp)
    target_link_libraries(nanotins_dag_decode PRIVATE nanotins_protocols)
    add_test(NAME nanotins_dag_decode COMMAND $<TARGET_FILE:nanotins_dag_decode>)
    set_tests_properties(nanotins_dag_decode PROPERTIES LABELS "smoke")

    # M5c: DAG bulk (count->prefix-sum->scatter) == sequential, serial == thread pool (GPU determinism prereq).
    add_executable(nanotins_dag_bulk tests/test_dag_bulk.cpp)
    target_link_libraries(nanotins_dag_bulk PRIVATE nanotins_protocols)
    add_test(NAME nanotins_dag_bulk COMMAND $<TARGET_FILE:nanotins_dag_bulk>)
    set_tests_properties(nanotins_dag_bulk PROPERTIES LABELS "smoke")
endif()
