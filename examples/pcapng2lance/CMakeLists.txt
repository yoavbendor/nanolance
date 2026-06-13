# pcapng -> Lance example: a thin consumer of the standalone parsing stack (top-level ../../soatins,
# ../../nanotins, ../../gputins, added by the root CMakeLists before this dir). The be<>/bits<>/SoA/arrow
# reflection lives in soatins; pcap/pcapng + L2/L3/L4 parsing in nanotins; the CUDA executors in gputins.
# This directory owns only the Lance-writing glue — the CLI driver, streaming windows, the per-PDU Lance
# table writer, the staged enrich pipeline, and the (optional) CUDA build wiring for the GPU bridge TU.

# The pcapng -> Lance converter.
add_executable(pcapng2lance src/pcapng2lance_main.cpp)
target_sources(pcapng2lance PRIVATE src/phase_b_runner.cpp)
target_sources(pcapng2lance PRIVATE src/pcapng2lance_gpu_bridge.cpp)
target_include_directories(pcapng2lance PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(pcapng2lance PRIVATE nanotins gputins nanolance)

# --- optional GPU (nvexec) bulk path -------------------------------------------------------------
# OFF by default; the CPU build needs none of this (all GPU code is behind #ifdef NANOTINS_ENABLE_CUDA).
# When ON, the driver TU must be compiled as CUDA so the nvexec device scheduler + device lambdas work.
# The exact toolchain wiring (clang -x cuda vs nvcc, the nvexec target name, the CUDA arch) is host
# specific — see nanotins/docs/GPU_BULK_INTEGRATION.md. This block is the documented starting point.
option(NANOTINS_ENABLE_CUDA "Build pcapng2lance with the GPU (nvexec) bulk path" OFF)
if(NANOTINS_ENABLE_CUDA)
    find_package(CUDAToolkit REQUIRED)

    set(_NT_GPU_ARCH sm_89 CACHE STRING "CUDA arch for pcapng2lance (L40S = sm_89)")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_NT_CUDA_OPTS -x cuda --cuda-gpu-arch=${_NT_GPU_ARCH} -Wno-unknown-cuda-version
                          -U_GLIBCXX_ASSERTIONS -D__STDC_HOSTED__=1)
        if(DEFINED CUDAToolkit_ROOT)
            list(APPEND _NT_CUDA_OPTS --cuda-path=${CUDAToolkit_ROOT})
        elseif(DEFINED CUDAToolkit_LIBRARY_DIR)
            get_filename_component(_NT_CUDA_ROOT "${CUDAToolkit_LIBRARY_DIR}" DIRECTORY)
            list(APPEND _NT_CUDA_OPTS --cuda-path=${_NT_CUDA_ROOT})
        endif()
        # clang 20 + stdexec/nvexec currently crashes on this very large TU under -O2 in our CUDA build.
        set_source_files_properties(src/pcapng2lance_main.cpp PROPERTIES COMPILE_OPTIONS "-O0;-g0")
        set_source_files_properties(src/pcapng2lance_gpu_bridge.cpp PROPERTIES
            COMPILE_DEFINITIONS "NANOTINS_ENABLE_CUDA=1"
            COMPILE_OPTIONS "${_NT_CUDA_OPTS}")
    else()
        enable_language(CUDA)
        set_property(SOURCE src/pcapng2lance_gpu_bridge.cpp PROPERTY LANGUAGE CUDA)
        set_target_properties(pcapng2lance PROPERTIES CUDA_ARCHITECTURES 89)
        target_compile_definitions(pcapng2lance PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:NANOTINS_ENABLE_CUDA=1>)
        target_compile_options(pcapng2lance PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:-std=c++20 --expt-relaxed-constexpr --extended-lambda>)
    endif()

    target_include_directories(pcapng2lance PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
    if(DEFINED CUDAToolkit_LIBRARY_DIR)
        target_link_directories(pcapng2lance PRIVATE ${CUDAToolkit_LIBRARY_DIR})
    endif()
    target_link_libraries(pcapng2lance PRIVATE CUDA::cudart)
    message(STATUS "pcapng2lance: NANOTINS_ENABLE_CUDA=ON with clang-cuda (${_NT_GPU_ARCH})")
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
    # pcap scanner (linked via the umbrella target) and the shared pcap_fixtures.hpp from nanotins/tests.
    add_executable(pcapng2lance_driver_test tests/test_pcapng2lance_driver.cpp)
    target_include_directories(pcapng2lance_driver_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/nanotins/tests")
    target_link_libraries(pcapng2lance_driver_test PRIVATE nanotins nanolance)
    add_test(NAME pcapng2lance_driver COMMAND $<TARGET_FILE:pcapng2lance_driver_test> $<TARGET_FILE:pcapng2lance>)
    set_tests_properties(pcapng2lance_driver PROPERTIES LABELS "smoke")

    # struct_spec GPU smoke (T5): GPU scatter_spec == CPU spec_soa, byte-identical. Inert on a CPU build
    # (the gputins header compiles to nothing without NANOTINS_ENABLE_CUDA), so it passes trivially here;
    # the CUDA host compiles this TU as CUDA (the same -x cuda step as the gpu bridge) and runs the real
    # GPU==CPU check. See nanotins/docs/GPU_BULK_INTEGRATION.md.
    add_executable(struct_spec_gpu_smoke tests/test_struct_spec_gpu.cpp)
    target_link_libraries(struct_spec_gpu_smoke PRIVATE nanotins gputins)
    add_test(NAME struct_spec_gpu_smoke COMMAND $<TARGET_FILE:struct_spec_gpu_smoke>)
    set_tests_properties(struct_spec_gpu_smoke PROPERTIES LABELS "smoke")
    if(NANOTINS_ENABLE_CUDA AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # struct_spec_gpu_smoke is a single-source target, so compile it as CUDA via TARGET-level options
        # (reliable — source-level COMPILE_OPTIONS via set_source_files_properties did not propagate to the
        # build rule here). _NT_CUDA_OPTS (-x cuda, --cuda-gpu-arch, ...) is set in the GPU block above.
        target_compile_definitions(struct_spec_gpu_smoke PRIVATE NANOTINS_ENABLE_CUDA=1)
        target_compile_options(struct_spec_gpu_smoke PRIVATE ${_NT_CUDA_OPTS})
        target_include_directories(struct_spec_gpu_smoke PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
        if(DEFINED CUDAToolkit_LIBRARY_DIR)
            target_link_directories(struct_spec_gpu_smoke PRIVATE ${CUDAToolkit_LIBRARY_DIR})
        endif()
        target_link_libraries(struct_spec_gpu_smoke PRIVATE CUDA::cudart)
    endif()

    # DAG decode equivalence + benchmark: serial == bulk == gpu, bit-exact, with per-mode timing. The CTest
    # runs a small N (serial==bulk, plus ==gpu on a CUDA build); run the binary with --packets 32000000
    # --bench on the GPU host for the at-scale check + gpu~=cpu timings.
    add_executable(dag_gpu_equiv_bench tests/dag_gpu_equiv_bench.cpp)
    target_link_libraries(dag_gpu_equiv_bench PRIVATE nanotins gputins)
    add_test(NAME dag_gpu_equiv COMMAND $<TARGET_FILE:dag_gpu_equiv_bench> --packets 50000 --bench)
    set_tests_properties(dag_gpu_equiv PROPERTIES LABELS "smoke")
    if(NANOTINS_ENABLE_CUDA AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # Single-source target -> compile as CUDA via TARGET-level options (same as struct_spec_gpu_smoke).
        target_compile_definitions(dag_gpu_equiv_bench PRIVATE NANOTINS_ENABLE_CUDA=1)
        target_compile_options(dag_gpu_equiv_bench PRIVATE ${_NT_CUDA_OPTS})
        target_include_directories(dag_gpu_equiv_bench PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
        if(DEFINED CUDAToolkit_LIBRARY_DIR)
            target_link_directories(dag_gpu_equiv_bench PRIVATE ${CUDAToolkit_LIBRARY_DIR})
        endif()
        target_link_libraries(dag_gpu_equiv_bench PRIVATE CUDA::cudart)
    endif()

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
