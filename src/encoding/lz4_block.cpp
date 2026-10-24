// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Lance's LZ4 envelope over nanom's LZ4 block decoder (nanom/codec.hpp), the one parquet2nanoarrow
// reads LZ4_RAW pages with. This file keeps only what is Lance's: the [u32 size] prefix and the size
// checks made before anything is allocated.

#include "nanolance/lz4_block.hpp"

#include "nanolance/read_safety.hpp"

#include <nanom/codec.hpp>

#include <cstddef>
#include <span>

namespace nano_lance::lz4_block {

bool decompress_block(const std::uint8_t* data, std::size_t size, std::size_t uncompressed_size,
                      std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    // The declared size decides the allocation, so it has to be checked against what this block could
    // possibly produce BEFORE allocating -- otherwise an 8-byte buffer declaring 3.7 GB makes the
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
    if (size == 0U) {  // nothing to decode: valid only when nothing was promised
        if (uncompressed_size != 0U) {
            error = "LZ4 block is empty but declares " + std::to_string(uncompressed_size) + " bytes";
            return false;
        }
        return true;
    }
    out.resize(uncompressed_size);
    const auto produced = ::nanom::codec::lz4_block_decompress(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size),
        std::span<std::byte>(reinterpret_cast<std::byte*>(out.data()), out.size()));
    if (!produced) {
        error = std::string("LZ4 block is malformed: ") + produced.error().what + " at byte " +
                std::to_string(produced.error().at);
        out.clear();
        return false;
    }
    if (*produced != uncompressed_size) {
        error = "LZ4 block decoded " + std::to_string(*produced) + " bytes, not the declared " +
                std::to_string(uncompressed_size);
        out.clear();
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
