# fuselance — mount a Lance table with blob.v2 entries as a read-only FUSE filesystem.
#
# fuse3 resolution order:
#   1. System package (libfuse3-dev / fuse3-devel) via pkg-config  — fastest
#   2. Build from source via ExternalProject + meson                — no sudo needed
#      Requires: meson, ninja (both pip-installable without sudo)
#   3. Neither found → skip with a status message

# ── 1. Try system package ────────────────────────────────────────────────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FUSE3 QUIET fuse3)
endif()

# ── 2. Build from source if system package absent ───────────────────────────
if(NOT FUSE3_FOUND)
    find_program(MESON_EXE meson)
    find_program(NINJA_EXE ninja)

    if(NOT MESON_EXE OR NOT NINJA_EXE)
        message(STATUS "nanolance: skipping fuselance — libfuse3 not found and meson/ninja "
                       "not available to build it from source. "
                       "Install libfuse3-dev, or install meson+ninja (pip install meson ninja) "
                       "and re-run cmake.")
        # Unit tests don't need fuse3 — still build them.
        add_executable(fuselance_test_escape_slashes tests/test_escape_slashes.cpp)
        target_compile_features(fuselance_test_escape_slashes PRIVATE cxx_std_20)
        add_test(NAME fuselance_escape_slashes COMMAND fuselance_test_escape_slashes)
        set_tests_properties(fuselance_escape_slashes PROPERTIES LABELS "smoke")
        return()
    endif()

    message(STATUS "nanolance: libfuse3 not found via pkg-config — building from source "
                   "(fuse-3.16.2, static). meson=${MESON_EXE} ninja=${NINJA_EXE}")

    include(ExternalProject)

    set(_FUSE3_INSTALL "${CMAKE_BINARY_DIR}/_deps/fuse3-install")
    set(_FUSE3_BUILD   "${CMAKE_BINARY_DIR}/_deps/fuse3-build")

    # Static library path differs by OS/distro — probe both common locations.
    set(_FUSE3_LIB "${_FUSE3_INSTALL}/lib/libfuse3.a")
    if(NOT EXISTS "${_FUSE3_LIB}")
        set(_FUSE3_LIB "${_FUSE3_INSTALL}/lib64/libfuse3.a")
    endif()

    ExternalProject_Add(fuse3_from_source
        GIT_REPOSITORY  https://github.com/libfuse/libfuse.git
        GIT_TAG         fuse-3.16.2
        GIT_SHALLOW     TRUE
        PREFIX          "${CMAKE_BINARY_DIR}/_deps/fuse3"
        # meson configure
        CONFIGURE_COMMAND
            ${MESON_EXE} setup "${_FUSE3_BUILD}" "<SOURCE_DIR>"
                --prefix=${_FUSE3_INSTALL}
                --default-library=static
                --buildtype=release
                -Dexamples=false
                -Dtests=false
                -Duseroot=false
        # ninja build + install
        BUILD_COMMAND   ${NINJA_EXE} -C "${_FUSE3_BUILD}"
        INSTALL_COMMAND ${NINJA_EXE} -C "${_FUSE3_BUILD}" install
        BUILD_BYPRODUCTS
            "${_FUSE3_INSTALL}/lib/libfuse3.a"
            "${_FUSE3_INSTALL}/lib64/libfuse3.a"
    )

    # Wrap as an imported target so fuselance can just link against it.
    add_library(fuse3_built STATIC IMPORTED GLOBAL)
    # The actual .a path is resolved at install time; use a generator expression
    # so CMake doesn't complain about a non-existent path at configure time.
    set_target_properties(fuse3_built PROPERTIES
        IMPORTED_LOCATION         "${_FUSE3_INSTALL}/lib/libfuse3.a"
        IMPORTED_LOCATION_RELEASE "${_FUSE3_INSTALL}/lib/libfuse3.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_FUSE3_INSTALL}/include"
    )
    add_dependencies(fuse3_built fuse3_from_source)

    set(_FUSE3_TARGET    fuse3_built)
    set(_FUSE3_LINK_OPTS "")

else()
    # System fuse3 — wrap in an interface target for uniform linking below.
    add_library(fuse3_system INTERFACE)
    target_include_directories(fuse3_system INTERFACE ${FUSE3_INCLUDE_DIRS})
    target_link_libraries(fuse3_system      INTERFACE ${FUSE3_LIBRARIES})
    target_link_options(fuse3_system        INTERFACE ${FUSE3_LDFLAGS_OTHER})

    set(_FUSE3_TARGET    fuse3_system)
    set(_FUSE3_LINK_OPTS ${FUSE3_LDFLAGS_OTHER})
    message(STATUS "nanolance: using system fuse3 ${FUSE3_VERSION}")
endif()

# ── fuselance executable ─────────────────────────────────────────────────────
add_executable(fuselance src/fuselance_main.cpp)
# FUSE_USE_VERSION is defined in the source before <fuse3/fuse.h>.
target_compile_features(fuselance PUBLIC cxx_std_20)
target_link_libraries(fuselance PRIVATE nanolance_reader nanolance ${_FUSE3_TARGET})
if(_FUSE3_LINK_OPTS)
    target_link_options(fuselance PRIVATE ${_FUSE3_LINK_OPTS})
endif()

# ── Unit tests (no FUSE dependency) ─────────────────────────────────────────
add_executable(fuselance_test_escape_slashes tests/test_escape_slashes.cpp)
target_compile_features(fuselance_test_escape_slashes PRIVATE cxx_std_20)
add_test(NAME fuselance_escape_slashes COMMAND fuselance_test_escape_slashes)
set_tests_properties(fuselance_escape_slashes PROPERTIES LABELS "smoke")

add_executable(fuselance_test_fh_encoding tests/test_fh_encoding.cpp)
target_compile_features(fuselance_test_fh_encoding PRIVATE cxx_std_20)
add_test(NAME fuselance_fh_encoding COMMAND fuselance_test_fh_encoding)
set_tests_properties(fuselance_fh_encoding PROPERTIES LABELS "smoke")
