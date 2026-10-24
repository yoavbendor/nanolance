# Tests of the table layer added after the move into tests/table/ (src/table/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/table/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.

add_executable(nano_lance_dataset_transaction_test tests/table/test_dataset_transaction.cpp)
target_compile_features(nano_lance_dataset_transaction_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_dataset_transaction_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_dataset_transaction_test PRIVATE nanolance)
add_test(NAME nano_lance_dataset_transaction COMMAND $<TARGET_FILE:nano_lance_dataset_transaction_test>)
set_tests_properties(nano_lance_dataset_transaction PROPERTIES LABELS "smoke")

add_executable(nano_lance_dataset_transaction_ops_test tests/table/test_dataset_transaction_ops.cpp)
target_compile_features(nano_lance_dataset_transaction_ops_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_dataset_transaction_ops_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_dataset_transaction_ops_test PRIVATE nanolance)
add_test(NAME nano_lance_dataset_transaction_ops COMMAND $<TARGET_FILE:nano_lance_dataset_transaction_ops_test>)
set_tests_properties(nano_lance_dataset_transaction_ops PROPERTIES LABELS "smoke")
