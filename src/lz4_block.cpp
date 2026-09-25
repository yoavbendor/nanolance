// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/lz4_block.hpp"

#include "nanolance/read_safety.hpp"

#include <cstring>

namespace nano_lance::lz4_block {

namespace {

/// The shortest back-reference LZ4 will emit. A token's match-length nibble is stored minus this.
constexpr std::size_t kMinMatch = 4U;

/// Read one length that the token's 4-bit field said was saturated: 255 means "add 255 and keep
/// reading". Returns false on a truncated run or on a length that would overflow.
bool read_extended_length(const std::uint8_t* data, std::size_t size, std::size_t& pos,
                          std::size_t& length, std::string& error) {
    for (;;) {
        if (pos >= size) {
            error = "LZ4 block ends inside a length";
            return false;
        }
        const auto byte = data[pos++];
        if (length > SIZE_MAX - byte) {
            error = "LZ4 length overflows";
            return false;
        }
        length += byte;
        if (byte != 255U) {
            return true;
        }
    }
}

}  // namespace

bool decompress_block(const std::uint8_t* data, std::size_t size, std::size_t uncompressed_size,
                      std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    // The declared size decides the allocation, so it has to be checked against what this block could
    // possibly produce BEFORE reserving -- otherwise an 8-byte buffer declaring 3.7 GB makes the
    // reader allocate 3.7 GB and the read-limit budget (8 GiB per decoded buffer) never fires. Found
    // by tests/fuzz/fuzz_lz4.cpp, 38 executions in.
    //
    // The bound is exact enough to be safe: literals are copied 1:1 from the block, and a match
    // costs at least one extension byte per 255 output bytes, so no LZ4 block can expand by more
    // than 255x plus a constant.
    const std::size_t max_possible =
        size > (SIZE_MAX / 255U) - 1U ? SIZE_MAX : (size + 1U) * 255U;
    if (uncompressed_size > max_possible) {
        error = "LZ4 block declares " + std::to_string(uncompressed_size) +
                " uncompressed bytes, more than its " + std::to_string(size) +
                " compressed bytes could produce";
        return false;
    }
    out.reserve(uncompressed_size);
    std::size_t pos = 0;
    while (pos < size) {
        const auto token = data[pos++];

        std::size_t literal_length = token >> 4U;
        if (literal_length == 15U && !read_extended_length(data, size, pos, literal_length, error)) {
            return false;
        }
        if (literal_length > size - pos) {
            error = "LZ4 literal run runs past the end of the block";
            return false;
        }
        if (literal_length > uncompressed_size - out.size()) {
            error = "LZ4 literal run runs past the declared uncompressed size";
            return false;
        }
        out.insert(out.end(), data + pos, data + pos + literal_length);
        pos += literal_length;

        // The final sequence is literals only: no offset follows, and the block ends here.
        if (pos == size) {
            break;
        }
        if (size - pos < 2U) {
            error = "LZ4 block ends inside a match offset";
            return false;
        }
        const std::size_t offset = static_cast<std::size_t>(data[pos]) |
                                   (static_cast<std::size_t>(data[pos + 1U]) << 8U);
        pos += 2U;
        if (offset == 0U || offset > out.size()) {
            error = "LZ4 match offset points outside the data decoded so far";
            return false;
        }

        std::size_t match_length = token & 0x0FU;
        if (match_length == 15U && !read_extended_length(data, size, pos, match_length, error)) {
            return false;
        }
        match_length += kMinMatch;
        if (match_length > uncompressed_size - out.size()) {
            error = "LZ4 match runs past the declared uncompressed size";
            return false;
        }
        // Byte at a time, deliberately: LZ4 matches may overlap their own output (offset 1 with a
        // long match is how it spells a run), so a bulk copy would read bytes this loop is still
        // writing. `out` cannot reallocate mid-match -- the capacity was reserved above and the
        // length is bounded by uncompressed_size -- but indexing rather than holding a pointer keeps
        // that from being load-bearing.
        const auto start = out.size() - offset;
        for (std::size_t i = 0; i < match_length; ++i) {
            out.push_back(out[start + i]);
        }
    }
    if (out.size() != uncompressed_size) {
        error = "LZ4 block decoded " + std::to_string(out.size()) + " bytes, not the declared " +
                std::to_string(uncompressed_size);
        return false;
    }
    return true;
}

bool decompress_sized(const std::vector<std::uint8_t>& sized, std::vector<std::uint8_t>& out,
                      std::string& error) {
    if (sized.size() < 4U) {
        error = "LZ4 buffer is too short to hold its uncompressed size";
        return false;
    }
    const auto declared = load_le<std::uint32_t>(sized.data());
    if (declared > default_read_limits().max_uncompressed_bytes) {
        error = "LZ4 buffer declares " + std::to_string(declared) +
                " uncompressed bytes, over the decoded-size limit";
        return false;
    }
    return decompress_block(sized.data() + 4U, sized.size() - 4U, static_cast<std::size_t>(declared),
                            out, error);
}

}  // namespace nano_lance::lz4_block
