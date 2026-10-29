# Tests of the arrow layer added after the move into tests/arrow/ (src/arrow/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/arrow/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.

add_executable(nano_lance_column_values_test tests/arrow/test_column_values.cpp)
target_compile_features(nano_lance_column_values_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_column_values_test PRIVATE nanolance)
add_test(NAME nano_lance_column_values COMMAND $<TARGET_FILE:nano_lance_column_values_test>)
set_tests_properties(nano_lance_column_values PROPERTIES LABELS "smoke")

add_executable(nano_lance_arrow_views_test tests/arrow/test_arrow_views.cpp)
target_compile_features(nano_lance_arrow_views_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_arrow_views_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_arrow_views_test PRIVATE nanolance)
add_test(NAME nano_lance_arrow_views COMMAND $<TARGET_FILE:nano_lance_arrow_views_test>)
set_tests_properties(nano_lance_arrow_views PROPERTIES LABELS "smoke")
