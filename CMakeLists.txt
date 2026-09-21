cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(NANOLANCE_STANDALONE ON)
else()
    set(NANOLANCE_STANDALONE OFF)
endif()

# Bump here when releasing; also keep in sync with project() below when standalone.
set(NANOLANCE_VERSION_MAJOR 0)
set(NANOLANCE_VERSION_MINOR 2)
set(NANOLANCE_VERSION_PATCH 0)
set(NANOLANCE_VERSION_STRING
    "${NANOLANCE_VERSION_MAJOR}.${NANOLANCE_VERSION_MINOR}.${NANOLANCE_VERSION_PATCH}")

if(NANOLANCE_STANDALONE)
    # C is enabled so tests/test_readme_quickstart.c can compile the documented C snippet as real
    # C -- nano_lance_writer.h is an extern "C" header whose only other consumers are C++, so
    # nothing else in the tree would notice it drifting into C++-only syntax.
    project(nanolance VERSION ${NANOLANCE_VERSION_MAJOR}.${NANOLANCE_VERSION_MINOR}.${NANOLANCE_VERSION_PATCH} LANGUAGES C CXX)
    enable_testing()

    # Developer ergonomics for a standalone build (skipped when embedded):
    #   * compile_commands.json so clangd / IDEs index our sources AND the FetchContent'd deps
    #     (nanoarrow/zstd/CLI11/nanos3reader) we compile against. (Already gitignored.)
    #   * park FetchContent's downloads in a stable, IDE-indexable ./.deps-<generator> that survives
    #     `rm -rf build` (gitignored). Honour a caller-provided FETCHCONTENT_BASE_DIR (e.g. a CI
    #     cache) if already set.
    #
    # The directory is keyed by generator on purpose. FetchContent's sub-build trees bake in the
    # generator that created them, so a single shared ./.deps is poisoned the moment two generators
    # are used against the same source tree -- which is the DEFAULT path for a new user:
    # `pip install -e bindings/python` runs scikit-build-core (Ninja), and the documented
    # `cmake -S . -B build` then uses Makefiles and dies with "Does not match the generator used
    # previously: Ninja". Keying by generator lets both coexist.
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    if(NOT DEFINED FETCHCONTENT_BASE_DIR)
        string(REGEX REPLACE "[^A-Za-z0-9_.-]+" "_" _nanolance_generator_slug "${CMAKE_GENERATOR}")
        set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.deps-${_nanolance_generator_slug}"
            CACHE PATH "Fetched dependency sources")
        unset(_nanolance_generator_slug)
    endif()
endif()

option(ENABLE_NANO_LANCE_WRITER "Build nanolance (legacy cache name; OFF skips this tree)" ON)
if(DEFINED ENABLE_NANO_LANCE_WRITER AND NOT ENABLE_NANO_LANCE_WRITER)
    message(STATUS "nanolance: disabled (ENABLE_NANO_LANCE_WRITER=OFF)")
    return()
endif()

if(DEFINED ENABLE_DUAL_NANOBRIDGE AND NOT ENABLE_DUAL_NANOBRIDGE)
    message(STATUS "nanolance: skipped (ENABLE_DUAL_NANOBRIDGE=OFF)")
    return()
endif()

if(NANOLANCE_STANDALONE)
    option(NANOLANCE_BUILD_TESTS "Build nanolance tests and register CTest" ON)
else()
    option(NANOLANCE_BUILD_TESTS "Build nanolance tests and register CTest" OFF)
endif()
option(NANOLANCE_BUILD_TOOLS "Build arrowipc2lance CLI" ON)
option(NANOLANCE_ENABLE_S3 "S3 external blob support (needs NANOLANCE_S3_TARGET + NANOLANCE_S3_INCLUDE_DIR)" OFF)
# Raises the minimum supported CPU to Haswell-class (AVX2 + BMI2 + FMA, ubiquitous on x86_64 hardware
# since ~2013, including all current GitHub-hosted Actions runners). OFF by default: this is a real
# portability tradeoff for anyone embedding nanolance, not a free win, so it must be opted into
# explicitly rather than silently raised. Measured to meaningfully speed up the FastLanes bitpack
# pack_1024/unpack_1024 kernels (and whatever else the compiler can auto-vectorize wider) when the
# *whole* translation unit is compiled at this target -- unlike a scoped per-function target attribute,
# which was measured to give no benefit here since it blocks inlining across the attribute boundary.
option(NANOLANCE_ENABLE_AVX2 "Compile with -march=haswell (AVX2+BMI2+FMA); raises minimum CPU requirement" OFF)

set(NANOLANCE_S3_TARGET "" CACHE STRING "Target for S3 reads (e.g. stream_helper_s3)")
set(NANOLANCE_S3_INCLUDE_DIR "" CACHE PATH "Dir containing s3_aws_sdk_stream.h")

# ── Memory-safety / hardening build knobs (opt-in; used by the safety CI jobs) ────────────────────
# The reader parses untrusted on-disk bytes, so these let CI (and users) run the read path under
# sanitizers and fuzz it. They are OFF/empty by default and never affect a normal build.
set(NANOLANCE_SANITIZER "" CACHE STRING "Sanitizers to enable for all targets, e.g. 'address,undefined' or 'thread'")
option(NANOLANCE_WERROR "Treat warnings as errors on nanolance's own targets" OFF)
option(NANOLANCE_BUILD_FUZZERS "Build libFuzzer harnesses over the untrusted decode path (needs Clang)" OFF)

if(NANOLANCE_SANITIZER)
    message(STATUS "nanolance: sanitizers enabled: ${NANOLANCE_SANITIZER}")
    add_compile_options(-fsanitize=${NANOLANCE_SANITIZER} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${NANOLANCE_SANITIZER})
endif()

if(NANOLANCE_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "NANOLANCE_BUILD_FUZZERS requires a Clang toolchain (needs -fsanitize=fuzzer)")
    endif()
    message(STATUS "nanolance: building libFuzzer harnesses (coverage + address,undefined)")
    # Instrument the whole library for coverage feedback + ASan/UBSan; the harness executables add the
    # libFuzzer driver itself via -fsanitize=fuzzer at link.
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

if(NANOLANCE_WERROR)
    set(NANOLANCE_WARNING_FLAGS -Wall -Wextra -Werror)
else()
    set(NANOLANCE_WARNING_FLAGS)
endif()

include(FetchContent)

# --- nanoarrow ---
if(NOT TARGET nanoarrow_static OR NOT DEFINED nanoarrow_SOURCE_DIR)
    if(NOT NANOLANCE_STANDALONE)
        message(STATUS "nanolance: skipped (nanoarrow IPC targets missing)")
        return()
    endif()
    set(NANOARROW_IPC ON CACHE BOOL "" FORCE)
    set(NANOARROW_IPC_WITH_ZSTD OFF CACHE BOOL "" FORCE)
    set(NANOARROW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NANOARROW_BUILD_APPS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nanoarrow
        GIT_REPOSITORY https://github.com/apache/arrow-nanoarrow.git
        GIT_TAG ffe61d3cd2d02da9e60cfc405cd6c50c512f64b9
        # Pinned to a commit hash: a shallow clone can't check out an arbitrary SHA on many git servers
        # (corporate mirrors, GitHub Enterprise), which fails standalone configure with "reference is not a
        # tree". A full clone can, so shallow must stay off here.
        GIT_SHALLOW FALSE
    )
    FetchContent_Populate(nanoarrow)
    # Build into THIS build tree, not ${nanoarrow_BINARY_DIR} -- that lives under the shared
    # FETCHCONTENT_BASE_DIR, so two build trees would compile nanoarrow into the same object
    # directory. Keying that directory by generator is not enough: a sanitizer/fuzzer build
    # (-DNANOLANCE_BUILD_FUZZERS=ON adds -fsanitize=... globally) and a plain build share a generator
    # but must not share objects. Left shared, the fuzzer build's instrumented nanoarrow leaked into
    # the plain build, which then failed to link on __sanitizer_cov_* / __asan_* symbols. Sharing the
    # SOURCE checkout is the point of .deps and stays; sharing compiled output never was.
    # (_nanolance_zstd below already builds into CMAKE_CURRENT_BINARY_DIR for the same reason.)
    add_subdirectory(${nanoarrow_SOURCE_DIR} "${CMAKE_CURRENT_BINARY_DIR}/_nanolance_nanoarrow_build"
                     EXCLUDE_FROM_ALL)
    set(nanoarrow_SOURCE_DIR "${nanoarrow_SOURCE_DIR}" CACHE PATH "Nanoarrow source for nanolance" FORCE)
endif()

# --- zstd ---
set(NANOLANCE_ZSTD_TARGET "")
if(TARGET libzstd_static)
    set(NANOLANCE_ZSTD_TARGET libzstd_static)
elseif(TARGET zstd::libzstd_static)
    set(NANOLANCE_ZSTD_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd)
    set(NANOLANCE_ZSTD_TARGET zstd::libzstd)
else()
    find_package(zstd QUIET)
    if(TARGET zstd::libzstd_static)
        set(NANOLANCE_ZSTD_TARGET zstd::libzstd_static)
    elseif(TARGET zstd::libzstd)
        set(NANOLANCE_ZSTD_TARGET zstd::libzstd)
    endif()
endif()

if(NOT NANOLANCE_ZSTD_TARGET AND NANOLANCE_STANDALONE)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(_nanolance_zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.6
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(_nanolance_zstd)
    if(NOT _nanolance_zstd_POPULATED)
        FetchContent_Populate(_nanolance_zstd)
        add_subdirectory(
            "${_nanolance_zstd_SOURCE_DIR}/build/cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/_nanolance_zstd_build"
            EXCLUDE_FROM_ALL
        )
    endif()
    if(TARGET libzstd_static)
        set(NANOLANCE_ZSTD_TARGET libzstd_static)
    endif()
endif()

if(NOT NANOLANCE_ZSTD_TARGET)
    message(STATUS "nanolance: skipped (zstd target missing)")
    return()
endif()

# --- CLI11 (tools only) ---
if(NANOLANCE_BUILD_TOOLS)
    if(NOT TARGET CLI11::CLI11)
        if(NOT NANOLANCE_STANDALONE)
            message(FATAL_ERROR "nanolance: CLI11::CLI11 missing; set NANOLANCE_BUILD_TOOLS=OFF or add CLI11 to parent")
        endif()
        FetchContent_Declare(_nanolance_cli11
            GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
            GIT_TAG v2.4.2
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(_nanolance_cli11)
    endif()
endif()

set(_NANOLANCE_GEN_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${_NANOLANCE_GEN_INCLUDE}/nanolance")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
    "${_NANOLANCE_GEN_INCLUDE}/nanolance/version.h"
    @ONLY
)

if(NOT NANO_LANCE_WRITER_PYTHON)
    set(_nl_uv "${CMAKE_SOURCE_DIR}/uv_for_farmhosts/.venv/bin/python")
    if(EXISTS "${_nl_uv}")
        set(NANO_LANCE_WRITER_PYTHON "${_nl_uv}" CACHE FILEPATH "Python for nanolance fixture checks" FORCE)
    endif()
endif()
if(NOT NANO_LANCE_WRITER_PYTHON)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(NANO_LANCE_WRITER_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH "Python for nanolance fixture checks" FORCE)
endif()
set(NANOLANCE_PYTHON "${NANO_LANCE_WRITER_PYTHON}" CACHE FILEPATH "Alias of NANO_LANCE_WRITER_PYTHON" FORCE)

set(NANO_LANCE_BLOB_V2_GOLDEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/golden/blob_v2")

add_library(nanolance_proto STATIC generated/lance_minimal.pb.cpp)
target_compile_features(nanolance_proto PUBLIC cxx_std_20)
target_include_directories(nanolance_proto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/generated")

add_library(nanolance_reader STATIC
    src/manifest_reader.cpp
    src/nano_lance_reader.cpp
    src/nano_lance_external_blob.cpp
)
target_compile_features(nanolance_reader PUBLIC cxx_std_20)
target_include_directories(nanolance_reader PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${_NANOLANCE_GEN_INCLUDE}"
)
target_link_libraries(nanolance_reader PUBLIC nanolance_proto)

set(NANOLANCE_S3_CRYPTO "openssl" CACHE STRING
    "SigV4 crypto backend, forwarded to nanos3reader: openssl (default) | bundled")

# S3 external-blob reads. Two providers:
#   1. External AWS-SDK seam (legacy): the parent project supplies NANOLANCE_S3_TARGET + a directory with
#      s3_aws_sdk_stream.h. Used when NANOLANCE_S3_TARGET is set, so existing embedders are unaffected.
#   2. nanos3reader (default): the standalone read-only S3 range reader (the same code that used to live
#      in-tree as src/s3_min_reader.cpp), pulled in via FetchContent. Used whenever S3 is enabled and no
#      external AWS-SDK target is given.
if(NANOLANCE_ENABLE_S3 AND NANOLANCE_S3_TARGET AND TARGET "${NANOLANCE_S3_TARGET}" AND NANOLANCE_S3_INCLUDE_DIR)
    target_include_directories(nanolance_reader PRIVATE "${NANOLANCE_S3_INCLUDE_DIR}")
    target_link_libraries(nanolance_reader PRIVATE "${NANOLANCE_S3_TARGET}")
    target_compile_definitions(nanolance_reader PRIVATE NANO_LANCE_READER_HAS_S3=1 NANO_LANCE_S3_AWS_SDK=1)
elseif(NANOLANCE_ENABLE_S3)
    include(FetchContent)
    set(NANOS3READER_CRYPTO "${NANOLANCE_S3_CRYPTO}" CACHE STRING "" FORCE)  # forward the SigV4 crypto backend
    set(NANOS3READER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NANOS3READER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # nanos3reader 0.2.0 adds the on-disk LRU block cache (configure_disk_cache) that
    # nano_lance_block_cache_configure() forwards to. Pinned to the 0.2.0 commit; switch to the v0.2.0 tag
    # once it is published. An older reader still builds — the wrapper version-gates on NANOS3READER_VERSION.
    FetchContent_Declare(nanos3reader
        GIT_REPOSITORY https://github.com/yoavbendor/nanos3reader.git
        GIT_TAG 21558644771b37b6fefa440ee42ff7e8281c4748)
    FetchContent_MakeAvailable(nanos3reader)
    target_link_libraries(nanolance_reader PRIVATE nanos3reader::nanos3reader)
    target_compile_definitions(nanolance_reader PRIVATE NANO_LANCE_READER_HAS_S3=1)
    set(NANOLANCE_S3_NANOS3READER_ACTIVE ON)
endif()

add_library(nanolance STATIC
    src/version.cpp
    src/array_accessor.cpp
    src/blob_builder.cpp
    src/blob_v2_external.cpp
    src/data_file_reader.cpp
    src/data_file_writer.cpp
    src/lance_column_decoder.cpp
    src/lance_table_reader.cpp
    src/page_layout.cpp
    src/nano_lance_table_reader_c.cpp
    src/manifest_writer.cpp
    src/dataset_stitcher.cpp
    src/schema_mapper.cpp
    src/writer.cpp
)
target_compile_features(nanolance PUBLIC cxx_std_20)
target_include_directories(nanolance PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${_NANOLANCE_GEN_INCLUDE}"
    "${nanoarrow_SOURCE_DIR}/src"
)
target_link_libraries(nanolance PUBLIC
    nanolance_reader
    nanolance_proto
    nanoarrow_ipc_static
    nanoarrow_static
    "${NANOLANCE_ZSTD_TARGET}"
)

add_library(nanolance::nanolance ALIAS nanolance)

# Apply our own warning policy (opt-in via NANOLANCE_WERROR) only to nanolance's targets, not to the
# FetchContent'd dependencies.
if(NANOLANCE_WARNING_FLAGS)
    target_compile_options(nanolance_proto PRIVATE ${NANOLANCE_WARNING_FLAGS})
    target_compile_options(nanolance_reader PRIVATE ${NANOLANCE_WARNING_FLAGS})
    target_compile_options(nanolance PRIVATE ${NANOLANCE_WARNING_FLAGS})
endif()

if(NANOLANCE_ENABLE_AVX2)
    message(STATUS "nanolance: -march=haswell enabled (NANOLANCE_ENABLE_AVX2) -- minimum CPU raised to AVX2+BMI2+FMA")
    target_compile_options(nanolance_proto PRIVATE -march=haswell)
    target_compile_options(nanolance_reader PRIVATE -march=haswell)
    target_compile_options(nanolance PRIVATE -march=haswell)
endif()

# ── libFuzzer harness over the untrusted decode path (opt-in, Clang only) ─────────────────────────
# Defined here (not in the test block) so it builds independently of NANOLANCE_BUILD_TESTS.
if(NANOLANCE_BUILD_FUZZERS)
    add_executable(nanolance_fuzz_decode tests/fuzz/fuzz_decode.cpp)
    target_compile_features(nanolance_fuzz_decode PRIVATE cxx_std_20)
    target_link_libraries(nanolance_fuzz_decode PRIVATE nanolance)
    target_link_options(nanolance_fuzz_decode PRIVATE -fsanitize=fuzzer)

    # The PageLayout descriptor gets its own target: libFuzzer's coverage feedback works best aimed
    # at one grammar, and descriptor bytes look nothing like a data file.
    add_executable(nanolance_fuzz_page_layout tests/fuzz/fuzz_page_layout.cpp)
    target_compile_features(nanolance_fuzz_page_layout PRIVATE cxx_std_20)
    target_link_libraries(nanolance_fuzz_page_layout PRIVATE nanolance)
    target_link_options(nanolance_fuzz_page_layout PRIVATE -fsanitize=fuzzer)
endif()

if(NANOLANCE_BUILD_TOOLS)
    add_executable(arrowipc2lance arrowipc2lance_main.cpp)
    target_compile_features(arrowipc2lance PRIVATE cxx_std_20)
    target_link_libraries(arrowipc2lance PRIVATE CLI11::CLI11 nanolance)

    add_executable(nlbench tools/nlbench.cpp)
    target_compile_features(nlbench PRIVATE cxx_std_20)
    target_link_libraries(nlbench PRIVATE nanolance)

    add_executable(nlbench_gendata tools/nlbench_gendata.cpp)
    target_compile_features(nlbench_gendata PRIVATE cxx_std_20)
    target_link_libraries(nlbench_gendata PRIVATE nanolance)

    add_executable(nlance2table tools/nlance2table.cpp)
    target_compile_features(nlance2table PRIVATE cxx_std_20)
    target_link_libraries(nlance2table PRIVATE CLI11::CLI11 nanolance)

    # nlance-stitch: gather/merge many free-standing worker .lance folders into one self-contained dataset
    # (Option A: relocate fragments into one data/ + write one master manifest; no column data re-encode).
    add_executable(nlance_stitch tools/nlance_stitch.cpp)
    target_compile_features(nlance_stitch PRIVATE cxx_std_20)
    target_link_libraries(nlance_stitch PRIVATE CLI11::CLI11 nanolance)

    # Blob-fetch benchmark tooling: generate a tiled external-blob dataset, and fetch via nanolance.
    add_executable(nlance_blobgen tools/nlance_blobgen.cpp)
    target_compile_features(nlance_blobgen PRIVATE cxx_std_20)
    target_link_libraries(nlance_blobgen PRIVATE CLI11::CLI11 nanolance)

    add_executable(nlance_blobfetch tools/nlance_blobfetch.cpp)
    target_compile_features(nlance_blobfetch PRIVATE cxx_std_20)
    target_link_libraries(nlance_blobfetch PRIVATE CLI11::CLI11 nanolance)

    add_executable(nlance_pagelayout tools/nlance_pagelayout.cpp)
    target_compile_features(nlance_pagelayout PRIVATE cxx_std_20)
    target_link_libraries(nlance_pagelayout PRIVATE nanolance)
    set_target_properties(nlance_pagelayout PROPERTIES OUTPUT_NAME nlance-pagelayout)

    add_executable(nlance_info tools/nlance_info.cpp)
    target_compile_features(nlance_info PRIVATE cxx_std_20)
    target_link_libraries(nlance_info PRIVATE nanolance)
endif()

if(NANOLANCE_STANDALONE)
    option(NANOLANCE_BUILD_EXAMPLES "Build examples (pcapng2lance + nanotins)" ON)
else()
    option(NANOLANCE_BUILD_EXAMPLES "Build examples (pcapng2lance + nanotins)" OFF)
endif()
# The reflection/parsing stack (soatins + nanotins) is NOT a dependency of nanolance itself — nanolance
# reads/writes Lance using only nanoarrow + zstd + local code. It is a consumer-side dependency of the
# pcapng2lance example, which vendors it as a git submodule and adds it itself (see
# examples/pcapng2lance/CMakeLists.txt). A plain nanolance build (library, tools, tests) needs no submodule.
# Skip (don't fail) when the submodule is absent: a plain `git clone` without --recursive is the
# documented way to build the library, and EXAMPLES defaults ON for standalone builds, so a hard
# error here would break the quick start in README.md / docs/getting-started.md. Same graceful-skip
# posture fuselance already takes when libfuse3 is missing.
if(NANOLANCE_BUILD_EXAMPLES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/pcapng2lance/extern/nanotins/CMakeLists.txt")
        add_subdirectory(examples/pcapng2lance)
    else()
        message(STATUS
            "nanolance: skipping the pcapng2lance example -- its nanotins submodule is not checked "
            "out. The library, tools and tests build fine without it. To build the example, run: "
            "git submodule update --init --recursive")
    endif()
endif()

# pcapng2lance_nanom — the same L1 converter built on nanom instead of nanotins, for a head-to-head
# benchmark. nanom is an OPTIONAL, non-mandatory dependency pulled in ONLY by this example (its own git
# submodule under examples/pcapng2lance_nanom/extern/nanom), so this is its own opt-in toggle, OFF by
# default and independent of NANOLANCE_BUILD_EXAMPLES: a normal build never needs the nanom submodule.
option(NANOLANCE_BUILD_PCAPNG2LANCE_NANOM
       "Build the nanom-based pcapng2lance example (needs the nanom submodule)" OFF)
if(NANOLANCE_BUILD_PCAPNG2LANCE_NANOM)
    add_subdirectory(examples/pcapng2lance_nanom)
endif()

# fuselance — mount a Lance blob.v2 table as a read-only FUSE filesystem. Independent of the
# NANOLANCE_BUILD_EXAMPLES toggle above because that one pulls in pcapng2lance's nanotins submodule;
# fuselance needs only nanolance_reader + libfuse3, so it gets its own opt-in. Its CMakeLists skips
# itself gracefully when libfuse3 is not installed.
if(NANOLANCE_STANDALONE)
    option(NANOLANCE_BUILD_FUSE_EXAMPLE "Build the fuselance FUSE example (needs libfuse3-dev)" ON)
else()
    option(NANOLANCE_BUILD_FUSE_EXAMPLE "Build the fuselance FUSE example (needs libfuse3-dev)" OFF)
endif()
if(NANOLANCE_BUILD_FUSE_EXAMPLE)
    add_subdirectory(examples/fuselance)
endif()

if(NOT NANOLANCE_BUILD_TESTS)
    return()
endif()

add_executable(nano_lance_proto_roundtrip_test tests/test_lance_minimal_pb.cpp)
target_compile_features(nano_lance_proto_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_proto_roundtrip_test PRIVATE nanolance_proto)

add_executable(nano_lance_writer_lifecycle_test tests/test_writer_lifecycle.cpp)
target_compile_features(nano_lance_writer_lifecycle_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_writer_lifecycle_test PRIVATE nanolance)

add_executable(nano_lance_blob_builder_test tests/test_blob_builder.cpp)
target_compile_features(nano_lance_blob_builder_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_blob_builder_test PRIVATE nanolance)

add_executable(nano_lance_blob_v2_external_test tests/test_blob_v2_external_preprocess.cpp)
target_compile_features(nano_lance_blob_v2_external_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_blob_v2_external_test PRIVATE nanolance)

# Columnar blob.v2 encapsulation: producer supplies position/size columns, nanolance owns the envelope.
# (Depends only on nanolance — no soatins; the reflection stack is the example's concern, not nanolance's.)
add_executable(nano_lance_blob_v2_columnar_test tests/test_blob_v2_columnar.cpp)
target_compile_features(nano_lance_blob_v2_columnar_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_blob_v2_columnar_test PRIVATE nanolance)

add_executable(nano_lance_blob_v2_uri_dictionary_test tests/test_blob_v2_uri_dictionary.cpp)
target_compile_features(nano_lance_blob_v2_uri_dictionary_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_blob_v2_uri_dictionary_test PRIVATE nanolance)

add_executable(nano_lance_compression_roundtrip_test tests/test_compression_roundtrip.cpp)
target_compile_features(nano_lance_compression_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_compression_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_bool_roundtrip_test tests/test_bool_roundtrip.cpp)
target_compile_features(nano_lance_bool_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_bool_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_bitpacking_roundtrip_test tests/test_bitpacking_roundtrip.cpp)
target_compile_features(nano_lance_bitpacking_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_bitpacking_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_writer_options_test tests/test_writer_options.cpp)
target_compile_features(nano_lance_writer_options_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_writer_options_test PRIVATE nanolance)

add_executable(nano_lance_typed_writer_test tests/test_typed_writer.cpp)
target_compile_features(nano_lance_typed_writer_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_typed_writer_test PRIVATE nanolance)

add_executable(nano_lance_typed_blob_writer_test tests/test_typed_blob_writer.cpp)
target_compile_features(nano_lance_typed_blob_writer_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_typed_blob_writer_test PRIVATE nanolance)

add_executable(nano_lance_constant_roundtrip_test tests/test_constant_roundtrip.cpp)
target_compile_features(nano_lance_constant_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_constant_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_rle_roundtrip_test tests/test_rle_roundtrip.cpp)
target_compile_features(nano_lance_rle_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_rle_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_dict_rle_roundtrip_test tests/test_dict_rle_roundtrip.cpp)
target_compile_features(nano_lance_dict_rle_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_dict_rle_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_structural_default_test tests/test_structural_default.cpp)
target_compile_features(nano_lance_structural_default_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_structural_default_test PRIVATE nanolance)

add_executable(nano_lance_schema_change_error_test tests/test_schema_change_error.cpp)
target_compile_features(nano_lance_schema_change_error_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_schema_change_error_test PRIVATE nanolance)

add_executable(nano_lance_read_safety_test tests/test_read_safety.cpp)
target_compile_features(nano_lance_read_safety_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_read_safety_test PRIVATE nanolance)

add_executable(nano_lance_dict_roundtrip_test tests/test_dict_roundtrip.cpp)
target_compile_features(nano_lance_dict_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_dict_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_page_layout_test tests/test_page_layout.cpp)
target_link_libraries(nano_lance_page_layout_test PRIVATE nanolance)
target_compile_features(nano_lance_page_layout_test PRIVATE cxx_std_20)

add_executable(nano_lance_readme_quickstart_test tests/test_readme_quickstart.c)
target_link_libraries(nano_lance_readme_quickstart_test PRIVATE nanolance)
set_property(TARGET nano_lance_readme_quickstart_test PROPERTY C_STANDARD 99)

add_executable(nano_lance_null_rejection_test tests/test_null_rejection.cpp)
target_link_libraries(nano_lance_null_rejection_test PRIVATE nanolance)
target_compile_features(nano_lance_null_rejection_test PRIVATE cxx_std_20)

add_executable(nano_lance_array_accessor_test tests/test_array_accessor.cpp)
target_compile_features(nano_lance_array_accessor_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_array_accessor_test PRIVATE nanolance)

add_executable(nano_lance_schema_mapper_test tests/test_schema_mapper.cpp)
target_compile_features(nano_lance_schema_mapper_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_schema_mapper_test PRIVATE nanolance nanoarrow_ipc_static)
target_compile_definitions(nano_lance_schema_mapper_test PRIVATE
    NANO_LANCE_BLOB_V2_GOLDEN_DIR="${NANO_LANCE_BLOB_V2_GOLDEN_DIR}"
)

add_executable(nano_lance_manifest_reader_test tests/test_nano_lance_manifest_reader.cpp)
target_compile_features(nano_lance_manifest_reader_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_manifest_reader_test PRIVATE nanolance)
target_compile_definitions(nano_lance_manifest_reader_test PRIVATE
    NANO_LANCE_BLOB_V2_GOLDEN_DIR="${NANO_LANCE_BLOB_V2_GOLDEN_DIR}"
)

add_executable(nano_lance_writer_append_test tests/test_nano_lance_writer_append.cpp)
target_compile_features(nano_lance_writer_append_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_writer_append_test PRIVATE nanolance)

add_executable(nano_lance_stitch_test tests/test_dataset_stitch.cpp)
target_compile_features(nano_lance_stitch_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_stitch_test PRIVATE nanolance)

add_executable(nano_lance_data_file_reader_test tests/test_data_file_reader.cpp)
target_compile_features(nano_lance_data_file_reader_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_data_file_reader_test PRIVATE nanolance)

add_executable(nano_lance_table_roundtrip_test tests/test_nano_lance_table_roundtrip.cpp)
target_compile_features(nano_lance_table_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_table_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_external_blob_test tests/test_nano_lance_external_blob.cpp)
target_compile_features(nano_lance_external_blob_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_external_blob_test PRIVATE nanolance_reader)

# Live S3 integration test (only with the nanos3reader provider): range GETs against a real S3-compatible
# endpoint; skips unless NANOLANCE_S3_TEST_URI (+ AWS_* env) is set. tests/s3_minio_integration.sh wires
# this up against a throwaway MinIO. The SigV4 signer vectors now live in the nanos3reader repo's own tests.
if(NANOLANCE_S3_NANOS3READER_ACTIVE)
    add_executable(nano_lance_s3_min_integration_test tests/test_s3_min_integration.cpp)
    target_compile_features(nano_lance_s3_min_integration_test PRIVATE cxx_std_20)
    target_link_libraries(nano_lance_s3_min_integration_test PRIVATE nanolance_reader nanos3reader::nanos3reader)
endif()

if(NANOLANCE_BUILD_TOOLS)
    add_test(NAME app_arrowipc2lance_help COMMAND $<TARGET_FILE:arrowipc2lance> -h)
    set_tests_properties(app_arrowipc2lance_help PROPERTIES LABELS "smoke")
    add_test(NAME app_arrowipc2lance_version COMMAND $<TARGET_FILE:arrowipc2lance> --version)
    set_tests_properties(app_arrowipc2lance_version PROPERTIES LABELS "smoke")
endif()

add_test(NAME nano_lance_proto_roundtrip COMMAND $<TARGET_FILE:nano_lance_proto_roundtrip_test>)
set_tests_properties(nano_lance_proto_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_writer_lifecycle COMMAND $<TARGET_FILE:nano_lance_writer_lifecycle_test>)
set_tests_properties(nano_lance_writer_lifecycle PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_blob_builder COMMAND $<TARGET_FILE:nano_lance_blob_builder_test>)
set_tests_properties(nano_lance_blob_builder PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_blob_v2_external_preprocess COMMAND $<TARGET_FILE:nano_lance_blob_v2_external_test>)
add_test(NAME nano_lance_blob_v2_columnar COMMAND $<TARGET_FILE:nano_lance_blob_v2_columnar_test>)
set_tests_properties(nano_lance_blob_v2_external_preprocess PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_blob_v2_uri_dictionary COMMAND $<TARGET_FILE:nano_lance_blob_v2_uri_dictionary_test>)
set_tests_properties(nano_lance_blob_v2_uri_dictionary PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_compression_roundtrip COMMAND $<TARGET_FILE:nano_lance_compression_roundtrip_test>)
set_tests_properties(nano_lance_compression_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_bool_roundtrip COMMAND $<TARGET_FILE:nano_lance_bool_roundtrip_test>)
set_tests_properties(nano_lance_bool_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_writer_options COMMAND $<TARGET_FILE:nano_lance_writer_options_test>)
set_tests_properties(nano_lance_writer_options PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_typed_writer COMMAND $<TARGET_FILE:nano_lance_typed_writer_test>)
set_tests_properties(nano_lance_typed_writer PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_typed_blob_writer COMMAND $<TARGET_FILE:nano_lance_typed_blob_writer_test>)
set_tests_properties(nano_lance_typed_blob_writer PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_bitpacking_roundtrip COMMAND $<TARGET_FILE:nano_lance_bitpacking_roundtrip_test>)
set_tests_properties(nano_lance_bitpacking_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_constant_roundtrip COMMAND $<TARGET_FILE:nano_lance_constant_roundtrip_test>)
set_tests_properties(nano_lance_constant_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_rle_roundtrip COMMAND $<TARGET_FILE:nano_lance_rle_roundtrip_test>)
set_tests_properties(nano_lance_rle_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_dict_rle_roundtrip COMMAND $<TARGET_FILE:nano_lance_dict_rle_roundtrip_test>)
set_tests_properties(nano_lance_dict_rle_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_structural_default COMMAND $<TARGET_FILE:nano_lance_structural_default_test>)
set_tests_properties(nano_lance_structural_default PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_schema_change_error COMMAND $<TARGET_FILE:nano_lance_schema_change_error_test>)
set_tests_properties(nano_lance_schema_change_error PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_read_safety COMMAND $<TARGET_FILE:nano_lance_read_safety_test>)
set_tests_properties(nano_lance_read_safety PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_dict_roundtrip COMMAND $<TARGET_FILE:nano_lance_dict_roundtrip_test>)
set_tests_properties(nano_lance_dict_roundtrip PROPERTIES LABELS "smoke")

if(TARGET nlance_pagelayout AND NANOLANCE_PYTHON)
    add_test(NAME app_pagelayout_pylance_interop_smoke
             COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_pagelayout_pylance_interop.sh"
                     "$<TARGET_FILE:nlance_pagelayout>" "${NANOLANCE_PYTHON}")
    set_tests_properties(app_pagelayout_pylance_interop_smoke PROPERTIES LABELS smoke)
endif()

add_test(NAME nano_lance_page_layout
         COMMAND $<TARGET_FILE:nano_lance_page_layout_test> "${CMAKE_CURRENT_BINARY_DIR}/page_layout_probe")
set_tests_properties(nano_lance_page_layout PROPERTIES LABELS smoke)

add_test(NAME nano_lance_readme_quickstart
         COMMAND $<TARGET_FILE:nano_lance_readme_quickstart_test>
                 "${CMAKE_CURRENT_BINARY_DIR}/readme_quickstart.lance")
set_tests_properties(nano_lance_readme_quickstart PROPERTIES LABELS smoke)

add_test(NAME nano_lance_null_rejection COMMAND $<TARGET_FILE:nano_lance_null_rejection_test>)
set_tests_properties(nano_lance_null_rejection PROPERTIES LABELS smoke)

add_test(NAME nano_lance_array_accessor COMMAND $<TARGET_FILE:nano_lance_array_accessor_test>)
set_tests_properties(nano_lance_array_accessor PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_schema_mapper COMMAND $<TARGET_FILE:nano_lance_schema_mapper_test>)
set_tests_properties(nano_lance_schema_mapper PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_manifest_reader COMMAND $<TARGET_FILE:nano_lance_manifest_reader_test>)
set_tests_properties(nano_lance_manifest_reader PROPERTIES LABELS "smoke" DEPENDS nano_lance_blob_v2_golden_generate)

add_test(NAME nano_lance_writer_append COMMAND $<TARGET_FILE:nano_lance_writer_append_test>)
set_tests_properties(nano_lance_writer_append PROPERTIES LABELS "smoke")

# Gather/stitch: merge N free-standing datasets into one master == an in-order single dataset, validated
# through the full Lance read path via nlance2table (no Python). Needs the nlance2table tool.
if(TARGET nlance2table)
    add_test(NAME nano_lance_stitch
             COMMAND $<TARGET_FILE:nano_lance_stitch_test> $<TARGET_FILE:nlance2table>)
    set_tests_properties(nano_lance_stitch PROPERTIES LABELS "smoke")
endif()

add_test(NAME nano_lance_data_file_reader COMMAND $<TARGET_FILE:nano_lance_data_file_reader_test>)
set_tests_properties(nano_lance_data_file_reader PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_table_roundtrip COMMAND $<TARGET_FILE:nano_lance_table_roundtrip_test>)
set_tests_properties(nano_lance_table_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_external_blob COMMAND $<TARGET_FILE:nano_lance_external_blob_test>)
set_tests_properties(nano_lance_external_blob PROPERTIES LABELS "smoke")

if(NANOLANCE_S3_NANOS3READER_ACTIVE)
    # Live S3 integration: skips (CTest code 77) unless NANOLANCE_S3_TEST_URI + AWS_* env are set. Not in
    # the smoke set, so the default run never needs network or credentials.
    add_test(NAME nano_lance_s3_min_integration COMMAND $<TARGET_FILE:nano_lance_s3_min_integration_test>)
    set_tests_properties(nano_lance_s3_min_integration PROPERTIES LABELS "s3-integration" SKIP_RETURN_CODE 77)
endif()

add_test(
    NAME nano_lance_golden_python_deps
    COMMAND "${NANO_LANCE_WRITER_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_golden_lance_v22.py" --check-only
)
set_tests_properties(nano_lance_golden_python_deps PROPERTIES LABELS "smoke" SKIP_RETURN_CODE 77)

add_test(
    NAME nano_lance_blob_v2_golden_python_deps
    COMMAND "${NANO_LANCE_WRITER_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_golden_blob_v2.py" --check-only
)
set_tests_properties(nano_lance_blob_v2_golden_python_deps PROPERTIES LABELS "smoke" SKIP_RETURN_CODE 77)

add_test(
    NAME nano_lance_blob_v2_golden_generate
    COMMAND "${NANO_LANCE_WRITER_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_golden_blob_v2.py"
            --output-dir "${NANO_LANCE_BLOB_V2_GOLDEN_DIR}"
)
set_tests_properties(nano_lance_blob_v2_golden_generate PROPERTIES LABELS "smoke" SKIP_RETURN_CODE 77)

add_test(
    NAME nano_lance_blob_v2_golden_validate
    COMMAND "${NANO_LANCE_WRITER_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate_golden_blob_v2.py"
            --output-dir "${NANO_LANCE_BLOB_V2_GOLDEN_DIR}" --validate
)
set_tests_properties(nano_lance_blob_v2_golden_validate PROPERTIES
    LABELS "smoke"
    SKIP_RETURN_CODE 77
    DEPENDS nano_lance_blob_v2_golden_generate
)

if(NANOLANCE_BUILD_TOOLS)
    add_test(
        NAME app_arrowipc2lance_blob_v2_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_arrowipc2lance_blob_v2.sh"
                "$<TARGET_FILE:arrowipc2lance>"
                "${NANO_LANCE_WRITER_PYTHON}"
                "${NANO_LANCE_BLOB_V2_GOLDEN_DIR}"
    )
    set_tests_properties(app_arrowipc2lance_blob_v2_smoke PROPERTIES LABELS "smoke" DEPENDS nano_lance_blob_v2_golden_generate)

    add_test(
        NAME app_arrowipc2lance_nonnull_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_arrowipc2lance_nonnull.sh"
                "$<TARGET_FILE:arrowipc2lance>"
                "${NANO_LANCE_WRITER_PYTHON}"
    )
    set_tests_properties(app_arrowipc2lance_nonnull_smoke PROPERTIES LABELS "smoke")

    add_test(
        NAME app_arrowipc2lance_ignore_nullability_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_arrowipc2lance_ignore_nullability.sh"
                "$<TARGET_FILE:arrowipc2lance>"
                "${NANO_LANCE_WRITER_PYTHON}"
    )
    set_tests_properties(app_arrowipc2lance_ignore_nullability_smoke PROPERTIES LABELS "smoke")

    add_test(
        NAME app_arrowipc2lance_inspect_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_arrowipc2lance_inspect.sh"
                "$<TARGET_FILE:arrowipc2lance>"
                "${NANO_LANCE_WRITER_PYTHON}"
    )
    set_tests_properties(app_arrowipc2lance_inspect_smoke PROPERTIES LABELS "smoke")

    add_test(
        NAME app_arrowipc2lance_append_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_arrowipc2lance_append.sh"
                "$<TARGET_FILE:arrowipc2lance>"
                "${NANO_LANCE_WRITER_PYTHON}"
    )
    set_tests_properties(app_arrowipc2lance_append_smoke PROPERTIES LABELS "smoke")

    add_test(
        NAME app_dict_pylance_interop_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_dict_pylance_interop.sh"
                "$<TARGET_FILE:arrowipc2lance>"
                "${NANO_LANCE_WRITER_PYTHON}"
    )
    set_tests_properties(app_dict_pylance_interop_smoke PROPERTIES LABELS "smoke" SKIP_RETURN_CODE 77)

    add_test(
        NAME app_bss_zstd_pylance_interop_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_bss_zstd_pylance_interop.sh"
                "$<TARGET_FILE:arrowipc2lance>"
                "${NANO_LANCE_WRITER_PYTHON}"
    )
    set_tests_properties(app_bss_zstd_pylance_interop_smoke PROPERTIES LABELS "smoke" SKIP_RETURN_CODE 77)

    add_test(
        NAME app_bool_pylance_interop_smoke
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_bool_pylance_interop.sh"
                "$<TARGET_FILE:arrowipc2lance>"
                "${NANO_LANCE_WRITER_PYTHON}"
    )
    set_tests_properties(app_bool_pylance_interop_smoke PROPERTIES LABELS "smoke" SKIP_RETURN_CODE 77)

    # Negative: nlance-stitch must reject datasets with different schemas. Builds a pcapng2lance L1 table
    # and an arrowipc2lance (id/score) table and asserts the stitch fails. Lives here because it needs
    # arrowipc2lance (defined in this block) alongside pcapng2lance + nlance_stitch. Skips if pyarrow absent.
    if(TARGET pcapng2lance AND TARGET nlance_stitch)
        add_test(
            NAME pcapng2lance_stitch_schema_mismatch
            COMMAND "${NANO_LANCE_WRITER_PYTHON}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/examples/pcapng2lance/tests/test_pcapng2lance_stitch_schema_mismatch.py"
                    "$<TARGET_FILE:pcapng2lance>" "$<TARGET_FILE:nlance_stitch>" "$<TARGET_FILE:arrowipc2lance>"
                    "${CMAKE_CURRENT_SOURCE_DIR}/examples/pcapng2lance/tests/SRL_front_left_51_short.pcapng"
        )
        set_tests_properties(pcapng2lance_stitch_schema_mismatch PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)
    endif()
endif()
