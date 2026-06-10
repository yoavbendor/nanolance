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
    project(nanolance VERSION ${NANOLANCE_VERSION_MAJOR}.${NANOLANCE_VERSION_MINOR}.${NANOLANCE_VERSION_PATCH} LANGUAGES CXX)
    enable_testing()
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

set(NANOLANCE_S3_TARGET "" CACHE STRING "Target for S3 reads (e.g. stream_helper_s3)")
set(NANOLANCE_S3_INCLUDE_DIR "" CACHE PATH "Dir containing s3_aws_sdk_stream.h")

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
        GIT_SHALLOW TRUE
    )
    FetchContent_Populate(nanoarrow)
    add_subdirectory(${nanoarrow_SOURCE_DIR} ${nanoarrow_BINARY_DIR} EXCLUDE_FROM_ALL)
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

if(NANOLANCE_ENABLE_S3 AND NANOLANCE_S3_TARGET AND TARGET "${NANOLANCE_S3_TARGET}" AND NANOLANCE_S3_INCLUDE_DIR)
    target_include_directories(nanolance_reader PRIVATE "${NANOLANCE_S3_INCLUDE_DIR}")
    target_link_libraries(nanolance_reader PRIVATE "${NANOLANCE_S3_TARGET}")
    target_compile_definitions(nanolance_reader PRIVATE NANO_LANCE_READER_HAS_S3=1)
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
    src/nano_lance_table_reader_c.cpp
    src/manifest_writer.cpp
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

if(NANOLANCE_BUILD_TOOLS)
    add_executable(arrowipc2lance arrowipc2lance_main.cpp)
    target_compile_features(arrowipc2lance PRIVATE cxx_std_20)
    target_link_libraries(arrowipc2lance PRIVATE CLI11::CLI11 nanolance)

    add_executable(nlbench tools/nlbench.cpp)
    target_compile_features(nlbench PRIVATE cxx_std_20)
    target_link_libraries(nlbench PRIVATE nanolance)

    add_executable(nlance2table tools/nlance2table.cpp)
    target_compile_features(nlance2table PRIVATE cxx_std_20)
    target_link_libraries(nlance2table PRIVATE CLI11::CLI11 nanolance)
endif()

if(NANOLANCE_STANDALONE)
    option(NANOLANCE_BUILD_EXAMPLES "Build examples (pcapng2lance + nanotins)" ON)
else()
    option(NANOLANCE_BUILD_EXAMPLES "Build examples (pcapng2lance + nanotins)" OFF)
endif()
# nanotins: the standalone pcap/pcapng + L2/L3/L4 parsing & reflection library (nanoarrow + boost only).
# Built whenever the examples that consume it are, or tests are on. Its unit tests follow NANOLANCE_BUILD_TESTS.
if(NANOLANCE_BUILD_EXAMPLES OR NANOLANCE_BUILD_TESTS)
    set(NANOTINS_BUILD_TESTS ${NANOLANCE_BUILD_TESTS} CACHE BOOL "" FORCE)
    add_subdirectory(nanotins)
endif()

if(NANOLANCE_BUILD_EXAMPLES)
    add_subdirectory(examples/pcapng2lance)
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

add_executable(nano_lance_blob_v2_uri_dictionary_test tests/test_blob_v2_uri_dictionary.cpp)
target_compile_features(nano_lance_blob_v2_uri_dictionary_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_blob_v2_uri_dictionary_test PRIVATE nanolance)

add_executable(nano_lance_compression_roundtrip_test tests/test_compression_roundtrip.cpp)
target_compile_features(nano_lance_compression_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_compression_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_bitpacking_roundtrip_test tests/test_bitpacking_roundtrip.cpp)
target_compile_features(nano_lance_bitpacking_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_bitpacking_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_constant_roundtrip_test tests/test_constant_roundtrip.cpp)
target_compile_features(nano_lance_constant_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_constant_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_rle_roundtrip_test tests/test_rle_roundtrip.cpp)
target_compile_features(nano_lance_rle_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_rle_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_dict_rle_roundtrip_test tests/test_dict_rle_roundtrip.cpp)
target_compile_features(nano_lance_dict_rle_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_dict_rle_roundtrip_test PRIVATE nanolance)

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

add_executable(nano_lance_data_file_reader_test tests/test_data_file_reader.cpp)
target_compile_features(nano_lance_data_file_reader_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_data_file_reader_test PRIVATE nanolance)

add_executable(nano_lance_table_roundtrip_test tests/test_nano_lance_table_roundtrip.cpp)
target_compile_features(nano_lance_table_roundtrip_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_table_roundtrip_test PRIVATE nanolance)

add_executable(nano_lance_external_blob_test tests/test_nano_lance_external_blob.cpp)
target_compile_features(nano_lance_external_blob_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_external_blob_test PRIVATE nanolance_reader)

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
set_tests_properties(nano_lance_blob_v2_external_preprocess PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_blob_v2_uri_dictionary COMMAND $<TARGET_FILE:nano_lance_blob_v2_uri_dictionary_test>)
set_tests_properties(nano_lance_blob_v2_uri_dictionary PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_compression_roundtrip COMMAND $<TARGET_FILE:nano_lance_compression_roundtrip_test>)
set_tests_properties(nano_lance_compression_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_bitpacking_roundtrip COMMAND $<TARGET_FILE:nano_lance_bitpacking_roundtrip_test>)
set_tests_properties(nano_lance_bitpacking_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_constant_roundtrip COMMAND $<TARGET_FILE:nano_lance_constant_roundtrip_test>)
set_tests_properties(nano_lance_constant_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_rle_roundtrip COMMAND $<TARGET_FILE:nano_lance_rle_roundtrip_test>)
set_tests_properties(nano_lance_rle_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_dict_rle_roundtrip COMMAND $<TARGET_FILE:nano_lance_dict_rle_roundtrip_test>)
set_tests_properties(nano_lance_dict_rle_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_array_accessor COMMAND $<TARGET_FILE:nano_lance_array_accessor_test>)
set_tests_properties(nano_lance_array_accessor PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_schema_mapper COMMAND $<TARGET_FILE:nano_lance_schema_mapper_test>)
set_tests_properties(nano_lance_schema_mapper PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_manifest_reader COMMAND $<TARGET_FILE:nano_lance_manifest_reader_test>)
set_tests_properties(nano_lance_manifest_reader PROPERTIES LABELS "smoke" DEPENDS nano_lance_blob_v2_golden_generate)

add_test(NAME nano_lance_writer_append COMMAND $<TARGET_FILE:nano_lance_writer_append_test>)
set_tests_properties(nano_lance_writer_append PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_data_file_reader COMMAND $<TARGET_FILE:nano_lance_data_file_reader_test>)
set_tests_properties(nano_lance_data_file_reader PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_table_roundtrip COMMAND $<TARGET_FILE:nano_lance_table_roundtrip_test>)
set_tests_properties(nano_lance_table_roundtrip PROPERTIES LABELS "smoke")

add_test(NAME nano_lance_external_blob COMMAND $<TARGET_FILE:nano_lance_external_blob_test>)
set_tests_properties(nano_lance_external_blob PROPERTIES LABELS "smoke")

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
endif()
