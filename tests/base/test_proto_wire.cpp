// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The protobuf wire-format helpers (base/proto_wire.hpp) that the table layer uses for the messages it keeps
// outside its generated types: varints at their byte boundaries, keys, length-delimited and u64 fields (a zero
// u64 left off, as proto3 does), and Reader over every wire type it knows (varint, fixed64, length-delimited,
// fixed32), with each field's span. A malformed message (truncated varint, length past the end, short fixed
// fields, a varint longer than ten bytes, an unknown wire type) stops the reader with ok() false.

#include "base/proto_wire.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

namespace pw = nano_lance::proto_wire;

std::vector<std::uint8_t> varint(std::uint64_t v) {
    std::vector<std::uint8_t> out;
    pw::put_varint(out, v);
    return out;
}

/// Whether reading `bytes` ends malformed (after any number of good fields).
bool malformed(const std::vector<std::uint8_t>& bytes) {
    pw::Reader r(bytes.data(), bytes.size());
    pw::Field f;
    while (r.next(f)) {
    }
    return !r.ok();
}

}  // namespace

int main() {
    // Varints.
    require(varint(0) == std::vector<std::uint8_t>{0x00}, "0 is one byte");
    require(varint(127) == std::vector<std::uint8_t>{0x7F}, "127 is one byte");
    require(varint(128) == (std::vector<std::uint8_t>{0x80, 0x01}), "128 is two bytes");
    require(varint(300) == (std::vector<std::uint8_t>{0xAC, 0x02}), "300");
    require(varint(UINT64_MAX).size() == 10 && varint(UINT64_MAX).back() == 0x01, "the largest u64 is ten bytes");

    // A message of each field kind, read back.
    std::vector<std::uint8_t> msg;
    pw::put_u64_field(msg, 1, 0);  // left off
    require(msg.empty(), "a zero u64 field is not written");
    pw::put_u64_field(msg, 1, 150);
    pw::put_bytes_field(msg, 2, {'a', 'b', 'c'});
    pw::put_bytes_field(msg, 3, {});
    pw::put_key(msg, 4, 1);  // fixed64
    for (int i = 0; i < 8; ++i) {
        msg.push_back(static_cast<std::uint8_t>(i));
    }
    pw::put_key(msg, 5, 5);  // fixed32
    for (int i = 0; i < 4; ++i) {
        msg.push_back(static_cast<std::uint8_t>(0xF0 + i));
    }
    pw::put_u64_field(msg, 1000, UINT64_MAX);
    require(msg[0] == 0x08 && msg[1] == 0x96 && msg[2] == 0x01, "field 1 = 150 as protobuf writes it");

    {
        pw::Reader r(msg.data(), msg.size());
        pw::Field f;
        require(r.next(f) && f.number == 1 && f.wire == 0 && f.value == 150, "field 1");
        require(f.begin == msg.data() && f.end == msg.data() + 3, "field 1's span, key included");
        require(r.next(f) && f.number == 2 && f.wire == 2 && f.size == 3 &&
                    std::string(reinterpret_cast<const char*>(f.data), f.size) == "abc",
                "field 2");
        require(r.next(f) && f.number == 3 && f.wire == 2 && f.size == 0, "field 3, empty bytes");
        require(r.next(f) && f.number == 4 && f.wire == 1 && f.size == 8 && f.data[7] == 7, "field 4, fixed64");
        require(r.next(f) && f.number == 5 && f.wire == 5 && f.size == 4 && f.data[0] == 0xF0, "field 5, fixed32");
        require(r.next(f) && f.number == 1000 && f.wire == 0 && f.value == UINT64_MAX, "field 1000, a large number");
        require(!r.next(f) && r.ok(), "a clean end");
    }
    {
        pw::Reader r(nullptr, 0);
        pw::Field f;
        require(!r.next(f) && r.ok(), "an empty message is a clean end");
    }

    // Malformed messages.
    require(malformed({0x08}), "a key with no value");
    require(malformed({0x08, 0x80}), "a truncated varint");
    require(malformed({0x12, 0x05, 'a', 'b'}), "a length past the end");
    require(malformed({0x12, 0x80}), "a truncated length");
    require(malformed({0x21, 1, 2, 3}), "a short fixed64");
    require(malformed({0x2D, 1, 2}), "a short fixed32");
    require(malformed({0x0B}), "wire type 3 (start group) is not read");
    require(malformed({0x0F}), "wire type 7 does not exist");
    require(malformed({0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}),
            "a varint longer than ten bytes");
    {
        auto good = msg;
        good.push_back(0x08);  // a good message, then a truncated field
        pw::Reader r(good.data(), good.size());
        pw::Field f;
        int n = 0;
        while (r.next(f)) {
            ++n;
        }
        require(n == 6 && !r.ok(), "the good fields are read, then the reader stops not ok");
        require(!r.next(f), "and stays stopped");
    }

    std::cout << "proto_wire: all checks passed\n";
    return 0;
}
