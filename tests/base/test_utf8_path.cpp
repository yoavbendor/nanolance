// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// base/utf8_path.hpp: UTF-8 names in and out of std::filesystem::path, byte for byte, on every platform.

#include "base/utf8_path.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

int main() {
    std::printf("running nano_lance_utf8_path\n");
    std::fflush(stdout);
    namespace u8 = nano_lance::utf8;
    const std::string cafe = "caf\xC3\xA9";
    const std::string nihon = "\xE6\x97\xA5\xE6\x9C\xAC";

    check(u8::string_of(u8::from_utf8(cafe)) == cafe, "a non-ASCII name round trips");
    check(u8::string_of(u8::from_utf8(nihon)) == nihon, "a CJK name round trips");
    check(u8::string_of(u8::from_utf8("")).empty(), "the empty name round trips");
    check(u8::generic_of(u8::from_utf8("a") / u8::from_utf8(cafe)) == "a/" + cafe, "generic_of joins with '/'");
    check(u8::from_utf8(nihon).filename() == u8::from_utf8(nihon), "the name is one component");

    // A file created through a UTF-8 path is listed back under the same UTF-8 name.
    const auto dir = std::filesystem::temp_directory_path() / "nanolance_utf8_path_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / u8::from_utf8(cafe)) << "x"; }
    bool listed = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        listed = listed || u8::string_of(entry.path().filename()) == cafe;
    }
    check(listed, "a file named by a UTF-8 path lists back under that name");
    std::filesystem::remove_all(dir);

    if (failures == 0) {
        std::printf("ok\n");
    }
    return failures == 0 ? 0 : 1;
}
