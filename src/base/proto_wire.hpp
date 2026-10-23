// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nano_lance::proto_wire {

// Protobuf wire-format helpers for the messages this codec keeps outside its generated types (the
// fields of `unknown` bytes). The messages here are tiny and fixed.


inline void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

inline void put_key(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint32_t wire) {
    put_varint(out, (static_cast<std::uint64_t>(field) << 3U) | wire);
}

inline void put_bytes_field(std::vector<std::uint8_t>& out, std::uint32_t field, const std::vector<std::uint8_t>& bytes) {
    put_key(out, field, 2);
    put_varint(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

inline void put_u64_field(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint64_t v) {
    if (v == 0) {
        return;  // proto3 default
    }
    put_key(out, field, 0);
    put_varint(out, v);
}

struct Field {
    std::uint32_t number = 0;
    std::uint32_t wire = 0;
    std::uint64_t value = 0;                  // wire 0
    const std::uint8_t* data = nullptr;       // wire 2
    std::size_t size = 0;
    const std::uint8_t* begin = nullptr;      // the whole field, key included
    const std::uint8_t* end = nullptr;
};

/// Iterates the fields of one message.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : at_(data), end_(data + size) {}

    /// Next field; false at the end. `ok()` tells a clean end from a malformed message.
    bool next(Field& f) {
        if (at_ >= end_ || !ok_) {
            return false;
        }
        f.begin = at_;
        std::uint64_t key = 0;
        if (!varint(key)) {
            return false;
        }
        f.number = static_cast<std::uint32_t>(key >> 3U);
        f.wire = static_cast<std::uint32_t>(key & 7U);
        switch (f.wire) {
            case 0:
                if (!varint(f.value)) {
                    return false;
                }
                break;
            case 1:
                if (end_ - at_ < 8) {
                    return fail();
                }
                f.data = at_;
                f.size = 8;
                at_ += 8;
                break;
            case 2: {
                std::uint64_t n = 0;
                if (!varint(n) || n > static_cast<std::uint64_t>(end_ - at_)) {
                    return fail();
                }
                f.data = at_;
                f.size = static_cast<std::size_t>(n);
                at_ += n;
                break;
            }
            case 5:
                if (end_ - at_ < 4) {
                    return fail();
                }
                f.data = at_;
                f.size = 4;
                at_ += 4;
                break;
            default:
                return fail();
        }
        f.end = at_;
        return true;
    }
    bool ok() const { return ok_; }

private:
    bool fail() {
        ok_ = false;
        return false;
    }
    bool varint(std::uint64_t& out) {
        out = 0;
        for (unsigned shift = 0; shift < 70; shift += 7) {
            if (at_ >= end_) {
                return fail();
            }
            const std::uint8_t b = *at_++;
            out |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
            if ((b & 0x80U) == 0U) {
                return true;
            }
        }
        return fail();
    }
    const std::uint8_t* at_;
    const std::uint8_t* end_;
    bool ok_ = true;
};
}  // namespace nano_lance::proto_wire
