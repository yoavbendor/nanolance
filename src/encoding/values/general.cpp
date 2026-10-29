// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Lance's general buffer compression: zstd framed as [u64 LE size][zstd frame], lz4 as [u32 LE size][LZ4 block].

#include "encoding/values/general.hpp"

#include "nanolance/lz4_block.hpp"
#include "nanolance/read_safety.hpp"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace nano_lance::general {

// Frame a raw buffer as Lance's general-compression payload: [u64 LE uncompressed size][zstd frame].
bool zstd_frame(const std::vector<std::uint8_t>& raw, int level, std::vector<std::uint8_t>& out,
                       std::string& error) {
    const auto bound = ZSTD_compressBound(raw.size());
    // resize() (not assign(n, 0)): the 8-byte header is filled byte-by-byte below and ZSTD_compress
    // unconditionally overwrites the rest, so this only needs to zero a growing size delta (or nothing,
    // if the caller reuses `out` across calls of similar or shrinking size) rather than always
    // re-touching all n bytes the way assign(n, val) is specified to.
    out.resize(8U + bound);
    const std::uint64_t uncompressed = raw.size();
    for (int i = 0; i < 8; ++i) {
        out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((uncompressed >> (8 * i)) & 0xFFU);
    }
    // Reused per-thread compression context: one-shot ZSTD_compress() allocates AND ZEROES a fresh
    // multi-hundred-KB context (hash/chain tables, window) on every call, which callgrind showed as
    // ~40% of a compressed float column's write instructions across its ~150 per-chunk calls -- far
    // more than the actual compression work. ZSTD_compressCCtx on a reused context skips that setup
    // cost and produces byte-identical output (same algorithm, same level).
    thread_local std::unique_ptr<ZSTD_CCtx, std::size_t (*)(ZSTD_CCtx*)> cctx(ZSTD_createCCtx(),
                                                                              &ZSTD_freeCCtx);
    const auto csize = cctx != nullptr
                           ? ZSTD_compressCCtx(cctx.get(), out.data() + 8U, bound, raw.data(), raw.size(), level)
                           : ZSTD_compress(out.data() + 8U, bound, raw.data(), raw.size(), level);
    if (ZSTD_isError(csize) != 0U) {
        error = std::string("zstd compress failed: ") + ZSTD_getErrorName(csize);
        return false;
    }
    out.resize(8U + csize);
    return true;
}

// Inverse of zstd_frame: [u64 LE uncompressed size][zstd frame] -> raw bytes.
bool lz4_frame(const std::vector<std::uint8_t>& raw, std::vector<std::uint8_t>& out, std::string& error) {
    return lz4_block::compress_sized(raw.data(), raw.size(), out, error);
}

bool zstd_unframe(const std::vector<std::uint8_t>& framed, std::vector<std::uint8_t>& out, std::string& error) {
    if (framed.size() < 8U) {
        error = "zstd frame shorter than size header";
        return false;
    }
    const std::uint64_t uncompressed = load_le<std::uint64_t>(framed.data());
    // The declared size decides the allocation, and it is written by the file -- as is the zstd frame's
    // own content size, so the cross-check below proves only that the two agree, not that either is
    // true. An 899-byte page declaring 8.4 GB passed both and was allocated; found by
    // tests/fuzz/fuzz_column_decode.cpp within 10,000 executions.
    //
    // The bound has to come from bytes that are actually present. zstd cannot expand more than its
    // best block allows: an RLE block is 4 bytes (3-byte header + the byte) for at most 128 KiB of
    // output, so no frame exceeds 32768x its compressed size. Measured: 512 MiB of zeros at level 22
    // compresses 32,732:1. LZ4 has the same kind of bound (255x) in lz4_block.cpp, for the same reason.
    const std::uint64_t compressed = static_cast<std::uint64_t>(framed.size() - 8U);
    if (uncompressed > default_read_limits().max_uncompressed_bytes || !fits_size_t(uncompressed) ||
        uncompressed > compressed * kZstdMaxExpansion) {
        error = "zstd frame declares " + std::to_string(uncompressed) + " uncompressed bytes from " +
                std::to_string(compressed) + " compressed, more than zstd can produce";
        return false;
    }
    const unsigned long long content =
        ZSTD_getFrameContentSize(framed.data() + 8U, framed.size() - 8U);
    if (content != ZSTD_CONTENTSIZE_UNKNOWN && content != ZSTD_CONTENTSIZE_ERROR &&
        content != uncompressed) {
        error = "zstd frame content size disagrees with declared size";
        return false;
    }
    // Past this size the declared length is no longer trusted with an up-front allocation: a frame
    // can legitimately claim 32768x its compressed size, so an 85 KiB page could still ask for
    // 2.8 GB before a byte of it was shown to exist (fuzz_column_decode found that too). Large
    // frames are streamed instead, and the buffer grows only as real output arrives.
    constexpr std::uint64_t kPreallocateUpTo = std::uint64_t{64} << 20U;
    if (uncompressed > kPreallocateUpTo) {
        thread_local std::unique_ptr<ZSTD_DStream, std::size_t (*)(ZSTD_DStream*)> stream(ZSTD_createDStream(),
                                                                                         &ZSTD_freeDStream);
        if (stream == nullptr || ZSTD_isError(ZSTD_initDStream(stream.get())) != 0U) {
            error = "zstd stream could not be created";
            return false;
        }
        out.clear();
        ZSTD_inBuffer in{framed.data() + 8U, framed.size() - 8U, 0};
        std::size_t produced = 0;
        std::size_t ret = 1;
        while (ret != 0U) {
            if (produced == out.size()) {
                if (produced == static_cast<std::size_t>(uncompressed)) {
                    error = "zstd frame decompresses past its declared size";
                    return false;
                }
                const auto grown = std::min<std::uint64_t>(uncompressed, std::max<std::uint64_t>(produced * 2U, kPreallocateUpTo));
                out.resize(static_cast<std::size_t>(grown));
            }
            ZSTD_outBuffer dst{out.data(), out.size(), produced};
            const auto before_in = in.pos;
            ret = ZSTD_decompressStream(stream.get(), &dst, &in);
            if (ZSTD_isError(ret) != 0U) {
                error = "zstd decompress failed";
                return false;
            }
            if (dst.pos == produced && in.pos == before_in && ret != 0U) {
                error = "zstd frame is truncated";
                return false;
            }
            produced = dst.pos;
        }
        if (produced != uncompressed) {
            error = "zstd frame decompressed to " + std::to_string(produced) + " bytes, not the declared " +
                    std::to_string(uncompressed);
            return false;
        }
        out.resize(produced);
        return true;
    }
    // resize() (not assign(n, 0)) so a reused `out` across many page calls of similar size isn't
    // re-zeroed every time — ZSTD_decompress below unconditionally overwrites all out.size() bytes.
    out.resize(static_cast<std::size_t>(uncompressed));
    // Reused per-thread decompression context: one-shot ZSTD_decompress() allocates and zeroes a fresh
    // context on every call; ZSTD_decompressDCtx on a reused context skips that setup per page (same
    // reuse rationale as the writer's zstd_frame, identical output).
    thread_local std::unique_ptr<ZSTD_DCtx, std::size_t (*)(ZSTD_DCtx*)> dctx(ZSTD_createDCtx(),
                                                                              &ZSTD_freeDCtx);
    const auto got = dctx != nullptr
                         ? ZSTD_decompressDCtx(dctx.get(), out.data(), out.size(), framed.data() + 8U,
                                               framed.size() - 8U)
                         : ZSTD_decompress(out.data(), out.size(), framed.data() + 8U, framed.size() - 8U);
    if (ZSTD_isError(got) != 0U || got != uncompressed) {
        error = "zstd decompress failed for variable-width column";
        return false;
    }
    return true;
}

bool decompress(const std::string& scheme, const std::uint8_t* data, std::size_t size,
                               std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    if (scheme.empty() || scheme == "none") {
        out.assign(data, data + size);
        return true;
    }
    if (size == 0U) {
        return true;  // Lance's decompressors leave an empty buffer empty
    }
    std::vector<std::uint8_t> framed;
    if (scheme == "lz4") {
        framed.assign(data, data + size);
        return lz4_block::decompress_sized(framed, out, error);
    }
    if (scheme != "zstd") {
        error = "unsupported buffer compression '" + scheme + "'";
        return false;
    }
    // [u64 raw size][frame], or from older Lance a bare frame; Lance tells them apart the same way
    // (a frame's magic, then its reserved bit).
    static constexpr std::uint8_t kZstdMagic[4] = {0x28U, 0xB5U, 0x2FU, 0xFDU};
    const bool bare = size >= 5U && std::memcmp(data, kZstdMagic, 4U) == 0 && (data[4] & 0x08U) == 0U;
    if (!bare) {
        framed.assign(data, data + size);
        return zstd_unframe(framed, out, error);
    }
    const unsigned long long content = ZSTD_getFrameContentSize(data, size);
    if (content != ZSTD_CONTENTSIZE_UNKNOWN && content != ZSTD_CONTENTSIZE_ERROR) {
        framed.resize(size + 8U);
        const std::uint64_t declared = content;
        std::memcpy(framed.data(), &declared, 8U);
        std::memcpy(framed.data() + 8U, data, size);
        return zstd_unframe(framed, out, error);
    }
    // A frame that does not say how large it is: stream it, growing the output only as bytes arrive
    // and never past the bound zstd itself can produce from this many compressed bytes.
    const std::uint64_t bound = std::min<std::uint64_t>(default_read_limits().max_uncompressed_bytes,
                                                        static_cast<std::uint64_t>(size) * kZstdMaxExpansion);
    std::unique_ptr<ZSTD_DStream, std::size_t (*)(ZSTD_DStream*)> stream(ZSTD_createDStream(), &ZSTD_freeDStream);
    if (stream == nullptr || ZSTD_isError(ZSTD_initDStream(stream.get())) != 0U) {
        error = "zstd stream could not be created";
        return false;
    }
    ZSTD_inBuffer in{data, size, 0};
    std::size_t produced = 0;
    std::size_t ret = 1;
    while (ret != 0U) {
        if (produced == out.size()) {
            if (produced >= bound) {
                error = "zstd buffer decompresses past what its size allows";
                return false;
            }
            out.resize(static_cast<std::size_t>(std::min<std::uint64_t>(bound, std::max<std::size_t>(produced * 2U, 1U << 16U))));
        }
        ZSTD_outBuffer dst{out.data(), out.size(), produced};
        const auto before_in = in.pos;
        ret = ZSTD_decompressStream(stream.get(), &dst, &in);
        if (ZSTD_isError(ret) != 0U) {
            error = "zstd decompress failed";
            return false;
        }
        if (dst.pos == produced && in.pos == before_in && ret != 0U) {
            error = "zstd buffer is truncated";
            return false;
        }
        produced = dst.pos;
    }
    out.resize(produced);
    return true;
}

}  // namespace nano_lance::general
