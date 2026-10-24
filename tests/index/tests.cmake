# Tests of the index layer added after the move into tests/index/ (src/index/README.md lists what they cover).
# Included from the root CMakeLists.txt; paths are from the repository root. Register each test as the others
# are: add_executable(nano_lance_<name>_test tests/index/test_<name>.cpp), add_test(NAME nano_lance_<name> ...),
# LABELS "smoke" for a fast one. The layer label is added for you.

add_executable(nano_lance_fts_document_text_test tests/index/test_fts_document_text.cpp)
target_compile_features(nano_lance_fts_document_text_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_fts_document_text_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_fts_document_text_test PRIVATE nanolance)
add_test(NAME nano_lance_fts_document_text COMMAND $<TARGET_FILE:nano_lance_fts_document_text_test>)
set_tests_properties(nano_lance_fts_document_text PROPERTIES LABELS "smoke")

add_executable(nano_lance_fts_search_test tests/index/test_fts_search.cpp)
target_compile_features(nano_lance_fts_search_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_fts_search_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_fts_search_test PRIVATE nanolance)
add_test(NAME nano_lance_fts_search COMMAND $<TARGET_FILE:nano_lance_fts_search_test>)
set_tests_properties(nano_lance_fts_search PROPERTIES LABELS "smoke")

add_executable(nano_lance_inverted_segment_test tests/index/test_inverted_segment.cpp)
target_compile_features(nano_lance_inverted_segment_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_inverted_segment_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_inverted_segment_test PRIVATE nanolance)
add_test(NAME nano_lance_inverted_segment COMMAND $<TARGET_FILE:nano_lance_inverted_segment_test>)
set_tests_properties(nano_lance_inverted_segment PROPERTIES LABELS "smoke")

add_executable(nano_lance_fts_jieba_test tests/index/test_fts_jieba.cpp)
target_compile_features(nano_lance_fts_jieba_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_fts_jieba_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_fts_jieba_test PRIVATE nanolance)
add_test(NAME nano_lance_fts_jieba COMMAND $<TARGET_FILE:nano_lance_fts_jieba_test>)
set_tests_properties(nano_lance_fts_jieba PROPERTIES LABELS "smoke")

add_executable(nano_lance_fts_lindera_test tests/index/test_fts_lindera.cpp)
target_compile_features(nano_lance_fts_lindera_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_fts_lindera_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_fts_lindera_test PRIVATE nanolance)
add_test(NAME nano_lance_fts_lindera COMMAND $<TARGET_FILE:nano_lance_fts_lindera_test>)
set_tests_properties(nano_lance_fts_lindera PROPERTIES LABELS "smoke")

add_executable(nano_lance_scalar_segment_test tests/index/test_scalar_segment.cpp)
target_compile_features(nano_lance_scalar_segment_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_scalar_segment_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_scalar_segment_test PRIVATE nanolance)
add_test(NAME nano_lance_scalar_segment COMMAND $<TARGET_FILE:nano_lance_scalar_segment_test>)
set_tests_properties(nano_lance_scalar_segment PROPERTIES LABELS "smoke")

add_executable(nano_lance_vector_search_test tests/index/test_vector_search.cpp)
target_compile_features(nano_lance_vector_search_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_vector_search_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_vector_search_test PRIVATE nanolance)
add_test(NAME nano_lance_vector_search COMMAND $<TARGET_FILE:nano_lance_vector_search_test>)
set_tests_properties(nano_lance_vector_search PROPERTIES LABELS "smoke")

add_executable(nano_lance_vector_segment_test tests/index/test_vector_segment.cpp)
target_compile_features(nano_lance_vector_segment_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_vector_segment_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_vector_segment_test PRIVATE nanolance)
add_test(NAME nano_lance_vector_segment COMMAND $<TARGET_FILE:nano_lance_vector_segment_test>)
set_tests_properties(nano_lance_vector_segment PROPERTIES LABELS "smoke")

add_executable(nano_lance_hnsw_build_test tests/index/test_hnsw_build.cpp)
target_compile_features(nano_lance_hnsw_build_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_hnsw_build_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_hnsw_build_test PRIVATE nanolance)
add_test(NAME nano_lance_hnsw_build COMMAND $<TARGET_FILE:nano_lance_hnsw_build_test>)
set_tests_properties(nano_lance_hnsw_build PROPERTIES LABELS "smoke")

add_executable(nano_lance_rabitq_build_test tests/index/test_rabitq_build.cpp)
target_compile_features(nano_lance_rabitq_build_test PRIVATE cxx_std_20)
target_include_directories(nano_lance_rabitq_build_test PRIVATE ${NANOLANCE_PRIVATE_INCLUDE})
target_link_libraries(nano_lance_rabitq_build_test PRIVATE nanolance)
add_test(NAME nano_lance_rabitq_build COMMAND $<TARGET_FILE:nano_lance_rabitq_build_test>)
set_tests_properties(nano_lance_rabitq_build PROPERTIES LABELS "smoke")
