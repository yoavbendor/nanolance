# One small program per layer (examples/layers/<layer>.cpp), built with the tests and run by ctest as
# nano_lance_example_<layer>. Included from the root CMakeLists.txt, in the same scope as tests/<layer>/tests.cmake, so
# only when NANOLANCE_BUILD_TESTS is on. They are not tied to NANOLANCE_BUILD_EXAMPLES (that option is pcapng2lance).
# Each example checks its own results and returns nonzero with a message on a mismatch. The layer label is added by the
# root CMakeLists.txt from the first source, examples/layers/<layer>.cpp.

foreach(_nl_example IN ITEMS base io arrow encoding file table expr scan index dataset api)
    add_executable(nano_lance_example_${_nl_example}_test examples/layers/${_nl_example}.cpp)
    target_compile_features(nano_lance_example_${_nl_example}_test PRIVATE cxx_std_20)
    target_link_libraries(nano_lance_example_${_nl_example}_test PRIVATE nanolance)
    add_test(NAME nano_lance_example_${_nl_example}
             COMMAND $<TARGET_FILE:nano_lance_example_${_nl_example}_test>)
    set_tests_properties(nano_lance_example_${_nl_example} PROPERTIES LABELS "example")
endforeach()

# The encoding example runs one same-layer module that has no public header (values/bitpack, which the file layer
# calls), so it reads it from src/ as the layer's tests do. Nothing else in the examples uses a private header.
target_include_directories(nano_lance_example_encoding_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
