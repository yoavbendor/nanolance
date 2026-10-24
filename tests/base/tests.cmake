# Tests of the base layer added after the move into tests/base/ (src/base/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/base/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.

add_executable(nano_lance_bool_bitpack_test tests/base/test_bool_bitpack.cpp)
target_compile_features(nano_lance_bool_bitpack_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_bool_bitpack_test PRIVATE nanolance)
add_test(NAME nano_lance_bool_bitpack COMMAND $<TARGET_FILE:nano_lance_bool_bitpack_test>)
set_tests_properties(nano_lance_bool_bitpack PROPERTIES LABELS "smoke")

add_executable(nano_lance_float16_test tests/base/test_float16.cpp)
target_compile_features(nano_lance_float16_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_float16_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_float16_test PRIVATE nanolance)
add_test(NAME nano_lance_float16 COMMAND $<TARGET_FILE:nano_lance_float16_test>)
set_tests_properties(nano_lance_float16 PROPERTIES LABELS "smoke")

add_executable(nano_lance_fts_json_test tests/base/test_fts_json.cpp)
target_compile_features(nano_lance_fts_json_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_fts_json_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_fts_json_test PRIVATE nanolance)
add_test(NAME nano_lance_fts_json COMMAND $<TARGET_FILE:nano_lance_fts_json_test>)
set_tests_properties(nano_lance_fts_json PROPERTIES LABELS "smoke")

add_executable(nano_lance_proto_wire_test tests/base/test_proto_wire.cpp)
target_compile_features(nano_lance_proto_wire_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_proto_wire_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_proto_wire_test PRIVATE nanolance)
add_test(NAME nano_lance_proto_wire COMMAND $<TARGET_FILE:nano_lance_proto_wire_test>)
set_tests_properties(nano_lance_proto_wire PROPERTIES LABELS "smoke")

add_executable(nano_lance_version_test tests/base/test_version.cpp)
target_compile_features(nano_lance_version_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_version_test PRIVATE nanolance)
add_test(NAME nano_lance_version COMMAND $<TARGET_FILE:nano_lance_version_test>)
set_tests_properties(nano_lance_version PROPERTIES LABELS "smoke")
