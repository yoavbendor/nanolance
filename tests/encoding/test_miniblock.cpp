// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// encoding/layout/miniblock: chunks written then split back, with and without definition levels, over one,
// two and three buffers of sizes 0, 1, 7, 8, 9 and 32760 (the 12-bit metadata word's limit); the exact
// header bytes and fill of the two shapes nanolance writes; chunk_bytes and chunk_header agreeing with
// append_chunk; metadata words both ways and a page's chunk counts; the readers of shapes nanolance does not
// write (u16 sizes, a repetition slot), hand-built; and the refusals: truncated headers, sizes past the
// payload, too many buffers, and metadata that does not add up.

#include "encoding/layout/miniblock.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

namespace mb = nano_lance::miniblock;

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

using Bytes = std::vector<std::uint8_t>;

/// `n` bytes, distinct per `seed`, with no zeros or fill bytes, so a misplaced byte shows.
Bytes bytes_of(std::size_t n, std::uint8_t seed) {
    Bytes out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<std::uint8_t>(1U + (seed * 37U + i * 11U) % 200U);
    }
    return out;
}

mb::ChunkShape written_shape(bool levels, std::uint32_t buffers) {
    mb::ChunkShape shape;
    shape.has_definition = levels;
    shape.num_buffers = buffers;
    shape.large_buffer_sizes = true;
    return shape;
}

void round_trip(std::span<const std::size_t> sizes, std::size_t def_bytes) {
    const auto label = "buffers " + std::to_string(sizes.size()) + " (first " + std::to_string(sizes[0]) +
                       "), def " + std::to_string(def_bytes);
    std::vector<Bytes> buffers;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        buffers.push_back(bytes_of(sizes[i], static_cast<std::uint8_t>(i + 1U)));
    }
    std::vector<std::span<const std::uint8_t>> views(buffers.begin(), buffers.end());
    const Bytes def = bytes_of(def_bytes, 9U);
    const std::size_t levels = def_bytes == 0U ? 0U : 1000U;

    // Two identical chunks back to back: the second must start exactly where the first one's footprint ends.
    Bytes page;
    mb::append_chunk(page, levels, def, views);
    const auto one = page.size();
    mb::append_chunk(page, levels, def, views);
    require(one % 8U == 0U && page.size() == 2U * one, label + ": chunks are padded to 8 and contiguous");
    require(mb::chunk_bytes(def_bytes, sizes) == one, label + ": chunk_bytes matches append_chunk");
    const auto header = mb::chunk_header(levels, def_bytes, sizes);
    require(Bytes(page.begin(), page.begin() + static_cast<std::ptrdiff_t>(header.size())) == header,
            label + ": chunk_header is append_chunk's header");

    std::vector<mb::ChunkView> chunks;
    std::string error;
    require(mb::split_payload(page, written_shape(def_bytes != 0U, static_cast<std::uint32_t>(sizes.size())), chunks,
                              error),
            label + ": split: " + error);
    require(chunks.size() == 2U, label + ": two chunks");
    for (const auto& chunk : chunks) {
        require(chunk.values == buffers[0], label + ": first buffer back");
        require(chunk.extra_buffers.size() + 1U == buffers.size(), label + ": buffer count");
        for (std::size_t i = 1; i < buffers.size(); ++i) {
            require(chunk.extra_buffers[i - 1U] == buffers[i], label + ": buffer " + std::to_string(i) + " back");
        }
        require(chunk.repdef == def, label + ": levels back");
        require(chunk.repdef_values == levels, label + ": level count");
        require(chunk.num_levels == levels, label + ": num_levels");
        require(chunk.rep.empty(), label + ": no repetition");
    }

    // Splitting again into the same vector refills it rather than appending.
    require(mb::split_payload(std::span(page).first(one),
                              written_shape(def_bytes != 0U, static_cast<std::uint32_t>(sizes.size())), chunks, error) &&
                chunks.size() == 1U && chunks[0].values == buffers[0],
            label + ": re-split into a used vector");
}

void test_round_trips() {
    for (const std::size_t first : {0U, 1U, 7U, 8U, 9U, 1000U, 32760U}) {
        for (const std::size_t def : {0U, 2U, 6U, 8U, 128U}) {
            const std::size_t one[1] = {first};
            round_trip(one, def);
            const std::size_t two[2] = {first, 3U};
            round_trip(two, def);
            const std::size_t three[3] = {5U, first, 0U};
            round_trip(three, def);
        }
    }
}

void test_exact_bytes() {
    // No levels, one buffer of 5: [u16 0][u32 5] fill to 8, then the values and fill to 8.
    Bytes out;
    const Bytes values = {1, 2, 3, 4, 5};
    mb::append_chunk(out, 7U, {}, values);
    const Bytes want = {0, 0, 5, 0, 0, 0, 0xFE, 0xFE, 1, 2, 3, 4, 5, 0xFE, 0xFE, 0xFE};
    require(out == want, "no-levels chunk bytes (num_levels is 0 without levels)");

    // Levels: [u16 levels][u16 def size][u32 value size], the levels padded, then the values padded.
    out.clear();
    const Bytes def = {9, 9};
    mb::append_chunk(out, 3U, def, values);
    const Bytes want_def = {3, 0, 2, 0, 5, 0, 0, 0, 9, 9, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE,
                            1, 2, 3, 4, 5, 0xFE, 0xFE, 0xFE};
    require(out == want_def, "levels chunk bytes");
}

void test_control_words() {
    for (const std::size_t bytes : {8U, 16U, 4096U, 32768U}) {
        for (std::size_t log = 1; log <= 15U; ++log) {
            const auto w = mb::control_word(bytes, std::size_t{1} << log, false);
            const auto back = mb::decode_control_word(w);
            require(back.chunk_bytes == bytes && back.log_values == log,
                    "control word " + std::to_string(bytes) + " / 2^" + std::to_string(log));
        }
        const auto last = mb::decode_control_word(mb::control_word(bytes, 777U, true));
        require(last.chunk_bytes == bytes && last.log_values == 0U, "last chunk's word has log2 0");
    }
    // The 12-bit limit of a u16 word: 4096 words of 8 bytes.
    require(mb::control_word(32768U, 2U, true) == 0xFFF0U, "largest u16 word");

    // A page: three chunks of 1024, 1024 and a tail of 5.
    const mb::ChunkMeta meta[3] = {{4104U, 1024U}, {4104U, 1024U}, {48U, 5U}};
    const auto control = mb::control_buffer(meta);
    require(control.size() == 12U, "u32 per chunk");
    std::vector<std::uint64_t> items;
    std::string error;
    require(mb::decode_chunk_items(control, true, 2053U, items, error), "page counts: " + error);
    require(items == std::vector<std::uint64_t>{1024U, 1024U, 5U}, "page counts back");
    require(mb::decode_control_word(control[8] | (control[9] << 8U)).chunk_bytes == 48U, "tail footprint");

    // u16 words, as stock Lance writes small pages.
    const Bytes small = {0x1A, 0x00, 0x30, 0x00};  // 2^10 values, then the last chunk
    require(mb::decode_chunk_items(small, false, 1500U, items, error) &&
                items == std::vector<std::uint64_t>{1024U, 476U},
            "u16 words: " + error);
}

void test_control_refusals() {
    std::vector<std::uint64_t> items;
    std::string error;
    require(!mb::decode_chunk_items({}, true, 1U, items, error), "empty metadata refused");
    const Bytes odd = {1, 2, 3};
    require(!mb::decode_chunk_items(odd, true, 1U, items, error), "metadata not whole words refused");
    const Bytes zero_log = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00};
    require(!mb::decode_chunk_items(zero_log, true, 10U, items, error), "a non-last chunk with log2 0 refused");
    const Bytes too_many = {0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // 1024 then the last
    require(!mb::decode_chunk_items(too_many, true, 100U, items, error), "more values than items refused");
    require(!mb::decode_chunk_items(too_many, true, 1024U, items, error), "an empty last chunk refused");
    require(mb::decode_chunk_items(too_many, true, 1025U, items, error), "exactly one left for the last chunk");
}

/// Shapes nanolance never writes, built by hand from the header's layout.
void test_foreign_shapes() {
    // u16 sizes, two buffers, no levels (an old nanolance file): [0][3][2] pad, [3 bytes] pad, [2 bytes] pad.
    const Bytes old = {0, 0, 3, 0, 2, 0, 0, 0, 1, 2, 3, 0, 0, 0, 0, 0, 4, 5, 0, 0, 0, 0, 0, 0};
    std::vector<mb::ChunkView> chunks;
    std::string error;
    require(mb::split_payload(old, mb::ChunkShape{}, chunks, error), "default shape: " + error);
    require(chunks.size() == 1U && chunks[0].values == Bytes{1, 2, 3} && chunks[0].extra_buffers.size() == 1U &&
                chunks[0].extra_buffers[0] == Bytes{4, 5},
            "default shape buffers");

    // A list page: repetition and definition slots, one u32 buffer.
    mb::ChunkShape list;
    list.has_repetition = true;
    list.has_definition = true;
    list.num_buffers = 1U;
    list.large_buffer_sizes = true;
    // [u16 4 levels][u16 rep 2][u16 def 2][u32 1] = 10, pad 16; rep pad 8; def pad 8; value pad 8.
    const Bytes page = {4, 0, 2, 0, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 7, 0, 0, 0, 0, 0, 0,
                        8, 8, 0, 0, 0, 0, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0};
    require(mb::split_payload(page, list, chunks, error), "list shape: " + error);
    require(chunks.size() == 1U && chunks[0].rep == Bytes{7, 7} && chunks[0].repdef == Bytes{8, 8} &&
                chunks[0].values == Bytes{42} && chunks[0].num_levels == 4U && chunks[0].repdef_values == 4U,
            "list shape buffers");

    mb::ChunkHeader header;
    require(mb::read_chunk_header(page, 0U, list, header, error) && header.chunk_bytes == page.size() &&
                header.rep_size == 2U && header.def_size == 2U && header.buffer_sizes == std::vector<std::uint32_t>{1U},
            "list header");
}

void test_refusals() {
    Bytes page;
    const Bytes values = bytes_of(100U, 1U);
    mb::append_chunk(page, 0U, {}, values);
    const auto shape = written_shape(false, 1U);
    std::vector<mb::ChunkView> chunks;
    std::string error;

    require(!mb::split_payload(std::span(page).first(4U), shape, chunks, error), "truncated header refused");
    require(!mb::split_payload(std::span(page).first(page.size() - 8U), shape, chunks, error),
            "values past the payload refused");
    require(error.find("exceeds") != std::string::npos, "the refusal says why: " + error);

    // A size that would wrap a 32-bit sum.
    Bytes huge = page;
    huge[2] = huge[3] = huge[4] = huge[5] = 0xFF;
    require(!mb::split_payload(huge, shape, chunks, error), "a 4 GiB size refused");

    mb::ChunkShape wide = shape;
    wide.num_buffers = 65U;
    mb::ChunkHeader header;
    require(!mb::read_chunk_header(page, 0U, wide, header, error), "65 buffers per chunk refused");
    require(!mb::read_chunk_header(page, page.size() + 1U, shape, header, error), "an offset past the end refused");

    // A second chunk that is cut short: the first one comes back, and the split fails.
    Bytes two = page;
    mb::append_chunk(two, 0U, {}, values);
    two.resize(two.size() - 8U);
    require(!mb::split_payload(two, shape, chunks, error) && chunks.size() == 1U,
            "a cut second chunk refused, the first kept");

    require(mb::split_payload({}, shape, chunks, error) && chunks.empty(), "an empty page has no chunks");
}

}  // namespace

int main() {
    test_round_trips();
    test_exact_bytes();
    test_control_words();
    test_control_refusals();
    test_foreign_shapes();
    test_refusals();
    std::cout << "miniblock: ok\n";
    return 0;
}
