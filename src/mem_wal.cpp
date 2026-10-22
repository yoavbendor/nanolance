// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/mem_wal.hpp"

#include "index_files.hpp"
#include "lance_minimal.pb.hpp"
#include "nanolance/dataset_transaction.hpp"
#include "nanolance/manifest_reader.hpp"
#include "proto_wire.hpp"

#include <array>
#include <chrono>
#include <cstring>

namespace nano_lance {
namespace {

constexpr const char* kMemWalDetailsTypeUrl = "/lance.table.MemWalIndexDetails";
constexpr std::uint32_t kCreateIndexOperation = 103;  // lance.table.Transaction.CreateIndex

std::vector<std::uint8_t> bytes_of(const std::string& text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

/// IndexMetadata as Lance writes the MemWAL index: a name, a dataset version, the details in an Any,
/// index_version 0 (a proto3 optional, so written even at zero) and the creation time. No fields, no
/// fragment bitmap and no files: the index covers the table, not a column or a set of fragments.
std::vector<std::uint8_t> encode_mem_wal_index(const std::array<std::uint8_t, 16>& uuid, std::uint64_t dataset_version,
                                               const std::vector<std::uint8_t>& details, std::uint64_t created_at) {
    std::vector<std::uint8_t> uuid_message;  // UUID { bytes uuid = 1 }
    proto_wire::put_bytes_field(uuid_message, 1, std::vector<std::uint8_t>(uuid.begin(), uuid.end()));
    std::vector<std::uint8_t> any;  // google.protobuf.Any { type_url = 1, value = 2 }
    proto_wire::put_bytes_field(any, 1, bytes_of(kMemWalDetailsTypeUrl));
    proto_wire::put_bytes_field(any, 2, details);
    std::vector<std::uint8_t> out;
    proto_wire::put_bytes_field(out, 1, uuid_message);
    proto_wire::put_bytes_field(out, 3, bytes_of(kMemWalIndexName));
    proto_wire::put_u64_field(out, 4, dataset_version);
    proto_wire::put_bytes_field(out, 6, any);
    proto_wire::put_key(out, 7, 0);  // index_version, present even when 0
    proto_wire::put_varint(out, 0);
    proto_wire::put_u64_field(out, 8, created_at);
    return out;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

bool dataset_mem_wal_details(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                             std::optional<std::vector<std::uint8_t>>& out, std::string& error) {
    error.clear();
    out.reset();
    pb::Manifest manifest;
    if (has_version) {
        if (!load_manifest_version(dataset_path, version, manifest, error)) {
            return false;
        }
    } else if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        return false;
    }
    for (const auto& index : manifest.indices) {
        if (index.name != kMemWalIndexName) {
            continue;
        }
        if (!ends_with(index.details_type_url, "MemWalIndexDetails")) {
            error = "Index details is not for the MemWAL index, but " + index.details_type_url;
            return false;
        }
        out = index.details_value;
        return true;
    }
    return true;
}

bool dataset_initialize_mem_wal(const std::filesystem::path& dataset_path, std::uint64_t base_version,
                                const std::vector<std::uint8_t>& details, std::uint64_t& new_version,
                                std::string& error) {
    error.clear();
    if (base_version == 0U) {
        error = "Invalid user input: MemWAL needs an existing dataset";
        return false;
    }
    const auto created_at = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    const auto index = encode_mem_wal_index(index_files::new_uuid(), base_version, details, created_at);
    std::vector<std::uint8_t> operation;  // lance.table.CreateIndex { repeated IndexMetadata new_indices = 1 }
    proto_wire::put_bytes_field(operation, 1, index);

    HandBuiltCommit commit;
    commit.operation_field = kCreateIndexOperation;
    commit.operation = std::move(operation);
    commit.base_version = base_version;
    commit.read_version = base_version;
    return dataset_commit_hand_built(dataset_path, commit, new_version, error);
}

}  // namespace nano_lance
