// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The operations of a Lance transaction, as the commit path and the conflict checks decode them. A
// transaction file names one operation by its field number in lance.table.Transaction; decoding must
// read each kind's fields as Lance writes them (repeated fragments, packed or unpacked ids, maps as
// entries) and refuse what it cannot read rather than guess at it.

#include "lance_minimal.pb.hpp"
#include "table/dataset_operation.hpp"

#include <cstdint>
#include <cstdlib>
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

using Bytes = std::vector<std::uint8_t>;

void varint(Bytes& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void key(Bytes& out, std::uint32_t field, std::uint32_t wire) {
    varint(out, (static_cast<std::uint64_t>(field) << 3U) | wire);
}

void uint_field(Bytes& out, std::uint32_t field, std::uint64_t value) {
    key(out, field, 0);
    varint(out, value);
}

void bytes_field(Bytes& out, std::uint32_t field, const Bytes& value) {
    key(out, field, 2);
    varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

pb::DataFragment fragment(std::uint64_t id, std::uint64_t rows) {
    pb::DataFragment f;
    f.id = id;
    f.physical_rows = rows;
    return f;
}

nano_lance::Operation decode(std::uint32_t kind, const Bytes& message) {
    nano_lance::Operation op;
    std::string error;
    const bool decoded = nano_lance::decode_operation(kind, message, op, error);
    require(decoded, "decode " + std::to_string(kind) + ": " + error);
    return op;
}

}  // namespace

void require(bool ok, const std::string& msg, const std::string& detail) {
    require(ok, msg + detail);
}

int main() {
    // Append: each fragment is a repeated message in field 1.
    {
        Bytes msg;
        bytes_field(msg, 1, pb::encode_data_fragment(fragment(3, 10)));
        bytes_field(msg, 1, pb::encode_data_fragment(fragment(4, 20)));
        const auto op = decode(nano_lance::kAppend, msg);
        require(op.kind == nano_lance::kAppend, "the kind is kept");
        require(op.fragments.size() == 2, "two appended fragments");
        require(op.fragments[0].id == 3 && op.fragments[0].physical_rows == 10, "the first fragment");
        require(op.fragments[1].id == 4 && op.fragments[1].physical_rows == 20, "the second fragment");
    }

    // Delete: updated fragments in field 1; deleted fragment ids in field 2, packed or one per field.
    {
        Bytes msg;
        bytes_field(msg, 1, pb::encode_data_fragment(fragment(0, 8)));
        uint_field(msg, 2, 5);
        uint_field(msg, 2, 7);
        Bytes packed;
        varint(packed, 9);
        varint(packed, 300);  // a two-byte varint
        bytes_field(msg, 2, packed);
        const auto op = decode(nano_lance::kDelete, msg);
        require(op.updated_fragments.size() == 1 && op.updated_fragments[0].id == 0, "the updated fragment");
        require(op.removed_ids == std::vector<std::uint64_t>({5, 7, 9, 300}),
                "unpacked and packed ids are read in order");
    }

    // An empty Delete is a valid message: nothing updated, nothing removed.
    {
        const auto op = decode(nano_lance::kDelete, {});
        require(op.updated_fragments.empty() && op.removed_ids.empty(), "an empty Delete changes nothing");
    }

    // Restore, ReserveFragments: one varint each.
    {
        Bytes restore;
        uint_field(restore, 1, 4);
        require(decode(nano_lance::kRestore, restore).restore_version == 4, "Restore names its version");

        Bytes reserve;
        uint_field(reserve, 1, 12);
        require(decode(nano_lance::kReserveFragments, reserve).reserve == 12, "ReserveFragments names its count");
    }

    // UpdateBases: each BasePath message is kept as its bytes, in order.
    {
        Bytes base_a = {0x08, 0x01, 0x22, 0x02, '/', 'a'};
        Bytes base_b = {0x08, 0x02, 0x22, 0x02, '/', 'b'};
        Bytes msg;
        bytes_field(msg, 1, base_a);
        bytes_field(msg, 1, base_b);
        const auto op = decode(nano_lance::kUpdateBases, msg);
        require(op.new_bases == std::vector<Bytes>({base_a, base_b}), "the new base paths, in order");
    }

    // Overwrite: schema fields (field 2), schema metadata entries (3), config upserts (4), initial bases (5).
    {
        pb::Field column;
        column.name = "id";
        column.id = 0;
        column.logical_type = "int64";
        Bytes msg;
        bytes_field(msg, 2, pb::encode_field(column));

        Bytes entry;  // a map entry: key 1, value 2
        bytes_field(entry, 1, Bytes{'k'});
        bytes_field(entry, 2, Bytes{'v'});
        bytes_field(msg, 4, entry);

        const auto op = decode(nano_lance::kOverwrite, msg);
        require(op.has_schema && op.schema.size() == 1 && op.schema[0].name == "id" &&
                    op.schema[0].logical_type == "int64",
                "an overwrite carries its schema");
        require(op.config_upserts.size() == 1 && op.config_upserts.at("k") == "v", "a config upsert");

        const auto empty = decode(nano_lance::kOverwrite, {});
        require(empty.has_schema, "an overwrite always has a schema, even an empty one");
    }

    // An operation this decoder does not read is refused when it has content to read.
    {
        nano_lance::Operation op;
        std::string error;
        require(!nano_lance::decode_operation(nano_lance::kCloneField, Bytes{0x08, 0x01}, op, error),
                "a clone operation has no decoder");
        require(error.find("is not supported") != std::string::npos, "the refusal says so: ", error);
    }

    // An empty message of a kind this decoder does not read is refused too (it once decoded as an empty
    // operation, the refusal only running inside the field loop).
    {
        nano_lance::Operation op;
        std::string error;
        require(!nano_lance::decode_operation(nano_lance::kCloneField, {}, op, error), "an empty clone is refused");
        require(error.find("is not supported") != std::string::npos, "the refusal says so: ", error);
    }

    // operation_name: Lance's names; an unknown field number is "an operation".
    {
        require(std::string(nano_lance::operation_name(nano_lance::kAppend)) == "Append", "Append's name");
        require(std::string(nano_lance::operation_name(nano_lance::kDataOverlay)) == "DataOverlay",
                "DataOverlay's name");
        require(std::string(nano_lance::operation_name(nano_lance::kCloneField)) == "an operation",
                "an operation with no name");
    }

    // A transaction file names its operation: the one field in 100..115 with a message.
    {
        Bytes file;
        uint_field(file, 1, 2);  // read_version
        bytes_field(file, 2, Bytes{'u', 'u', 'i', 'd'});
        bytes_field(file, nano_lance::kDelete, Bytes{0x08, 0x03});
        std::uint32_t field = 0;
        Bytes message;
        std::string error;
        require(nano_lance::transaction_file_operation(file, field, message, error), "a transaction file: ", error);
        require(field == nano_lance::kDelete && message == Bytes({0x08, 0x03}), "its operation and message");

        Bytes no_op;
        uint_field(no_op, 1, 2);
        require(!nano_lance::transaction_file_operation(no_op, field, message, error), "no operation is refused");
        require(error == "a transaction file has no operation", "the error says so: ", error);
    }

    std::cout << "dataset operations: append, delete, restore, reserve, bases, overwrite, file operation OK\n";
    return 0;
}
