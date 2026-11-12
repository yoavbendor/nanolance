// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The path jail (nanolance/path_safety.hpp): safe_join_under joins a manifest-supplied relative path under a
// dataset directory only when it stays there. Bare names and sub-paths join (a "." dropped); an empty path,
// an absolute one and any ".." component are refused, even one that would end back inside. Under a registered
// URI scheme the join keeps the URI's "//" (no lexical normalization). No file is touched: the check is lexical.

#include "nanolance/path_safety.hpp"

#include "nanolance/byte_source.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

/// The joined path, compared as "/"-separated text: on Windows std::filesystem joins with "\" and keeps the
/// separators of the inputs, so a path's native string is not what these checks mean.
bool joins_to(const std::optional<std::filesystem::path>& p, const std::string& expected) {
    return p.has_value() && p->generic_string() == expected;
}

}  // namespace

int main() try {
    std::cout << "running nano_lance_path_safety\n" << std::flush;
    namespace fs = std::filesystem;
    using nano_lance::safe_join_under;
    const fs::path base = "/data/ds.lance/data";

    // Safe paths.
    require(joins_to(safe_join_under(base, "0a1b2c.lance"), "/data/ds.lance/data/0a1b2c.lance"), "a bare file name");
    require(joins_to(safe_join_under(base, "sub/dir/file.blob"), "/data/ds.lance/data/sub/dir/file.blob"),
            "a sub-path");
    require(joins_to(safe_join_under(base, "./file.lance"), "/data/ds.lance/data/file.lance"),
            "a leading . is dropped");
    require(joins_to(safe_join_under("/data/ds.lance/./data/", "f.lance"), "/data/ds.lance/data/f.lance"),
            "the base is normalized");
    require(joins_to(safe_join_under("relative/base", "f.lance"), "relative/base/f.lance"), "a relative base");

    // Refused paths.
    require(!safe_join_under(base, "").has_value(), "an empty path");
    require(!safe_join_under(base, "/etc/passwd").has_value(), "an absolute path");
    require(!safe_join_under(base, "../other.lance").has_value(), "a leading ..");
    require(!safe_join_under(base, "a/../../x").has_value(), "a .. that walks out");
    require(!safe_join_under(base, "a/../b").has_value(), "a .. even when it ends inside");
    require(!safe_join_under(base, "..").has_value(), "just ..");
    // A backslash is a separator on Windows, where these are ".." components; a POSIX name carrying one is refused
    // too, so the answer for "..\" is the same wherever the dataset is read.
    require(!safe_join_under(base, "..\\other.lance").has_value(), "a backslash-separated ..");
    require(!safe_join_under(base, "a\\..\\..\\x").has_value(), "a backslash .. that walks out");
#ifdef _WIN32
    // Drive letters, UNC roots, alternate data streams and reserved device names open something other than
    // the file the name says.
    require(!safe_join_under(base, "C:x").has_value(), "a drive-relative name");
    require(!safe_join_under(base, "C:\\x").has_value(), "a drive-absolute name");
    require(!safe_join_under(base, "\\\\server\\share\\x").has_value(), "a UNC name");
    require(!safe_join_under(base, "a:stream").has_value(), "an alternate data stream");
    require(!safe_join_under(base, "CON").has_value(), "a reserved device name");
    require(!safe_join_under(base, "nul.lance").has_value(), "a reserved device name with an extension");
    require(!safe_join_under(base, "lpt1 .txt").has_value(), "a reserved device name with a space before the dot");
#endif

    // A URI base under a registered scheme: joined as is, with "/" separators.
    {
        nano_lance::io::register_source_factory(
            "jailtest", [](const std::string&, std::string& error) -> std::unique_ptr<nano_lance::io::ByteSource> {
                error = "not used";
                return nullptr;
            });
        const auto p = safe_join_under("jailtest:///bucket/ds.lance/data", "f.lance");
        require(p.has_value() && p->string() == "jailtest:///bucket/ds.lance/data/f.lance",
                "a URI keeps its // (" + (p ? p->string() : std::string("refused")) + ")");
        require(!safe_join_under("jailtest:///bucket/ds.lance/data", "../x").has_value(), "a URI refuses .. too");
        require(!safe_join_under("jailtest:///bucket/ds.lance/data", "/x").has_value(), "and an absolute path");
        nano_lance::io::register_source_factory("jailtest", {});
    }

    std::cout << "path_safety: all checks passed\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: uncaught exception: " << e.what() << "\n";
    return 1;
}
