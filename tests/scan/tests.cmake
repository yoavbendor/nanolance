# Tests of the scan layer added after the move into tests/scan/ (src/scan/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/scan/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.

add_executable(nano_lance_dict_row_selection_test tests/scan/test_dict_row_selection.cpp)
target_compile_features(nano_lance_dict_row_selection_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_dict_row_selection_test PRIVATE nanolance)
add_test(NAME nano_lance_dict_row_selection COMMAND $<TARGET_FILE:nano_lance_dict_row_selection_test>)
set_tests_properties(nano_lance_dict_row_selection PROPERTIES LABELS "smoke")

# A list of dictionary items, from a format 2.0 file pylance 12 wrote (tests/golden/list_dictionary_v20).
add_executable(nano_lance_list_dictionary_read_test tests/scan/test_list_dictionary_read.cpp)
target_compile_features(nano_lance_list_dictionary_read_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_list_dictionary_read_test PRIVATE nanolance)
add_test(NAME nano_lance_list_dictionary_read
         COMMAND $<TARGET_FILE:nano_lance_list_dictionary_read_test>
                 "${PROJECT_SOURCE_DIR}/tests/golden/list_dictionary_v20/dataset.lance")
set_tests_properties(nano_lance_list_dictionary_read PROPERTIES LABELS "smoke")

# A fixed_size_list of structs with a list or map field, in format 2.2 files pylance 12 wrote
# (tests/golden/fsl_struct_v22): the list, large-list and map variants, read whole, by a row range and by take.
add_executable(nano_lance_fsl_nested_read_test tests/scan/test_fsl_nested_read.cpp)
target_compile_features(nano_lance_fsl_nested_read_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_fsl_nested_read_test PRIVATE nanolance)
add_test(NAME nano_lance_fsl_nested_read
         COMMAND $<TARGET_FILE:nano_lance_fsl_nested_read_test>
                 "${PROJECT_SOURCE_DIR}/tests/golden/fsl_struct_v22")
set_tests_properties(nano_lance_fsl_nested_read PROPERTIES LABELS "smoke")
