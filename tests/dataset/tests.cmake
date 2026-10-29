# Tests of the dataset layer added after the move into tests/dataset/ (src/dataset/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/dataset/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.


add_executable(nano_lance_dataset_test tests/dataset/test_dataset.cpp)
target_compile_features(nano_lance_dataset_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_dataset_test PRIVATE nanolance)
add_test(NAME nano_lance_dataset COMMAND $<TARGET_FILE:nano_lance_dataset_test>)
set_tests_properties(nano_lance_dataset PROPERTIES LABELS "smoke")

add_executable(nano_lance_dataset_ops_test tests/dataset/test_dataset_ops.cpp)
target_compile_features(nano_lance_dataset_ops_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_dataset_ops_test PRIVATE nanolance)
add_test(NAME nano_lance_dataset_ops COMMAND $<TARGET_FILE:nano_lance_dataset_ops_test>)
set_tests_properties(nano_lance_dataset_ops PROPERTIES LABELS "smoke")

add_executable(nano_lance_index_segments_test tests/dataset/test_index_segments.cpp)
target_compile_features(nano_lance_index_segments_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_index_segments_test PRIVATE nanolance)
add_test(NAME nano_lance_index_segments COMMAND $<TARGET_FILE:nano_lance_index_segments_test>)
set_tests_properties(nano_lance_index_segments PROPERTIES LABELS "smoke")

add_executable(nano_lance_index_optimize_test tests/dataset/test_index_optimize.cpp)
target_compile_features(nano_lance_index_optimize_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_index_optimize_test PRIVATE nanolance)
add_test(NAME nano_lance_index_optimize COMMAND $<TARGET_FILE:nano_lance_index_optimize_test>)
set_tests_properties(nano_lance_index_optimize PROPERTIES LABELS "smoke")

# lance_dataset_scan and LanceTableStream::open_request: a whole scan request (version, fragments, projection,
# row addresses, range, filter) and its failures.
add_executable(nano_lance_dataset_scan_test tests/dataset/test_dataset_scan.cpp)
target_compile_features(nano_lance_dataset_scan_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_dataset_scan_test PRIVATE nanolance)
add_test(NAME nano_lance_dataset_scan COMMAND $<TARGET_FILE:nano_lance_dataset_scan_test>)
