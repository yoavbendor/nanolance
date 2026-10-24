# Tests of the encoding layer added after the move into tests/encoding/ (src/encoding/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/encoding/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.

add_executable(nano_lance_lance_descriptors_test tests/encoding/test_lance_descriptors.cpp)
target_compile_features(nano_lance_lance_descriptors_test PRIVATE cxx_std_23)
target_link_libraries(nano_lance_lance_descriptors_test PRIVATE nanolance nanolance_nanom)
target_include_directories(nano_lance_lance_descriptors_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
add_test(NAME nano_lance_lance_descriptors COMMAND $<TARGET_FILE:nano_lance_lance_descriptors_test>)
set_tests_properties(nano_lance_lance_descriptors PROPERTIES LABELS "smoke")

add_executable(nano_lance_encodings_io_test tests/encoding/test_encodings_io.cpp)
target_compile_features(nano_lance_encodings_io_test PRIVATE cxx_std_23)
target_link_libraries(nano_lance_encodings_io_test PRIVATE nanolance nanolance_nanom)
target_include_directories(nano_lance_encodings_io_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
add_test(NAME nano_lance_encodings_io COMMAND $<TARGET_FILE:nano_lance_encodings_io_test>)
set_tests_properties(nano_lance_encodings_io PROPERTIES LABELS "smoke")

add_executable(nano_lance_lance_v20_decoder_test tests/encoding/test_lance_v20_decoder.cpp)
target_compile_features(nano_lance_lance_v20_decoder_test PRIVATE cxx_std_20)
target_link_libraries(nano_lance_lance_v20_decoder_test PRIVATE nanolance)
add_test(NAME nano_lance_lance_v20_decoder COMMAND $<TARGET_FILE:nano_lance_lance_v20_decoder_test>)
set_tests_properties(nano_lance_lance_v20_decoder PROPERTIES LABELS "smoke")
