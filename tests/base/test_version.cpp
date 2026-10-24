// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The library version (nanolance/version.hpp): library_version() is the version the build was configured
// with (nanolance/version.h, from CMakeLists.txt), "MAJOR.MINOR.PATCH" with each part matching its macro.

#include "nanolance/version.hpp"

#include "nanolance/version.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

}  // namespace

int main() {
    const char* v = nanolance::library_version();
    require(v != nullptr && *v != '\0', "a version string");
    require(std::strcmp(v, NANOLANCE_VERSION_STRING) == 0, "the configured version");
    const std::string expected = std::to_string(NANOLANCE_VERSION_MAJOR) + "." + std::to_string(NANOLANCE_VERSION_MINOR) +
                                 "." + std::to_string(NANOLANCE_VERSION_PATCH);
    require(expected == v, "MAJOR.MINOR.PATCH from the macros");
    require(nanolance::library_version() == v, "the same string on every call");
    std::cout << "version: " << v << "\n";
    return 0;
}
