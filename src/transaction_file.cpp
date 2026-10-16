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
#include <functional>
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

/// Lance's Project.preserves_nullability is false when a field became non-nullable.
bool project_tightens_nullability(const pb::Manifest& before, const pb::Manifest& after) {
    std::map<std::int32_t, bool> nullable;
    for (const auto& f : before.fields) {
        nullable[f.id] = f.nullable;
    }
    for (const auto& f : after.fields) {
        const auto it = nullable.find(f.id);
        if (it != nullable.end() && it->second && !f.nullable) {
            return true;
        }
    }
    return false;
}

/// Lance's merge_introduces_required_field (Merge.preserves_nullability is its negation): a new
/// field that is not nullable at the first new node of its path, or any new node under a
/// non-nullable top-level column.
bool merge_introduces_required_field(const pb::Manifest& before, const pb::Manifest& after) {
    const auto children = [](const pb::Manifest& m, std::int32_t parent) {
        std::vector<const pb::Field*> out;
        for (const auto& f : m.fields) {
            if (f.parent_id == parent) {
                out.push_back(&f);
            }
        }
        return out;
    };
    // (any new node, any first-new node non-nullable) below old_parent / new_parent.
    std::function<std::pair<bool, bool>(std::int32_t, std::int32_t)> subtree = [&](std::int32_t old_parent,
                                                                                    std::int32_t new_parent) {
        bool any_new = false;
        bool any_required = false;
        const auto old_children = children(before, old_parent);
        for (const auto* field : children(after, new_parent)) {
            const auto it = std::find_if(old_children.begin(), old_children.end(),
                                         [&](const pb::Field* o) { return o->name == field->name; });
            if (it != old_children.end()) {
                const auto [n, r] = subtree((*it)->id, field->id);
                any_new = any_new || n;
                any_required = any_required || r;
            } else {
                any_new = true;
                any_required = any_required || !field->nullable;
            }
        }
        return std::make_pair(any_new, any_required);
    };
    const auto old_top = children(before, -1);
    for (const auto* field : children(after, -1)) {
        const auto it = std::find_if(old_top.begin(), old_top.end(),
                                     [&](const pb::Field* o) { return o->name == field->name; });
        if (it == old_top.end()) {
            if (!field->nullable) {
                return true;
            }
            continue;
        }
        const auto [any_new, any_required] = subtree((*it)->id, field->id);
        if (any_required || (any_new && !field->nullable)) {
            return true;
        }
    }
    return false;
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
    if (next.operation == pb::Manifest::Operation::Restore) {
        Bytes op;
        put_uint(op, 1, next.restored_version);
        return {kRestore, op};
    }
    if (parent == nullptr || next.operation == pb::Manifest::Operation::Overwrite) {
        // (Without the version before, every config value is an upsert: the same table config.)
        return {kOverwrite, overwrite(next, parent != nullptr ? parent->config : std::map<std::string, std::string>{})};
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
            // (Files only removed -- those a dropped column left empty -- change no data: a Project.)
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
    bool schema_changed = before_schema != after_schema;
    // Only field metadata changed (Lance's update_field_metadata): an UpdateConfig of it.
    Bytes field_metadata;
    if (schema_changed && parent->fields.size() == next.fields.size() && parent->fragments.size() == next.fragments.size()) {
        bool only_metadata = parent->schema_metadata == next.schema_metadata;
        for (std::size_t i = 0; only_metadata && i < next.fields.size(); ++i) {
            pb::Field a = parent->fields[i];
            pb::Field b = next.fields[i];
            const auto changes = update_map(as_strings(a.metadata), as_strings(b.metadata));
            a.metadata.clear();
            b.metadata.clear();
            only_metadata = pb::encode_field(a) == pb::encode_field(b);
            if (only_metadata && !changes.empty()) {
                Bytes entry;
                put_uint(entry, 1, static_cast<std::uint32_t>(next.fields[i].id));
                put_bytes(entry, 2, changes);
                put_bytes(field_metadata, 9, entry);
            }
        }
        if (only_metadata) {
            for (std::size_t i = 0; only_metadata && i < next.fragments.size(); ++i) {
                only_metadata = fragment_bytes(parent->fragments[i]) == fragment_bytes(next.fragments[i]);
            }
        }
        if (only_metadata) {
            schema_changed = false;
        } else {
            field_metadata.clear();
        }
    }

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
            put_schema(op, 1, next);  // a drop, a rename, a nullability change: Project
            put_uint(op, 2, project_tightens_nullability(*parent, next) ? 0U : 1U);
            return {kProject, op};
        }
        for (const auto& f : next.fragments) {
            put_bytes(op, 1, fragment_bytes(f));
        }
        put_schema(op, 2, next);
        put_schema_metadata(op, 3, next);
        put_uint(op, 4, merge_introduces_required_field(*parent, next) ? 0U : 1U);
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
    op.insert(op.end(), field_metadata.begin(), field_metadata.end());
    return {kUpdateConfig, op};
}

}  // namespace

bool write_transaction_file(const std::filesystem::path& dataset_path, const pb::Manifest& next,
                            std::string& file_name, std::string& error) {
    const bool explicit_op = next.explicit_operation_field != 0U;
    const std::uint64_t read_version =
        explicit_op ? next.explicit_read_version : (next.version > 0U ? next.version - 1U : 0U);
    pb::Manifest parent;
    bool has_parent = false;
    // An append, an overwrite and a restore say what they do; the rest is read off the change.
    using Op = pb::Manifest::Operation;
    const bool needs_parent = !explicit_op && next.operation != Op::Append && next.operation != Op::Overwrite &&
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
    for (const auto& [key, value] : next.transaction_properties) {
        Bytes entry;
        put_string(entry, 1, key);
        put_string(entry, 2, value);
        put_bytes(tx, 4, entry);
    }
    if (explicit_op) {
        put_bytes(tx, next.explicit_operation_field, next.explicit_operation);
    } else {
        const auto [field, op] = operation(has_parent ? &parent : nullptr, next);
        put_bytes(tx, field, op);
    }

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

std::pair<std::uint32_t, std::vector<std::uint8_t>> derive_transaction_operation(const pb::Manifest& parent,
                                                                                  const pb::Manifest& next) {
    return operation(&parent, next);
}

void remove_transaction_file(const std::filesystem::path& dataset_path, const std::string& file_name) {
    std::error_code ec;
    if (!file_name.empty()) {
        std::filesystem::remove(dataset_path / "_transactions" / file_name, ec);
    }
}

}  // namespace nano_lance
