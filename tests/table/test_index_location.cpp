// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Where an index's files go, and the names its metadata carries: a field addressed by its dotted path
// (a nested field is found under its parent, never by its bare name), the path of a JSON index's
// details, and the creation and removal of an index's directory on local disk. A build that cannot make
// its directory must say so before it writes anything into it.

#include "lance_minimal.pb.hpp"
#include "table/index_location.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace pb = nano_lance::pb;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

pb::Field field(const std::string& name, std::int32_t id, std::int32_t parent) {
    pb::Field f;
    f.name = name;
    f.id = id;
    f.parent_id = parent;
    return f;
}

}  // namespace

void require(bool ok, const std::string& msg, const std::string& detail) {
    require(ok, msg + detail);
}

int main() {
    // A schema with a top-level struct "s" holding "x" (itself holding "z") and "y", and a top-level "id".
    pb::Manifest manifest;
    manifest.fields = {
        field("id", 0, -1),
        field("s", 1, -1),
        field("x", 2, 1),
        field("y", 3, 1),
        field("z", 4, 2),
    };

    {
        std::vector<std::string> parts;
        const auto* found = nano_lance::index_location::find_field(manifest, "id", parts);
        require(found != nullptr && found->id == 0, "a top-level field is found by its name");
        require(parts == std::vector<std::string>({"id"}), "its path is its own name");
    }
    {
        std::vector<std::string> parts;
        const auto* found = nano_lance::index_location::find_field(manifest, "s.y", parts);
        require(found != nullptr && found->id == 3, "a nested field is found through its parent");
        require(parts == std::vector<std::string>({"s", "y"}), "its path lists every part");
    }
    {
        std::vector<std::string> parts;
        const auto* found = nano_lance::index_location::find_field(manifest, "s.x.z", parts);
        require(found != nullptr && found->id == 4, "a field three levels deep is found");
        require(parts == std::vector<std::string>({"s", "x", "z"}), "the deep path lists every part");
    }
    {
        std::vector<std::string> parts;
        require(nano_lance::index_location::find_field(manifest, "s.nope", parts) == nullptr,
                "a missing nested name is not found");
        require(nano_lance::index_location::find_field(manifest, "missing", parts) == nullptr,
                "a missing top-level name is not found");
        require(nano_lance::index_location::find_field(manifest, "y", parts) == nullptr,
                "a nested field is not a top-level name");
        require(nano_lance::index_location::find_field(manifest, "x.z", parts) == nullptr,
                "a path must start at a top-level field");
    }

    // The path of a JsonIndexDetails: field 1, a string. Absent, it is empty; malformed, refused.
    {
        const std::string json = "/indices/json/a.json";
        std::vector<std::uint8_t> details = {0x0A, static_cast<std::uint8_t>(json.size())};
        details.insert(details.end(), json.begin(), json.end());
        std::string path;
        require(nano_lance::index_location::json_index_path(details, path), "a JSON index's details decode");
        require(path == json, "the path is field 1 of the details");

        require(nano_lance::index_location::json_index_path({}, path) && path.empty(),
                "details with no path name none");

        const std::vector<std::uint8_t> truncated = {0x0A, 0x09, 'a'};  // claims 9 bytes, has 1
        require(!nano_lance::index_location::json_index_path(truncated, path), "truncated details are refused");
    }

    // The directory of an index: created with its parents, idempotent, refused under a file, removed whole.
    {
        const auto root = std::filesystem::temp_directory_path() / "nano_lance_index_location_test";
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root);

        const auto dir = root / "_indices" / "0123-uuid";
        std::string error;
        require(nano_lance::index_location::create_index_dir(dir, error), "create: ", error);
        require(std::filesystem::is_directory(dir), "the index directory exists, parents included");
        require(nano_lance::index_location::create_index_dir(dir, error), "creating it again is fine: ", error);

        {
            std::ofstream out(dir / "part.bin", std::ios::binary);
            out << "payload";
        }
        nano_lance::index_location::remove_index_dir(dir);
        require(!std::filesystem::exists(dir), "removing the index removes its directory");
        require(std::filesystem::is_directory(root / "_indices"), "its parent is left alone");

        nano_lance::index_location::remove_index_dir(dir);  // already gone: nothing to do, no failure

        const auto blocker = root / "blocker";
        {
            std::ofstream out(blocker, std::ios::binary);
            out << "a file, not a directory";
        }
        require(!nano_lance::index_location::create_index_dir(blocker / "under", error),
                "a directory cannot be made under a file");
        require(error.rfind("cannot create ", 0) == 0, "the error names the directory: ", error);

        std::filesystem::remove_all(root, ec);
    }

    std::cout << "index location: dotted field paths, JSON index path, index directory create/remove OK\n";
    return 0;
}
