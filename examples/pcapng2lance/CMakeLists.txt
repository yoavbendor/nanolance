# pcapng -> Lance example: a thin consumer of the standalone parsing stack. nanolance itself does NOT depend
# on that stack (it reads/writes Lance with nanoarrow + local code only); the parsing stack is THIS example's
# dependency, so the example owns it — vendored as a git submodule under extern/nanotins and added here. The
# be<>/bits<>/SoA/arrow reflection lives in soatins; pcap/pcapng + L2/L3/L4 parsing in nanotins. This
# directory owns the Lance-writing glue — the CLI driver, streaming windows, the per-PDU Lance table writer,
# and the staged enrich pipeline.
#
# CPU-only. (A GPU bulk path via the gputins/CUDA layer is a planned future feature; `--gpu` currently
# reports that and exits.)

# Pull in the sister nanotins stack (soatins::core + nanotins). This is the only place the submodule is
# referenced — a plain nanolance build never needs it.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/nanotins/CMakeLists.txt")
    message(FATAL_ERROR
        "examples/pcapng2lance/extern/nanotins is empty — run: git submodule update --init --recursive")
endif()
# Build the nanotins/soatins unit tests alongside this example's tests when tests are on.
if(NANOLANCE_BUILD_TESTS)
    set(SOATINS_BUILD_TESTS ON CACHE BOOL "" FORCE)
    set(NANOTINS_BUILD_TESTS ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(extern/nanotins)

# The pcapng -> Lance converter.
add_executable(pcapng2lance src/pcapng2lance_main.cpp)
target_sources(pcapng2lance PRIVATE src/phase_b_runner.cpp)
target_include_directories(pcapng2lance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcapng2lance PRIVATE nanotins nanolance)

# dpar2lance -> apply DPAR rules to a capture and write each matched rule table to its own Lance dataset
# (the columnar counterpart to nanotins' NDJSON `dpar` example). Reuses this example's vendored nanotins
# submodule + the nanolance writer.
add_executable(dpar2lance src/dpar2lance_main.cpp)
target_include_directories(dpar2lance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dpar2lance PRIVATE nanotins nanolance)

if(NANOLANCE_BUILD_TESTS)
    # struct -> soa -> arrow -> Lance -> read-back (exercises nanotins reflection through the nanolance
    # writer/reader; stays here because it depends on nanolance, not just nanotins).
    add_executable(nanotins_roundtrip_test tests/test_reflect_roundtrip.cpp)
    target_link_libraries(nanotins_roundtrip_test PRIVATE nanotins nanolance)
    add_test(NAME nanotins_roundtrip COMMAND $<TARGET_FILE:nanotins_roundtrip_test>)
    set_tests_properties(nanotins_roundtrip PROPERTIES LABELS "smoke")

    # nanolance reads back every fixed-width type nanotins writes, even with a blob column present.
    add_executable(nanotins_reader_parity_test tests/test_reader_parity.cpp)
    target_link_libraries(nanotins_reader_parity_test PRIVATE nanotins nanolance)
    add_test(NAME nanotins_reader_parity COMMAND $<TARGET_FILE:nanotins_reader_parity_test>)
    set_tests_properties(nanotins_reader_parity PROPERTIES LABELS "smoke")

    # DPAR rule table -> Arrow -> Lance write -> read-back roundtrip (the dpar2lance sink end to end).
    add_executable(dpar2lance_roundtrip_test tests/test_dpar2lance_roundtrip.cpp)
    target_include_directories(dpar2lance_roundtrip_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(dpar2lance_roundtrip_test PRIVATE nanotins nanolance)
    add_test(NAME dpar2lance_roundtrip COMMAND $<TARGET_FILE:dpar2lance_roundtrip_test>)
    set_tests_properties(dpar2lance_roundtrip PROPERTIES LABELS "smoke")

    # M6: the DAG-driven PDU tables are byte-identical (schema + Arrow buffers) to the protocols:: tables,
    # so the driver's decode path can move to the spec/DAG without changing the Lance output.
    add_executable(pdu_table_interop_test tests/test_pdu_table_interop.cpp)
    target_include_directories(pdu_table_interop_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(pdu_table_interop_test PRIVATE nanotins nanolance)
    add_test(NAME pdu_table_interop COMMAND $<TARGET_FILE:pdu_table_interop_test>)
    set_tests_properties(pdu_table_interop PROPERTIES LABELS "smoke")

    # M6 (full pipeline, no Python): write both PDU-table paths to real Lance, dump via nlance2table, diff
    # the CSV. Proves the spec/DAG tables == protocols:: tables through Lance write/read, not just in-memory.
    if(TARGET nlance2table)
        add_executable(pdu_table_lance_interop_test tests/test_pdu_table_lance_interop.cpp)
        target_include_directories(pdu_table_lance_interop_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(pdu_table_lance_interop_test PRIVATE nanotins nanolance)
        add_test(NAME pdu_table_lance_interop
                 COMMAND $<TARGET_FILE:pdu_table_lance_interop_test> $<TARGET_FILE:nlance2table>)
        set_tests_properties(pdu_table_lance_interop PROPERTIES LABELS "smoke")
    endif()

    # End-to-end driver self-check (manifest + KV metadata + external-blob premise). Uses the nanotins
    # pcap scanner (linked via the umbrella target) and the shared pcap_fixtures.hpp from the vendored
    # nanotins submodule's tests dir.
    add_executable(pcapng2lance_driver_test tests/test_pcapng2lance_driver.cpp)
    target_include_directories(pcapng2lance_driver_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/extern/nanotins/nanotins/tests")
    target_link_libraries(pcapng2lance_driver_test PRIVATE nanotins nanolance)
    add_test(NAME pcapng2lance_driver COMMAND $<TARGET_FILE:pcapng2lance_driver_test> $<TARGET_FILE:pcapng2lance>)
    set_tests_properties(pcapng2lance_driver PROPERTIES LABELS "smoke")

    # (The GPU equivalence tests — wire_spec_gpu_smoke, dag_gpu_equiv_bench — moved out with the gputins
    # layer; serial==bulk determinism is covered by the nanotins suite's test_dag_bulk.)

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
                add_test(NAME pcapng2lance_stitch_equiv
                    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_stitch_equiv.py"
                            $<TARGET_FILE:pcapng2lance> $<TARGET_FILE:nlance_stitch> $<TARGET_FILE:nlance2table>
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/SRL_front_left_51_short.pcapng")
                set_tests_properties(pcapng2lance_stitch_equiv PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)
            endif()
        endif()
    endif()
endif()
