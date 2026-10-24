# Tests of the api layer added after the move into tests/api/ (src/api/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/api/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.

add_executable(nano_lance_writer_internal_test tests/api/test_writer_internal.cpp)
target_compile_features(nano_lance_writer_internal_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_writer_internal_test PRIVATE nanolance)
add_test(NAME nano_lance_writer_internal COMMAND $<TARGET_FILE:nano_lance_writer_internal_test>)
set_tests_properties(nano_lance_writer_internal PROPERTIES LABELS "smoke")
