// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "dataset_operation.hpp"

#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace nano_lance {
namespace {

using Bytes = OpBytes;

// ── protobuf, read ──────────────────────────────────────────────────────────────────────────────

/// One field of a message: its number, wire type, and value (a varint, or the bytes of a
/// length-delimited field).
struct WireField {
    std::uint32_t number = 0;
    std::uint32_t wire = 0;
    std::uint64_t value = 0;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    Bytes bytes() const { return Bytes(data, data + size); }
    std::string text() const { return std::string(reinterpret_cast<const char*>(data), size); }
};

class WireReader {
public:
    WireReader(const std::uint8_t* data, std::size_t size) : at_(data), end_(data + size) {}
    explicit WireReader(const Bytes& b) : WireReader(b.data(), b.size()) {}
    explicit WireReader(const WireField& f) : WireReader(f.data, f.size) {}

    /// The next field; false at the end, or on a malformed message (then `bad()`).
    bool next(WireField& f) {
        if (at_ >= end_) {
            return false;
        }
        std::uint64_t key = 0;
        if (!varint(key)) {
            return fail();
        }
        f = WireField{};
        f.number = static_cast<std::uint32_t>(key >> 3U);
        f.wire = static_cast<std::uint32_t>(key & 7U);
        switch (f.wire) {
            case 0:
                return varint(f.value) || fail();
            case 1:
                if (end_ - at_ < 8) {
                    return fail();
                }
                for (int i = 7; i >= 0; --i) {
                    f.value = (f.value << 8U) | at_[i];
                }
                at_ += 8;
                return true;
            case 2: {
                std::uint64_t n = 0;
                if (!varint(n) || n > static_cast<std::uint64_t>(end_ - at_)) {
                    return fail();
                }
                f.data = at_;
                f.size = static_cast<std::size_t>(n);
                at_ += n;
                return true;
            }
            case 5:
                if (end_ - at_ < 4) {
                    return fail();
                }
                for (int i = 3; i >= 0; --i) {
                    f.value = (f.value << 8U) | at_[i];
                }
                at_ += 4;
                return true;
            default:
                return fail();
        }
    }
    bool bad() const { return bad_; }

    bool varint(std::uint64_t& out) {
        out = 0;
        for (int shift = 0; shift < 64 && at_ < end_; shift += 7) {
            const std::uint8_t b = *at_++;
            out |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
            if ((b & 0x80U) == 0U) {
                return true;
            }
        }
        return false;
    }

private:
    bool fail() {
        bad_ = true;
        at_ = end_;
        return false;
    }
    const std::uint8_t* at_;
    const std::uint8_t* end_;
    bool bad_ = false;
};

/// A repeated scalar field: packed (one length-delimited run) or not (one varint per entry).
void append_varints(const WireField& f, std::vector<std::uint64_t>& out) {
    if (f.wire == 0) {
        out.push_back(f.value);
        return;
    }
    WireReader r(f);
    std::uint64_t v = 0;
    while (r.varint(v)) {
        out.push_back(v);
    }
}

bool decode_fragment(const WireField& f, pb::DataFragment& out, std::string& error) {
    if (!pb::decode_data_fragment(f.bytes(), out)) {
        error = "Invalid user input: a fragment of the transaction could not be decoded";
        return false;
    }
    return true;
}

bool decode_fields(const std::vector<WireField>& protos, std::vector<pb::Field>& out, std::string& error) {
    for (const auto& p : protos) {
        pb::Field field;
        if (!pb::decode_field(p.bytes(), field)) {
            error = "Invalid user input: a schema field of the transaction could not be decoded";
            return false;
        }
        out.push_back(std::move(field));
    }
    return true;
}

/// A map<string, bytes> entry (key 1, value 2).
std::pair<std::string, Bytes> map_entry(const WireField& f) {
    std::pair<std::string, Bytes> out;
    WireReader r(f);
    WireField e;
    while (r.next(e)) {
        if (e.number == 1) {
            out.first = e.text();
        } else if (e.number == 2) {
            out.second = e.bytes();
        }
    }
    return out;
}

UpdateMap decode_update_map(const WireField& f) {
    UpdateMap out;
    WireReader r(f);
    WireField e;
    while (r.next(e)) {
        if (e.number == 1) {
            std::string key;
            std::optional<std::string> value;
            WireReader er(e);
            WireField kv;
            while (er.next(kv)) {
                if (kv.number == 1) {
                    key = kv.text();
                } else if (kv.number == 2) {
                    value = kv.text();
                }
            }
            out.entries.emplace_back(std::move(key), std::move(value));
        } else if (e.number == 2) {
            out.replace = e.value != 0U;
        }
    }
    return out;
}

}  // namespace

const char* operation_name(std::uint32_t field) {
    switch (field) {
        case kAppend: return "Append";
        case kDelete: return "Delete";
        case kOverwrite: return "Overwrite";
        case kCreateIndex: return "CreateIndex";
        case kRewrite: return "Rewrite";
        case kMerge: return "Merge";
        case kRestore: return "Restore";
        case kReserveFragments: return "ReserveFragments";
        case kUpdate: return "Update";
        case kProject: return "Project";
        case kUpdateConfig: return "UpdateConfig";
        case kDataReplacement: return "DataReplacement";
        case kUpdateBases: return "UpdateBases";
        case kDataOverlay: return "DataOverlay";
        default: return "an operation";
    }
}

namespace {

bool uuid_of(const WireField& f, std::array<std::uint8_t, 16>& out) {
    WireReader r(f);
    WireField e;
    while (r.next(e)) {
        if (e.number == 1 && e.size == 16) {
            std::copy(e.data, e.data + 16, out.begin());
            return true;
        }
    }
    return false;
}

}  // namespace

bool decode_operation(std::uint32_t kind, const Bytes& message, Operation& op, std::string& error) {
    op.kind = kind;
    WireReader r(message);
    WireField f;
    std::vector<WireField> schema;
    auto fragment_into = [&](std::vector<pb::DataFragment>& out) {
        pb::DataFragment frag;
        if (!decode_fragment(f, frag, error)) {
            return false;
        }
        out.push_back(std::move(frag));
        return true;
    };
    while (r.next(f)) {
        switch (kind) {
            case kAppend:
                if (f.number == 1 && !fragment_into(op.fragments)) return false;
                break;
            case kDelete:
                if (f.number == 1 && !fragment_into(op.updated_fragments)) return false;
                if (f.number == 2) append_varints(f, op.removed_ids);
                break;
            case kOverwrite:
            case kMerge:
                if (f.number == 1 && !fragment_into(op.fragments)) return false;
                if (f.number == 2) schema.push_back(f);
                if (f.number == 3) {
                    op.has_schema_metadata = true;
                    op.schema_metadata.insert(map_entry(f));
                }
                if (kind == kOverwrite && f.number == 4) {
                    auto [k, v] = map_entry(f);
                    op.config_upserts[k] = std::string(v.begin(), v.end());
                }
                if (kind == kOverwrite && f.number == 5) op.initial_bases.push_back(f.bytes());
                if (kind == kMerge && f.number == 4) op.preserves_nullability = f.value != 0U;
                break;
            case kProject:
                if (f.number == 1) schema.push_back(f);
                if (f.number == 2) op.preserves_nullability = f.value != 0U;
                break;
            case kCreateIndex:
                if (f.number == 1 || f.number == 2) {
                    pb::IndexMetadata index;
                    if (!pb::decode_index_message(f.data, f.size, index, error)) {
                        error = "Invalid user input: an index of the transaction could not be decoded";
                        return false;
                    }
                    (f.number == 1 ? op.new_indices : op.removed_indices).push_back(std::move(index));
                }
                break;
            case kRestore:
                if (f.number == 1) op.restore_version = f.value;
                break;
            case kReserveFragments:
                if (f.number == 1) op.reserve = f.value;
                break;
            case kUpdate:
                if (f.number == 1) append_varints(f, op.removed_ids);
                if (f.number == 2 && !fragment_into(op.updated_fragments)) return false;
                if (f.number == 3 && !fragment_into(op.fragments)) return false;
                if (f.number == 4) append_varints(f, op.fields_modified);
                if (f.number == 7) op.rewrite_columns = f.value == 1U;
                if (f.number == 10) {
                    std::pair<std::uint64_t, OpBytes> entry;
                    WireReader er(f);
                    WireField e;
                    while (er.next(e)) {
                        if (e.number == 1) entry.first = e.value;
                        if (e.number == 2) entry.second = e.bytes();
                    }
                    op.updated_fragment_offsets.push_back(std::move(entry));
                }
                break;
            case kRewrite: {
                if (f.number == 1 || f.number == 2) {  // the deprecated single group
                    if (op.groups.empty()) op.groups.emplace_back();
                    if (!fragment_into(f.number == 1 ? op.groups.front().old_fragments
                                                     : op.groups.front().new_fragments)) {
                        return false;
                    }
                } else if (f.number == 3) {
                    RewriteGroup group;
                    WireReader gr(f);
                    WireField g;
                    while (gr.next(g)) {
                        pb::DataFragment frag;
                        if ((g.number == 1 || g.number == 2) && !decode_fragment(g, frag, error)) return false;
                        if (g.number == 1) group.old_fragments.push_back(std::move(frag));
                        if (g.number == 2) group.new_fragments.push_back(std::move(frag));
                    }
                    op.groups.push_back(std::move(group));
                } else if (f.number == 4) {
                    RewrittenIndex ri;
                    WireReader ir(f);
                    WireField g;
                    while (ir.next(g)) {
                        if (g.number == 1) uuid_of(g, ri.old_id);
                        if (g.number == 2) uuid_of(g, ri.new_id);
                        if (g.number == 3) {
                            WireReader ar(g);
                            WireField a;
                            while (ar.next(a)) {
                                if (a.number == 1) ri.details_type_url = a.text();
                                if (a.number == 2) ri.details_value = a.bytes();
                            }
                        }
                        if (g.number == 4) ri.index_version = static_cast<std::uint32_t>(g.value);
                    }
                    op.rewritten_indices.push_back(std::move(ri));
                }
                break;
            }
            case kUpdateBases:
                if (f.number == 1) op.new_bases.push_back(f.bytes());
                break;
            case kUpdateConfig:
                if (f.number == 6) op.config_updates = decode_update_map(f);
                if (f.number == 7) op.table_metadata_updates = decode_update_map(f);
                if (f.number == 8) op.schema_metadata_updates = decode_update_map(f);
                if (f.number == 9) {
                    std::int32_t id = 0;
                    UpdateMap m;
                    WireReader er(f);
                    WireField e;
                    while (er.next(e)) {
                        if (e.number == 1) id = static_cast<std::int32_t>(e.value);
                        if (e.number == 2) m = decode_update_map(e);
                    }
                    op.field_metadata_updates[id] = std::move(m);
                }
                // The deprecated fields (1-4): upserts, deletions and replacements of the same maps.
                if (f.number == 1) {
                    if (!op.config_updates) op.config_updates.emplace();
                    auto [k, v] = map_entry(f);
                    op.config_updates->entries.emplace_back(k, std::string(v.begin(), v.end()));
                }
                if (f.number == 2) {
                    if (!op.config_updates) op.config_updates.emplace();
                    op.config_updates->entries.emplace_back(f.text(), std::nullopt);
                }
                if (f.number == 3) {
                    if (!op.schema_metadata_updates) op.schema_metadata_updates = UpdateMap{{}, true};
                    auto [k, v] = map_entry(f);
                    op.schema_metadata_updates->entries.emplace_back(k, std::string(v.begin(), v.end()));
                }
                break;
            case kDataReplacement:
                if (f.number == 1) {
                    std::uint64_t id = 0;
                    pb::DataFile file;
                    bool has_file = false;
                    WireReader gr(f);
                    WireField g;
                    while (gr.next(g)) {
                        if (g.number == 1) id = g.value;
                        if (g.number == 2) has_file = pb::decode_data_file(g.bytes(), file);
                    }
                    if (!has_file) {
                        error = "Invalid user input: a DataReplacementGroup has no new file";
                        return false;
                    }
                    op.replacements.emplace_back(id, std::move(file));
                }
                break;
            case kDataOverlay:
                if (f.number == 1) {
                    std::pair<std::uint64_t, std::vector<Bytes>> group;
                    WireReader gr(f);
                    WireField g;
                    while (gr.next(g)) {
                        if (g.number == 1) group.first = g.value;
                        if (g.number == 2) group.second.push_back(g.bytes());
                    }
                    op.overlay_groups.push_back(std::move(group));
                }
                break;
            default:
                error = std::string("LanceDataset.commit of ") + operation_name(kind) + " is not supported";
                return false;
        }
    }
    if (r.bad()) {
        error = "Invalid user input: the transaction's operation could not be decoded";
        return false;
    }
    op.has_schema = !schema.empty() || kind == kOverwrite || kind == kMerge || kind == kProject;
    return decode_fields(schema, op.schema, error);
}

bool transaction_file_operation(const OpBytes& file, std::uint32_t& field, OpBytes& message, std::string& error) {
    WireReader r(file);
    WireField f;
    while (r.next(f)) {
        if (f.number >= kFirstOperationField && f.number <= kLastOperationField && f.wire == 2U) {
            field = f.number;
            message = f.bytes();
            return true;
        }
    }
    error = r.bad() ? "a transaction file could not be decoded" : "a transaction file has no operation";
    return false;
}

}  // namespace nano_lance
