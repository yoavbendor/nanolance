# pcapng -> Lance example. The converter (src/nanom_convert.cpp) and the DPAR/LLDP tools (src/dpar2lance_nanom_main.cpp)
# are built on nanom, the parser-combinator library: a git submodule under extern/nanom, pinned, and added here.
# nanolance's writer does the Lance output. Every table the converter and tools write is checked against the nanotins
# reference the examples were ported from (tests/golden/parity_digests.json).
#
# CPU-only. --gpu is a planned future feature; the converter reports that and exits.

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/nanom/CMakeLists.txt")
    message(FATAL_ERROR "examples/pcapng2lance/extern/nanom is empty -- run: git submodule update --init --recursive")
endif()
set(PCAPNG2LANCE_NANOM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/nanom")
if(NOT TARGET nanom::nanom)
    # nanom is header-only and built only here; its own tests and examples stay out of this build.
    add_subdirectory("${PCAPNG2LANCE_NANOM_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/nanom" EXCLUDE_FROM_ALL)
endif()

# The converter, C++23 (nanom requires it).
add_library(pcapng2lance_nanom_convert STATIC src/nanom_convert.cpp)
target_compile_features(pcapng2lance_nanom_convert PRIVATE cxx_std_23)
target_include_directories(pcapng2lance_nanom_convert PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include" "${PCAPNG2LANCE_NANOM_DIR}/examples/nanotins_parity")
target_link_libraries(pcapng2lance_nanom_convert PRIVATE nanom::nanom nanolance)

add_executable(pcapng2lance src/pcapng2lance.cpp)
target_include_directories(pcapng2lance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcapng2lance PRIVATE pcapng2lance_nanom_convert nanolance)

# dpar2lance and lldp2lance: the DPAR engine of nanom's parity example (include/nanom_dpar.hpp). lldp2lance is the
# same source with NANOM_LLDP_ONLY, which accepts the lldp parser only.
add_executable(dpar2lance src/dpar2lance_nanom_main.cpp)
add_executable(lldp2lance src/dpar2lance_nanom_main.cpp)
target_compile_definitions(lldp2lance PRIVATE NANOM_LLDP_ONLY)
foreach(_nm_dpar_tool dpar2lance lldp2lance)
    target_compile_features(${_nm_dpar_tool} PRIVATE cxx_std_23)
    target_include_directories(${_nm_dpar_tool} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${PCAPNG2LANCE_NANOM_DIR}/examples/nanotins_parity")
    target_link_libraries(${_nm_dpar_tool} PRIVATE nanom::nanom nanolance)
endforeach()

if(NANOLANCE_BUILD_TESTS)
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

        # Parity with the nanotins reference: every stage, one-shot and DPAR/LLDP table digests (golden/).
        if(TARGET dpar2lance AND TARGET lldp2lance)
            add_test(NAME pcapng2lance_parity_golden
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_parity_golden.py"
                        $<TARGET_FILE:pcapng2lance> $<TARGET_FILE:dpar2lance> $<TARGET_FILE:lldp2lance>
                        "${CMAKE_SOURCE_DIR}" "${PCAPNG2LANCE_NANOM_DIR}/examples/nanotins_parity/testdata"
                        "${CMAKE_CURRENT_BINARY_DIR}/parity_golden")
            set_tests_properties(pcapng2lance_parity_golden PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)
        endif()

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

            # Real gPTP capture: the spec_dag PTP common-header parser (GptpNode -> PtpHeaderSpec) must
            # match tshark's PTPv2 dissection field-for-field, across every message type present.
            add_test(NAME nlance2table_tshark_ptp
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nlance2table_tshark_ptp.py"
                        $<TARGET_FILE:nlance2table> $<TARGET_FILE:pcapng2lance>
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/some_ptp_data.pcapng")
            set_tests_properties(nlance2table_tshark_ptp PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

            # IPv6 extension headers + SRv6: our ext-header chain walk + SRH segment list + IPv6 options
            # (ipv6_routing / ipv6_srh_segment / ipv6_option tables) and the L4 fix, field-checked vs tshark
            # on a scapy-built capture (srv6_sample.pcap; regenerate with tests/gen_srv6_fixture.py).
            add_test(NAME nlance2table_tshark_srv6
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nlance2table_tshark_srv6.py"
                        $<TARGET_FILE:nlance2table> $<TARGET_FILE:pcapng2lance>
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/srv6_sample.pcap")
            set_tests_properties(nlance2table_tshark_srv6 PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

            # IPv4 options: the tlv_cursor ipv4_options walk over [IPv4 base + 20, IHL*4) -> the ipv4_option
            # child table, field-checked vs tshark's ip.opt.type on a scapy-built capture
            # (ipv4_options_sample.pcap; regenerate with tests/gen_ipv4_options_fixture.py).
            add_test(NAME nlance2table_tshark_ipv4_options
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nlance2table_tshark_ipv4_options.py"
                        $<TARGET_FILE:nlance2table> $<TARGET_FILE:pcapng2lance>
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/ipv4_options_sample.pcap")
            set_tests_properties(nlance2table_tshark_ipv4_options PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

            # Download-on-demand real-capture golden: fetch real IPv6/ext-header captures (GPL-licensed
            # upstreams — NOT committed; cached under the build tree) and field-check vs tshark. Skips (77)
            # if tshark is missing or the downloads fail (offline).
            add_test(NAME nlance2table_tshark_download
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nlance2table_tshark_download.py"
                        $<TARGET_FILE:nlance2table> $<TARGET_FILE:pcapng2lance>
                        "${CMAKE_CURRENT_BINARY_DIR}/ws_capture_cache")
            set_tests_properties(nlance2table_tshark_download PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

            # Harmonization: one-shot (--decode-l2l3) and staged (--stage l1->l4) must emit identical PDU
            # tables on a fragmented capture (same IPv4-fragmentation L4 gate in both decode paths).
            add_test(NAME pcapng2lance_frag_harmony
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_frag_harmony.py"
                        $<TARGET_FILE:pcapng2lance> $<TARGET_FILE:nlance2table>
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/SRL_front_left_51_short.pcapng")
            set_tests_properties(pcapng2lance_frag_harmony PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

            # Dataset-stitch equivalence: cut the capture into 3 contiguous packet slices (--drop/--count)
            # as free-standing datasets, stitch them, and verify the result is bit-exact to the single
            # dataset built from the whole range (read back + dumped via nlance2table). Needs nlance_stitch.
            if(TARGET nlance_stitch)
                set(_stitch_fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/SRL_front_left_51_short.pcapng")
                # core/order/uneven/single/decode-l2l3 equivalence: stitch(slices) == full, bit-exact.
                add_test(NAME pcapng2lance_stitch_equiv
                    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_stitch_equiv.py"
                            $<TARGET_FILE:pcapng2lance> $<TARGET_FILE:nlance_stitch> $<TARGET_FILE:nlance2table>
                            "${_stitch_fixture}")
                set_tests_properties(pcapng2lance_stitch_equiv PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

                # External-blob fetch through the stitched dataset (refs still resolve, byte-for-byte).
                add_test(NAME pcapng2lance_stitch_fetch
                    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_stitch_fetch.py"
                            $<TARGET_FILE:pcapng2lance> $<TARGET_FILE:nlance_stitch> $<TARGET_FILE:nlance2table>
                            "${_stitch_fixture}")
                set_tests_properties(pcapng2lance_stitch_fetch PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)

                # Stock-Lance (pylance) reads the stitched dataset identically to the full one (skips if absent).
                add_test(NAME pcapng2lance_stitch_pylance
                    COMMAND "${NANO_LANCE_WRITER_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_stitch_pylance.py"
                            $<TARGET_FILE:pcapng2lance> $<TARGET_FILE:nlance_stitch> "${_stitch_fixture}")
                set_tests_properties(pcapng2lance_stitch_pylance PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)
            endif()
        endif()
    endif()
endif()
