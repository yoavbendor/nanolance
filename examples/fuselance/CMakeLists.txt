# fuselance — mount a Lance table with blob.v2 entries as a read-only FUSE filesystem.
#
# System prerequisite: libfuse3-dev (Debian/Ubuntu) or fuse3-devel (RPM).
# If fuse3 is not found, this example skips itself (status message) rather than failing the build.

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FUSE3 fuse3)
endif()
if(NOT FUSE3_FOUND)
    message(STATUS "nanolance: skipping fuselance example — libfuse3 not found (install libfuse3-dev)")
    return()
endif()

add_executable(fuselance src/fuselance_main.cpp)
# FUSE_USE_VERSION is defined in the source before <fuse3/fuse.h>; keep it there as the single source of truth.
target_compile_features(fuselance PUBLIC cxx_std_20)
target_include_directories(fuselance PRIVATE ${FUSE3_INCLUDE_DIRS})
target_link_libraries(fuselance PRIVATE nanolance_reader nanolance ${FUSE3_LIBRARIES})
target_link_options(fuselance PRIVATE ${FUSE3_LDFLAGS_OTHER})
