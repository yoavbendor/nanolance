// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The transaction file of a commit, as Lance writes one (`_transactions/{read_version}-{uuid}.txn`,
// a lance.table.Transaction message). Lance reads the transactions of the versions committed since
// a writer's read version to decide whether its change still applies (and fails outright when one
// has none), so every version nanolance commits carries one. The operation is read off the change
// from the version before -- fragments added, deletion files changed, fragments replaced, schema,
// indices, config -- except where the change alone cannot say (an overwrite, a restore, a
// compaction: the commit says so in Manifest::operation).

#include "transaction_file.hpp"

#include "nanolance/manifest_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <set>

namespace nano_lance {
namespace {

using Bytes = std::vector<std::uint8_t>;

// ── protobuf, by hand: Transaction is written, never read, here ──────────────────────────────────

void varint(Bytes& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void tag(Bytes& out, std::uint32_t field, std::uint32_t wire_type) { varint(out, (field << 3U) | wire_type); }

void put_uint(Bytes& out, std::uint32_t field, std::uint64_t v) {
    if (v != 0U) {  // proto3: zero is left out
        tag(out, field, 0);
        varint(out, v);
    }
}

void put_bytes(Bytes& out, std::uint32_t field, const std::uint8_t* data, std::size_t size) {
    tag(out, field, 2);
    varint(out, size);
    out.insert(out.end(), data, data + size);
}
void put_bytes(Bytes& out, std::uint32_t field, const Bytes& b) { put_bytes(out, field, b.data(), b.size()); }
void put_string(Bytes& out, std::uint32_t field, const std::string& s) {
    if (!s.empty()) {
        put_bytes(out, field, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    }
}
/// A map entry (key 1, value 2), always written (an empty value is still an entry).
void put_entry(Bytes& out, std::uint32_t field, const std::string& key, const std::uint8_t* value, std::size_t size) {
    Bytes entry;
    put_string(entry, 1, key);
    put_bytes(entry, 2, value, size);
    put_bytes(out, field, entry);
}

Bytes fragment_bytes(const pb::DataFragment& f) { return pb::encode_data_fragment(f); }

void put_schema(Bytes& out, std::uint32_t field, const pb::Manifest& m) {
    for (const auto& f : m.fields) {
        put_bytes(out, field, pb::encode_field(f));
    }
}
void put_schema_metadata(Bytes& out, std::uint32_t field, const pb::Manifest& m) {
    for (const auto& [k, v] : m.schema_metadata) {
        put_entry(out, field, k, v.data(), v.size());
    }
}

/// UpdateMap: entries (key, value; no value = removed), merged into the existing map.
Bytes update_map(const std::map<std::string, std::string>& before, const std::map<std::string, std::string>& after) {
    Bytes out;
    for (const auto& [k, v] : after) {
        const auto it = before.find(k);
        if (it == before.end() || it->second != v) {
            Bytes entry;
            put_string(entry, 1, k);
            put_bytes(entry, 2, reinterpret_cast<const std::uint8_t*>(v.data()), v.size());
            put_bytes(out, 1, entry);
        }
    }
    for (const auto& [k, v] : before) {
        if (after.count(k) == 0U) {
            Bytes entry;
            put_string(entry, 1, k);  // no value: delete
            put_bytes(out, 1, entry);
        }
    }
    return out;
}

std::map<std::string, std::string> as_strings(const std::map<std::string, std::vector<std::uint8_t>>& m) {
    std::map<std::string, std::string> out;
    for (const auto& [k, v] : m) {
        out[k] = std::string(v.begin(), v.end());
    }
    return out;
}

std::string random_uuid() {
    thread_local std::mt19937_64 rng{std::random_device{}() ^ (static_cast<std::uint64_t>(std::random_device{}()) << 32U)};
    std::uint8_t b[16];
    for (int i = 0; i < 16; i += 8) {
        const auto r = rng();
        for (int j = 0; j < 8; ++j) {
            b[i + j] = static_cast<std::uint8_t>(r >> (8 * j));
        }
    }
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0FU) | 0x40U);  // version 4
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3FU) | 0x80U);  // RFC 4122 variant
    char s[37];
    std::snprintf(s, sizeof(s), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
                  b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return s;
}

// ── the operation, from the change ─────────────────────────────────────────────────────────────────

constexpr std::uint32_t kAppend = 100, kDelete = 101, kOverwrite = 102, kCreateIndex = 103, kRewrite = 104,
                        kMerge = 105, kRestore = 106, kReserveFragments = 107, kUpdate = 108, kProject = 109,
                        kUpdateConfig = 110;

Bytes overwrite(const pb::Manifest& next, const std::map<std::string, std::string>& prior_config) {
    Bytes op;
    for (const auto& f : next.fragments) {
        put_bytes(op, 1, fragment_bytes(f));
    }
    put_schema(op, 2, next);
    put_schema_metadata(op, 3, next);
    for (const auto& [k, v] : next.config) {
        const auto it = prior_config.find(k);
        if (it == prior_config.end() || it->second != v) {
            put_entry(op, 4, k, reinterpret_cast<const std::uint8_t*>(v.data()), v.size());
        }
    }
    return op;
}

/// The operation's field number and message.
std::pair<std::uint32_t, Bytes> operation(const pb::Manifest* parent, const pb::Manifest& next) {
    if (next.operation == pb::Manifest::Operation::Reserve) {
        Bytes op;
        put_uint(op, 1, next.reserved_fragments);
        return {kReserveFragments, op};
    }
    if (next.operation == pb::Manifest::Operation::Append) {
        Bytes op;
        for (std::size_t i = next.first_new_fragment; i < next.fragments.size(); ++i) {
            put_bytes(op, 1, fragment_bytes(next.fragments[i]));
        }
        return {kAppend, op};
    }
    if (parent == nullptr || next.operation == pb::Manifest::Operation::Overwrite) {
        // (Without the version before, every config value is an upsert: the same table config.)
        return {kOverwrite, overwrite(next, parent != nullptr ? parent->config : std::map<std::string, std::string>{})};
    }
    if (next.operation == pb::Manifest::Operation::Restore) {
        Bytes op;
        put_uint(op, 1, next.restored_version);
        return {kRestore, op};
    }
    std::map<std::uint64_t, Bytes> before;
    for (const auto& f : parent->fragments) {
        before[f.id] = fragment_bytes(f);
    }
    std::vector<const pb::DataFragment*> added;
    std::vector<const pb::DataFragment*> changed;
    std::set<std::uint64_t> kept;
    bool files_changed = false;  // a kept fragment's data files differ (not only its deletions)
    std::set<std::uint32_t> fields_modified;
    for (const auto& f : next.fragments) {
        const auto it = before.find(f.id);
        if (it == before.end()) {
            added.push_back(&f);
            continue;
        }
        kept.insert(f.id);
        if (fragment_bytes(f) != it->second) {
            changed.push_back(&f);
            const auto old = std::find_if(parent->fragments.begin(), parent->fragments.end(),
                                          [&](const pb::DataFragment& p) { return p.id == f.id; });
            std::set<std::string> old_files;
            for (const auto& file : old->files) {
                old_files.insert(file.path);
            }
            for (const auto& file : f.files) {
                if (old_files.count(file.path) == 0U) {
                    files_changed = true;
                    for (const auto id : file.fields) {
                        if (id >= 0) {
                            fields_modified.insert(static_cast<std::uint32_t>(id));
                        }
                    }
                }
            }
            if (f.files.size() != old->files.size()) {
                files_changed = true;
            }
        }
    }
    std::vector<std::uint64_t> removed;
    for (const auto& f : parent->fragments) {
        if (kept.count(f.id) == 0U) {
            removed.push_back(f.id);
        }
    }
    Bytes before_schema;
    Bytes after_schema;
    put_schema(before_schema, 1, *parent);
    put_schema(after_schema, 1, next);
    const bool schema_changed = before_schema != after_schema;

    if (next.operation == pb::Manifest::Operation::Rewrite) {
        // One group: the fragments replaced and those replacing them (Lance conflicts a concurrent
        // change to any of them, as it would with finer groups that overlap it).
        Bytes group;
        for (const auto& f : parent->fragments) {
            if (kept.count(f.id) == 0U) {
                put_bytes(group, 1, fragment_bytes(f));
            }
        }
        for (const auto* f : changed) {  // fragments rewritten in place keep their id
            put_bytes(group, 1, before[f->id]);
            put_bytes(group, 2, fragment_bytes(*f));
        }
        for (const auto* f : added) {
            put_bytes(group, 2, fragment_bytes(*f));
        }
        Bytes op;
        put_bytes(op, 3, group);
        return {kRewrite, op};
    }
    if (schema_changed) {
        Bytes op;
        if (added.empty() && removed.empty() && !files_changed) {
            put_schema(op, 1, next);  // a drop or a rename: Project
            return {kProject, op};
        }
        for (const auto& f : next.fragments) {
            put_bytes(op, 1, fragment_bytes(f));
        }
        put_schema(op, 2, next);
        put_schema_metadata(op, 3, next);
        return {kMerge, op};
    }
    const bool update = next.operation == pb::Manifest::Operation::Update;  // update, merge_insert
    if (!update && !added.empty() && removed.empty() && changed.empty()) {
        Bytes op;
        for (const auto* f : added) {
            put_bytes(op, 1, fragment_bytes(*f));
        }
        return {kAppend, op};
    }
    if (!update && added.empty() && !files_changed && (!changed.empty() || !removed.empty())) {
        Bytes op;
        for (const auto* f : changed) {
            put_bytes(op, 1, fragment_bytes(*f));
        }
        if (!removed.empty()) {
            Bytes packed;
            for (const auto id : removed) {
                varint(packed, id);
            }
            put_bytes(op, 2, packed);
        }
        return {kDelete, op};
    }
    if (update || !added.empty() || !removed.empty() || !changed.empty()) {
        Bytes op;
        if (!removed.empty()) {
            Bytes packed;
            for (const auto id : removed) {
                varint(packed, id);
            }
            put_bytes(op, 1, packed);
        }
        for (const auto* f : changed) {
            put_bytes(op, 2, fragment_bytes(*f));
        }
        for (const auto* f : added) {
            put_bytes(op, 3, fragment_bytes(*f));
        }
        if (!fields_modified.empty()) {
            Bytes packed;
            for (const auto id : fields_modified) {
                varint(packed, id);
            }
            put_bytes(op, 4, packed);
        }
        return {kUpdate, op};
    }
    // The fragments and schema as they were: indices, or config and metadata.
    std::map<std::string, Bytes> old_indices;
    for (const auto& index : parent->indices) {
        old_indices[pb::uuid_string(index.uuid)] = pb::encode_index_message(index);
    }
    Bytes created;
    std::set<std::string> next_uuids;
    for (const auto& index : next.indices) {
        const auto uuid = pb::uuid_string(index.uuid);
        next_uuids.insert(uuid);
        auto bytes = pb::encode_index_message(index);
        const auto it = old_indices.find(uuid);
        if (it == old_indices.end() || it->second != bytes) {
            put_bytes(created, 1, bytes);
        }
    }
    Bytes dropped;
    for (const auto& index : parent->indices) {
        const auto uuid = pb::uuid_string(index.uuid);
        const auto bytes = pb::encode_index_message(index);
        if (next_uuids.count(uuid) == 0U) {
            put_bytes(dropped, 2, bytes);
        } else {
            const auto now = std::find_if(next.indices.begin(), next.indices.end(), [&](const pb::IndexMetadata& i) {
                return pb::uuid_string(i.uuid) == uuid;
            });
            if (pb::encode_index_message(*now) != bytes) {
                put_bytes(dropped, 2, bytes);  // replaced
            }
        }
    }
    if (!created.empty() || !dropped.empty()) {
        created.insert(created.end(), dropped.begin(), dropped.end());
        return {kCreateIndex, created};
    }
    Bytes op;
    const auto config = update_map(parent->config, next.config);
    const auto table = update_map(parent->table_metadata, next.table_metadata);
    const auto schema = update_map(as_strings(parent->schema_metadata), as_strings(next.schema_metadata));
    if (!config.empty()) {
        put_bytes(op, 6, config);
    }
    if (!table.empty()) {
        put_bytes(op, 7, table);
    }
    if (!schema.empty()) {
        put_bytes(op, 8, schema);
    }
    return {kUpdateConfig, op};
}

}  // namespace

bool write_transaction_file(const std::filesystem::path& dataset_path, const pb::Manifest& next,
                            std::string& file_name, std::string& error) {
    const std::uint64_t read_version = next.version > 0U ? next.version - 1U : 0U;
    pb::Manifest parent;
    bool has_parent = false;
    // An append, an overwrite and a restore say what they do; the rest is read off the change.
    using Op = pb::Manifest::Operation;
    const bool needs_parent = next.operation != Op::Append && next.operation != Op::Overwrite &&
                              next.operation != Op::Restore && next.operation != Op::Reserve;
    if (read_version > 0U && needs_parent) {
        std::string load_error;
        has_parent = load_manifest_version(dataset_path, read_version, parent, load_error);
        if (!has_parent && next.operation == pb::Manifest::Operation::Derive) {
            error = "cannot write the transaction of version " + std::to_string(next.version) + ": " + load_error;
            return false;
        }
    }
    const auto uuid = random_uuid();
    Bytes tx;
    put_uint(tx, 1, read_version);
    put_string(tx, 2, uuid);
    const auto [field, op] = operation(has_parent ? &parent : nullptr, next);
    put_bytes(tx, field, op);

    file_name = std::to_string(read_version) + "-" + uuid + ".txn";
    const auto dir = dataset_path / "_transactions";
    std::error_code ec;
    std::ofstream out(dir / file_name, std::ios::binary | std::ios::trunc);
    if (!out) {  // the first transaction: no directory yet
        std::filesystem::create_directories(dir, ec);
        out.clear();
        out.open(dir / file_name, std::ios::binary | std::ios::trunc);
    }
    out.write(reinterpret_cast<const char*>(tx.data()), static_cast<std::streamsize>(tx.size()));
    out.close();
    if (!out) {
        std::filesystem::remove(dir / file_name, ec);
        error = "failed to write " + (dir / file_name).string();
        return false;
    }
    return true;
}

void remove_transaction_file(const std::filesystem::path& dataset_path, const std::string& file_name) {
    std::error_code ec;
    if (!file_name.empty()) {
        std::filesystem::remove(dataset_path / "_transactions" / file_name, ec);
    }
}

}  // namespace nano_lance
