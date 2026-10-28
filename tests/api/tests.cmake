# Tests of the api layer added after the move into tests/api/ (src/api/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/api/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.

add_executable(nano_lance_writer_internal_test tests/api/test_writer_internal.cpp)
target_compile_features(nano_lance_writer_internal_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_writer_internal_test PRIVATE nanolance)
add_test(NAME nano_lance_writer_internal COMMAND $<TARGET_FILE:nano_lance_writer_internal_test>)
set_tests_properties(nano_lance_writer_internal PROPERTIES LABELS "smoke")

if(TARGET lance_c)
    add_executable(nano_lance_lance_c_dataset_test tests/api/test_lance_c_dataset.cpp)
    target_compile_features(nano_lance_lance_c_dataset_test PRIVATE cxx_std_20)
    target_link_libraries(nano_lance_lance_c_dataset_test PRIVATE lance_c nanolance)
    add_test(NAME nano_lance_lance_c_dataset COMMAND $<TARGET_FILE:nano_lance_lance_c_dataset_test>
             "${CMAKE_CURRENT_BINARY_DIR}/lance_c_dataset_tmp")
    set_tests_properties(nano_lance_lance_c_dataset PROPERTIES LABELS "smoke")
endif()

# The table reader's one-shot reads, projection, row take and ranged stream (not covered by nano_lance_table_stream).
add_executable(nano_lance_table_reader_c_test tests/api/test_nano_lance_table_reader_c.cpp)
target_compile_features(nano_lance_table_reader_c_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_table_reader_c_test PRIVATE nanolance)
add_test(NAME nano_lance_table_reader_c COMMAND $<TARGET_FILE:nano_lance_table_reader_c_test>)

# nano_lance::Writer (writer.hpp): the calls on a closed writer, bad opens, append mode, re-open, moves, flushes.
add_executable(nano_lance_writer_cxx_test tests/api/test_writer_cxx.cpp)
target_compile_features(nano_lance_writer_cxx_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_writer_cxx_test PRIVATE nanolance)
add_test(NAME nano_lance_writer_cxx COMMAND $<TARGET_FILE:nano_lance_writer_cxx_test>)

# nano_lance_external_blob_size and the argument and end-of-object rules of nano_lance_fetch_external_blob.
add_executable(nano_lance_external_blob_size_test tests/api/test_nano_lance_external_blob_size.cpp)
target_compile_features(nano_lance_external_blob_size_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_external_blob_size_test PRIVATE nanolance)
add_test(NAME nano_lance_external_blob_size COMMAND $<TARGET_FILE:nano_lance_external_blob_size_test>)
