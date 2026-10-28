# Layer benchmarks (bench/<layer>/): one program per layer, which writes its own data, checks what it reads back, and times
# it. Included from the root CMakeLists.txt in the same scope as examples/layers/examples.cmake, so only when
# NANOLANCE_BUILD_TESTS is on. Each program also runs as a test with --quick, so the benchmarks always build and run;
# the full runs are for comparing numbers (see src/encoding/README.md and src/file/README.md, Performance).
# The layer label comes from the root CMakeLists.txt: the NL_BENCH_TARGET property names the program, whose first source
# is bench/<layer>/... .

add_executable(nl_bench_encoding bench/encoding/decode_bench.cpp)
add_executable(nl_bench_file bench/file/file_bench.cpp)
foreach(_nl_bench IN ITEMS nl_bench_encoding nl_bench_file)
    target_compile_features(${_nl_bench} PRIVATE cxx_std_20)
    target_link_libraries(${_nl_bench} PRIVATE nanolance)
    # The JSON header records the commit (from this source directory) and the build type.
    target_compile_definitions(${_nl_bench} PRIVATE
        "NL_BENCH_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
        "NL_BENCH_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"")
endforeach()

foreach(_nl_bench_layer IN ITEMS encoding file)
    add_test(NAME nano_lance_bench_${_nl_bench_layer}_quick
             COMMAND $<TARGET_FILE:nl_bench_${_nl_bench_layer}> --quick)
    set_tests_properties(nano_lance_bench_${_nl_bench_layer}_quick PROPERTIES
                         LABELS "bench;bench:${_nl_bench_layer}"
                         NL_BENCH_TARGET nl_bench_${_nl_bench_layer})
endforeach()
