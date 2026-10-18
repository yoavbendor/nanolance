# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
#
# Where nanolance's build fetches its git dependencies from. By default GitHub; behind a firewall,
# point everything at a mirror with one setting:
#
#   cmake -B build -DNANOLANCE_GIT_BASE=https://gitlab.example.com/mirrors
#
# (or the environment variable NANOLANCE_GIT_BASE, which also reaches the Python build). Each
# dependency is then <base>/<owner>/<repo>.git, the same owner/repo path as on GitHub. A mirror laid
# out differently sets the one URL that differs, e.g. -DNANOLANCE_GIT_URL_ZSTD=https://.../zstd.git.
# The pinned commits and tags are in the FetchContent_Declare calls; the mirror must carry them.
# docs/BUILDING_BEHIND_A_FIREWALL.md has the list and the commands.

include_guard(GLOBAL)

if(DEFINED ENV{NANOLANCE_GIT_BASE} AND NOT "$ENV{NANOLANCE_GIT_BASE}" STREQUAL "")
    set(_nanolance_git_base_default "$ENV{NANOLANCE_GIT_BASE}")
else()
    set(_nanolance_git_base_default "https://github.com")
endif()
set(NANOLANCE_GIT_BASE "${_nanolance_git_base_default}" CACHE STRING
    "Base URL of the git server nanolance's dependencies are cloned from (<base>/<owner>/<repo>.git)")
string(REGEX REPLACE "/+$" "" _nanolance_git_base "${NANOLANCE_GIT_BASE}")

# nanolance_git_url(<VAR> <owner/repo>): sets <VAR> to the dependency's URL: its own override when given
# (-D<VAR>=... or an environment variable of that name), else NANOLANCE_GIT_BASE/<owner/repo>.git. The
# override's cache entry stays empty unless set, so changing NANOLANCE_GIT_BASE later moves every
# dependency that has none.
function(nanolance_git_url var path)
    set(${var} "" CACHE STRING "git URL of ${path}; empty: NANOLANCE_GIT_BASE/${path}.git")
    if(NOT "${${var}}" STREQUAL "")
        set(_url "${${var}}")
    elseif(DEFINED ENV{${var}} AND NOT "$ENV{${var}}" STREQUAL "")
        set(_url "$ENV{${var}}")
    else()
        set(_url "${_nanolance_git_base}/${path}.git")
    endif()
    set(${var} "${_url}" PARENT_SCOPE)
endfunction()

nanolance_git_url(NANOLANCE_GIT_URL_NANOARROW    apache/arrow-nanoarrow)
nanolance_git_url(NANOLANCE_GIT_URL_ZSTD         facebook/zstd)
nanolance_git_url(NANOLANCE_GIT_URL_CLI11        CLIUtils/CLI11)
nanolance_git_url(NANOLANCE_GIT_URL_NANOM        yoavbendor/nanom)
nanolance_git_url(NANOLANCE_GIT_URL_NANOS3READER yoavbendor/nanos3reader)
nanolance_git_url(NANOLANCE_GIT_URL_LIBFUSE      libfuse/libfuse)
