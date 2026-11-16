# ndjson2lance -- a mixed stream of JSON-lines records in, one clean Lance table per record shape out.
# See README.md. Built only with NANOLANCE_BUILD_NDJSON2LANCE=ON (the top-level CMakeLists.txt).

include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NanolanceDeps.cmake")

# simdjson_arrow brings simdjson 5.x through its own FetchContent. Its tests, benchmarks and examples
# are not built here.
set(SIMDJSON_ARROW_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDJSON_ARROW_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SIMDJSON_ARROW_EXAMPLES OFF CACHE BOOL "" FORCE)
nanolance_git_url(NANOLANCE_GIT_URL_SIMDJSON_ARROW "simdjson/simdjson_arrow")
FetchContent_Declare(simdjson_arrow
    GIT_REPOSITORY ${NANOLANCE_GIT_URL_SIMDJSON_ARROW}
    GIT_TAG        v0.1.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(simdjson_arrow)

# CLI11 is fetched by the top-level build when NANOLANCE_BUILD_TOOLS is ON (its default).
if(NOT TARGET CLI11::CLI11)
    message(FATAL_ERROR "ndjson2lance: CLI11::CLI11 missing; build with -DNANOLANCE_BUILD_TOOLS=ON")
endif()

add_executable(ndjson2lance src/ndjson2lance.cpp)
target_compile_features(ndjson2lance PRIVATE cxx_std_20)
target_compile_options(ndjson2lance PRIVATE -Wall -Wextra ${NANOLANCE_WARNING_FLAGS})
target_link_libraries(ndjson2lance PRIVATE nanolance simdjson_arrow CLI11::CLI11)
