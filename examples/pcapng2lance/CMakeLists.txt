# pcapng -> Lance example + the header-only `nanotins` reflection core it exercises.
# Self-contained: FetchContent pulls only the header-only boost libs that boost::describe needs
# (works under MinGW; no system boost required).

include(FetchContent)

# Single-arg FetchContent_Populate is deprecated (CMP0169) but is exactly what we want for header-only
# deps: download the sources without configuring their (CMake-4-incompatible) CMakeLists.
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

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
target_include_directories(nanotins_boost INTERFACE
    "${_nt_inc_config}"
    "${_nt_inc_assert}"
    "${_nt_inc_preprocessor}"
    "${_nt_inc_mp11}"
    "${_nt_inc_describe}")

# stdexec (header-only) for the scheduler-agnostic ex::bulk path. On a CUDA host the same call swaps to
# nvexec::stream_context. Populate-only (its CMake pulls extra deps we don't need).
FetchContent_Declare(stdexec
    GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
    GIT_TAG main
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(stdexec)
if(NOT stdexec_POPULATED)
    FetchContent_Populate(stdexec)
endif()
add_library(nanotins_stdexec INTERFACE)
target_include_directories(nanotins_stdexec INTERFACE "${stdexec_SOURCE_DIR}/include")
target_compile_features(nanotins_stdexec INTERFACE cxx_std_20)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(nanotins_stdexec INTERFACE Threads::Threads)
endif()

# The nanotins reflection core (header-only) — the part destined to become its own library.
add_library(nanotins INTERFACE)
target_include_directories(nanotins INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(nanotins INTERFACE cxx_std_20)
target_link_libraries(nanotins INTERFACE nanotins_boost nanotins_stdexec)

# The pcapng -> Lance converter: driver + in-tree CPU reference implementation of the seam.
add_executable(pcapng2lance
    src/pcapng2lance_main.cpp
    src/pcap_blocks_ref.cpp)
target_include_directories(pcapng2lance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcapng2lance PRIVATE nanotins nanolance)

if(NANOLANCE_BUILD_TESTS)
    add_executable(nanotins_reflect_smoke tests/test_reflect_smoke.cpp)
    target_link_libraries(nanotins_reflect_smoke PRIVATE nanotins)
    add_test(NAME nanotins_reflect_smoke COMMAND $<TARGET_FILE:nanotins_reflect_smoke>)
    set_tests_properties(nanotins_reflect_smoke PROPERTIES LABELS "smoke")

    add_executable(nanotins_roundtrip_test tests/test_reflect_roundtrip.cpp)
    target_link_libraries(nanotins_roundtrip_test PRIVATE nanotins nanolance)
    add_test(NAME nanotins_roundtrip COMMAND $<TARGET_FILE:nanotins_roundtrip_test>)
    set_tests_properties(nanotins_roundtrip PROPERTIES LABELS "smoke")

    add_executable(pcapng2lance_seam_test tests/test_pcap_blocks.cpp src/pcap_blocks_ref.cpp)
    target_include_directories(pcapng2lance_seam_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(pcapng2lance_seam_test PRIVATE nanotins nanolance)
    add_test(NAME pcapng2lance_seam COMMAND $<TARGET_FILE:pcapng2lance_seam_test>)
    set_tests_properties(pcapng2lance_seam PROPERTIES LABELS "smoke")

    add_executable(nanotins_bulk_smoke tests/test_bulk_smoke.cpp)
    target_link_libraries(nanotins_bulk_smoke PRIVATE nanotins)
    add_test(NAME nanotins_bulk_smoke COMMAND $<TARGET_FILE:nanotins_bulk_smoke>)
    set_tests_properties(nanotins_bulk_smoke PROPERTIES LABELS "smoke")

    add_executable(pcapng2lance_protocols_test tests/test_protocols.cpp)
    target_include_directories(pcapng2lance_protocols_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(pcapng2lance_protocols_test PRIVATE nanotins)
    add_test(NAME pcapng2lance_protocols COMMAND $<TARGET_FILE:pcapng2lance_protocols_test>)
    set_tests_properties(pcapng2lance_protocols PROPERTIES LABELS "smoke")

    add_executable(nanotins_reader_parity_test tests/test_reader_parity.cpp)
    target_link_libraries(nanotins_reader_parity_test PRIVATE nanotins nanolance)
    add_test(NAME nanotins_reader_parity COMMAND $<TARGET_FILE:nanotins_reader_parity_test>)
    set_tests_properties(nanotins_reader_parity PROPERTIES LABELS "smoke")

    add_executable(pcapng2lance_driver_test tests/test_pcapng2lance_driver.cpp src/pcap_blocks_ref.cpp)
    target_include_directories(pcapng2lance_driver_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(pcapng2lance_driver_test PRIVATE nanotins nanolance)
    add_test(NAME pcapng2lance_driver COMMAND $<TARGET_FILE:pcapng2lance_driver_test> $<TARGET_FILE:pcapng2lance>)
    set_tests_properties(pcapng2lance_driver PROPERTIES LABELS "smoke")

    # Stock-Lance (pylance) interop + on-disk offset verification. Skips (CTest 77) if pylance absent.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME pcapng2lance_interop
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_interop.py"
                    $<TARGET_FILE:pcapng2lance>)
        set_tests_properties(pcapng2lance_interop PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

        # End-to-end on real committed captures, cross-checked against an independent Python walker.
        # test_short: single section. test_framed: 12 concatenated sections (per-SHB interface reset).
        add_test(NAME pcapng2lance_realfile
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_realfile.py"
                    $<TARGET_FILE:pcapng2lance> "${CMAKE_SOURCE_DIR}/tests/test_short.pcapng")
        set_tests_properties(pcapng2lance_realfile PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

        add_test(NAME pcapng2lance_realfile_multisection
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_realfile.py"
                    $<TARGET_FILE:pcapng2lance> "${CMAKE_SOURCE_DIR}/tests/test_framed.pcapng")
        set_tests_properties(pcapng2lance_realfile_multisection PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

        # M3: L2/L3 decode -> per-PDU Lance tables, verified via stock pylance.
        add_test(NAME pcapng2lance_l2l3
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_l2l3.py"
                    $<TARGET_FILE:pcapng2lance>)
        set_tests_properties(pcapng2lance_l2l3 PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

        # Staged / incremental: l1 -> l2 -> l3 -> l4 enriching one data folder, layer by layer.
        add_test(NAME pcapng2lance_staged
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_staged.py"
                    $<TARGET_FILE:pcapng2lance>)
        set_tests_properties(pcapng2lance_staged PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

        # Windowed streaming must equal the whole-file path (tiny windows force refill/straddle/grow/
        # multi-fragment). Single-section + 12-section captures (interface state across windows).
        add_test(NAME pcapng2lance_streaming
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_streaming.py"
                    $<TARGET_FILE:pcapng2lance> "${CMAKE_SOURCE_DIR}/tests/test_short.pcapng")
        set_tests_properties(pcapng2lance_streaming PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

        add_test(NAME pcapng2lance_streaming_multisection
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_streaming.py"
                    $<TARGET_FILE:pcapng2lance> "${CMAKE_SOURCE_DIR}/tests/test_framed.pcapng")
        set_tests_properties(pcapng2lance_streaming_multisection PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

        # Chunked enrich (memory-budget -> N rows, big-tile fetch + carve) must equal single-chunk enrich.
        add_test(NAME pcapng2lance_enrich_chunking
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_enrich_chunking.py"
                    $<TARGET_FILE:pcapng2lance>)
        set_tests_properties(pcapng2lance_enrich_chunking PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

        # nlance2table (top-level tool) dumps the PDU tables to CSV/NDJSON; registered only when that
        # tool is built (NANOLANCE_BUILD_TOOLS). Smoke = text-shape self-check; tshark = field alignment
        # vs Wireshark on the same pcapng (skips 77 if tshark is absent).
        if(TARGET nlance2table)
            add_test(NAME nlance2table_smoke
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nlance2table.py"
                        $<TARGET_FILE:nlance2table> $<TARGET_FILE:pcapng2lance>)
            set_tests_properties(nlance2table_smoke PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

            add_test(NAME nlance2table_tshark
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nlance2table_tshark.py"
                        $<TARGET_FILE:nlance2table> $<TARGET_FILE:pcapng2lance>)
            set_tests_properties(nlance2table_tshark PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

            # Real fragmented capture: regression guard for the IPv4-fragmentation L4 gate (udp only on the
            # first fragment) + L2/L3 field alignment vs tshark on real-world traffic.
            add_test(NAME nlance2table_tshark_realfile
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nlance2table_tshark_realfile.py"
                        $<TARGET_FILE:nlance2table> $<TARGET_FILE:pcapng2lance>
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/SRL_front_left_51_short.pcapng")
            set_tests_properties(nlance2table_tshark_realfile PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)
        endif()
    endif()
endif()
