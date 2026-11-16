# Tests of the file layer added after the move into tests/file/ (src/file/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/file/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.

add_executable(nano_lance_data_file_writer_test tests/file/test_data_file_writer.cpp)
target_compile_features(nano_lance_data_file_writer_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_data_file_writer_test PRIVATE nanolance)
add_test(NAME nano_lance_data_file_writer COMMAND $<TARGET_FILE:nano_lance_data_file_writer_test>)
set_tests_properties(nano_lance_data_file_writer PROPERTIES LABELS "smoke")

add_executable(nano_lance_format_version_test tests/file/test_format_version.cpp)
target_compile_features(nano_lance_format_version_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_format_version_test PRIVATE nanolance)
add_test(NAME nano_lance_format_version COMMAND $<TARGET_FILE:nano_lance_format_version_test>)
set_tests_properties(nano_lance_format_version PROPERTIES LABELS "smoke")

# Null-typed children of lists and structs (W1k5): list<null> and struct<a: null, b: int64> written by nano_lance::Writer,
# read back by the C reader, checked level by level.
add_executable(nano_lance_null_children_write_test tests/file/test_null_children_write.cpp)
target_compile_features(nano_lance_null_children_write_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_null_children_write_test PRIVATE nanolance)
add_test(NAME nano_lance_null_children_write COMMAND $<TARGET_FILE:nano_lance_null_children_write_test>)
set_tests_properties(nano_lance_null_children_write PROPERTIES LABELS "smoke")
