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
    set(_FUSE3_LIB "${_FUSE3_INSTALL}/lib/libfuse3.a")

    ExternalProject_Add(fuse3_from_source
        GIT_REPOSITORY  https://github.com/libfuse/libfuse.git
        GIT_TAG         fuse-3.16.2
        GIT_SHALLOW     TRUE
        PREFIX          "${CMAKE_BINARY_DIR}/_deps/fuse3"
        # meson configure.
        #   --libdir=lib pins the install libdir so libfuse3.a lands in a predictable
        #     ${prefix}/lib (meson otherwise defaults to a multiarch dir like
        #     lib/x86_64-linux-gnu on Debian/Ubuntu, or lib64 on Fedora).
        #   -Dutils=false: we only link the static lib. The utils (fusermount3) must be
        #     setuid-root to mount as a normal user, which a non-root install can't do
        #     anyway; at runtime fuselance uses the host's fusermount3 from PATH.
        #   -Dinitscriptdir=/-Dudevrulesdir: keep install writes inside our prefix so a
        #     non-root `ninja install` never touches /etc or system udev dirs.
        CONFIGURE_COMMAND
            ${MESON_EXE} setup "${_FUSE3_BUILD}" "<SOURCE_DIR>"
                --prefix=${_FUSE3_INSTALL}
                --libdir=lib
                --default-library=static
                --buildtype=release
                -Dexamples=false
                -Dtests=false
                -Dutils=false
                -Duseroot=false
                -Dudevrulesdir=${_FUSE3_INSTALL}/lib/udev/rules.d
                -Dinitscriptdir=
        # ninja build + install
        BUILD_COMMAND   ${NINJA_EXE} -C "${_FUSE3_BUILD}"
        INSTALL_COMMAND ${NINJA_EXE} -C "${_FUSE3_BUILD}" install
        BUILD_BYPRODUCTS "${_FUSE3_LIB}"
    )

    # Wrap as an imported target so fuselance can just link against it. The install tree
    # doesn't exist until build time, so pre-create the dirs — CMake errors out at
    # configure time on an imported target whose INTERFACE_INCLUDE_DIRECTORIES path is
    # missing.
    add_library(fuse3_built STATIC IMPORTED GLOBAL)
    file(MAKE_DIRECTORY "${_FUSE3_INSTALL}/include")
    file(MAKE_DIRECTORY "${_FUSE3_INSTALL}/lib")
    set_target_properties(fuse3_built PROPERTIES
        IMPORTED_LOCATION         "${_FUSE3_LIB}"
        IMPORTED_LOCATION_RELEASE "${_FUSE3_LIB}"
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

# ── Version header ───────────────────────────────────────────────────────────
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _FUSELANCE_GIT_DESC
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
else()
    set(_FUSELANCE_GIT_DESC "unknown")
endif()
string(TIMESTAMP _FUSELANCE_BUILD_DATE "%Y-%m-%d" UTC)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fuselance_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fuselance_version.h"
    @ONLY
)

# ── fuselance executable ─────────────────────────────────────────────────────
add_executable(fuselance src/fuselance_main.cpp)
# FUSE_USE_VERSION is defined in the source before <fuse3/fuse.h>.
target_compile_features(fuselance PUBLIC cxx_std_20)
target_include_directories(fuselance PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
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
