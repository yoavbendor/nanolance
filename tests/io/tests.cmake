# Tests of the io layer added after the move into tests/io/ (src/io/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/io/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.

add_executable(nano_lance_object_store_test tests/io/test_object_store.cpp)
target_compile_features(nano_lance_object_store_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_object_store_test PRIVATE nanolance)
add_test(NAME nano_lance_object_store COMMAND $<TARGET_FILE:nano_lance_object_store_test>)
set_tests_properties(nano_lance_object_store PROPERTIES LABELS "smoke")

add_executable(nano_lance_path_safety_test tests/io/test_path_safety.cpp)
target_compile_features(nano_lance_path_safety_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_path_safety_test PRIVATE nanolance)
add_test(NAME nano_lance_path_safety COMMAND $<TARGET_FILE:nano_lance_path_safety_test>)
set_tests_properties(nano_lance_path_safety PROPERTIES LABELS "smoke")
