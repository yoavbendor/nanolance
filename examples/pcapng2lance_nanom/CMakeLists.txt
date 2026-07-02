# pcapng -> Lance example, built on nanom (the parser-combinator sibling of examples/pcapng2lance).
#
# This directory is OPT-IN and OFF by default (see NANOLANCE_BUILD_PCAPNG2LANCE_NANOM in the top-level
# CMakeLists). nanom is referenced only here — a plain nanolance build, and the default `pcapng2lance`
# example build, never touch it. The point of the example is to benchmark nanom's scan+parse+tabulate
# path head-to-head against nanotins on the exact same L1 Lance output; nothing in nanolance depends on
# nanom, so keeping it non-mandatory keeps the core build free of the extra submodule.

# nanom is header-only and vendored as a git submodule under extern/nanom.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/nanom/CMakeLists.txt")
    message(FATAL_ERROR
        "examples/pcapng2lance_nanom/extern/nanom is empty — run: git submodule update --init --recursive")
endif()
# Pull in the nanom::nanom interface target. It builds its own tests/examples only when it is the top-level
# project, so add_subdirectory here contributes just the header target.
add_subdirectory(extern/nanom EXCLUDE_FROM_ALL)

add_executable(pcapng2lance_nanom src/pcapng2lance_nanom_main.cpp)
target_compile_features(pcapng2lance_nanom PRIVATE cxx_std_23)  # nanom requires C++23
# The nanom pcap/pcapng scanner (nm_pcap.hpp) lives in nanom's parity-example dir, not its include tree.
target_include_directories(pcapng2lance_nanom PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/nanom/examples/nanotins_parity")
target_link_libraries(pcapng2lance_nanom PRIVATE nanom::nanom nanolance)

if(NANOLANCE_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        # Stock-Lance (pylance) interop + on-disk offset verification, plus a byte-for-byte cross-check that
        # this nanom converter's packets.lance equals the nanotins pcapng2lance output when that target is
        # also built. Skips (CTest 77) if pylance is absent.
        set(_p2l_nanom_deps $<TARGET_FILE:pcapng2lance_nanom>)
        if(TARGET pcapng2lance)
            list(APPEND _p2l_nanom_deps $<TARGET_FILE:pcapng2lance>)
        endif()
        add_test(NAME pcapng2lance_nanom_interop
            COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pcapng2lance_nanom.py"
                    ${_p2l_nanom_deps})
        set_tests_properties(pcapng2lance_nanom_interop PROPERTIES LABELS "interop" SKIP_RETURN_CODE 77)
    endif()
endif()
