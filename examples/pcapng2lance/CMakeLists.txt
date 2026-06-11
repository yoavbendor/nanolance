# pcapng -> Lance example: a thin consumer of the standalone `nanotins` library (top-level ../../nanotins,
# added by the root CMakeLists before this dir). All pcap/pcapng + L2/L3/L4 parsing and the
# be<>/bits<>/SoA/arrow reflection live in nanotins; this directory owns only the Lance-writing glue —
# the CLI driver, streaming windows, the per-PDU Lance table writer, and the staged enrich pipeline.

# The pcapng -> Lance converter.
add_executable(pcapng2lance src/pcapng2lance_main.cpp)
target_include_directories(pcapng2lance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcapng2lance PRIVATE nanotins nanolance)

# --- optional GPU (nvexec) bulk path -------------------------------------------------------------
# OFF by default; the CPU build needs none of this (all GPU code is behind #ifdef NANOTINS_ENABLE_CUDA).
# When ON, the driver TU must be compiled as CUDA so the nvexec device scheduler + device lambdas work.
# The exact toolchain wiring (clang -x cuda vs nvcc, the nvexec target name, the CUDA arch) is host
# specific — see nanotins/docs/GPU_BULK_INTEGRATION.md. This block is the documented starting point.
option(NANOTINS_ENABLE_CUDA "Build pcapng2lance with the GPU (nvexec) bulk path" OFF)
if(NANOTINS_ENABLE_CUDA)
    target_compile_definitions(pcapng2lance PRIVATE NANOTINS_ENABLE_CUDA=1)
    # Compile the driver TU as CUDA. With the LLVM/clang-cuda toolchain the experiment used, this is
    # `clang++ -x cuda --cuda-gpu-arch=sm_XX`; with NVIDIA's toolchain it is nvcc. Pick ONE:
    #   (clang-cuda) set via CMAKE_CXX_COMPILER=clang++ + the flags below, OR
    #   (nvcc)       enable_language(CUDA) and set_source_files_properties(... LANGUAGE CUDA).
    # nvexec headers live under the stdexec include dir (already on nanotins_core); link cudart.
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        target_link_libraries(pcapng2lance PRIVATE CUDA::cudart)
    endif()
    # set(_NT_GPU_ARCH sm_80 CACHE STRING "CUDA arch for the weaker model's GPU")
    # target_compile_options(pcapng2lance PRIVATE -x cuda --cuda-gpu-arch=${_NT_GPU_ARCH}
    #                        --expt-relaxed-constexpr)
    message(STATUS "pcapng2lance: NANOTINS_ENABLE_CUDA=ON — see nanotins/docs/GPU_BULK_INTEGRATION.md to finish toolchain wiring")
endif()

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

    # End-to-end driver self-check (manifest + KV metadata + external-blob premise). Uses the nanotins
    # pcap scanner (linked via the umbrella target) and the shared pcap_fixtures.hpp from nanotins/tests.
    add_executable(pcapng2lance_driver_test tests/test_pcapng2lance_driver.cpp)
    target_include_directories(pcapng2lance_driver_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/nanotins/tests")
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

            # Harmonization: one-shot (--decode-l2l3) and staged (--stage l1->l4) must emit identical PDU
            # tables on a fragmented capture (same IPv4-fragmentation L4 gate in both decode paths).
            add_test(NAME pcapng2lance_frag_harmony
                COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_frag_harmony.py"
                        $<TARGET_FILE:pcapng2lance> $<TARGET_FILE:nlance2table>
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/SRL_front_left_51_short.pcapng")
            set_tests_properties(pcapng2lance_frag_harmony PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)
        endif()
    endif()
endif()
