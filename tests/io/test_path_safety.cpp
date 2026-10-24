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
#include <filesystem>
#include <iostream>
#include <memory>
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
    namespace fs = std::filesystem;
    using nano_lance::safe_join_under;
    const fs::path base = "/data/ds.lance/data";

    // Safe paths.
    {
        const auto p = safe_join_under(base, "0a1b2c.lance");
        require(p.has_value() && *p == fs::path("/data/ds.lance/data/0a1b2c.lance"), "a bare file name");
        const auto q = safe_join_under(base, "sub/dir/file.blob");
        require(q.has_value() && *q == fs::path("/data/ds.lance/data/sub/dir/file.blob"), "a sub-path");
        const auto r = safe_join_under(base, "./file.lance");
        require(r.has_value() && *r == fs::path("/data/ds.lance/data/file.lance"), "a leading . is dropped");
        const auto s = safe_join_under("/data/ds.lance/./data/", "f.lance");
        require(s.has_value() && *s == fs::path("/data/ds.lance/data/f.lance"), "the base is normalized");
        const auto t = safe_join_under("relative/base", "f.lance");
        require(t.has_value() && *t == fs::path("relative/base/f.lance"), "a relative base");
    }

    // Refused paths.
    require(!safe_join_under(base, "").has_value(), "an empty path");
    require(!safe_join_under(base, "/etc/passwd").has_value(), "an absolute path");
    require(!safe_join_under(base, "../other.lance").has_value(), "a leading ..");
    require(!safe_join_under(base, "a/../../x").has_value(), "a .. that walks out");
    require(!safe_join_under(base, "a/../b").has_value(), "a .. even when it ends inside");
    require(!safe_join_under(base, "..").has_value(), "just ..");

    // A URI base under a registered scheme: joined as is.
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
}
