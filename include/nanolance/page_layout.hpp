// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include "lance_minimal.pb.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/// Typed view of the `/lance.encodings21.PageLayout` descriptor carried in `ColumnPage`'s field 4.
///
/// WHY THIS EXISTS. nanolance's writer already emits this descriptor for every page -- it is why
/// stock Lance can read nanolance files at all. The reader never looked at it: `decode_column_page`
/// parsed fields 1, 2, 3 and 5 and skipped field 4 outright, and
/// `decode_lance_physical_column` branched instead on nanolance-private field metadata
/// (`nanolance:packing` = constant / rle / dict / dict-rle / bitpack / bss-zstd). A file written by
/// the Rust `lance` crate carries none of those keys, so it fell through to a flat-page assumption
/// that did not match and died on a size check -- which is why a plain 5000-row int64 column from
/// pylance failed to decode while nanolance's own bitpacked int64 column decoded fine. The two are
/// the SAME encoding: pylance's descriptor and nanolance's are byte-identical apart from the chunk
/// row count, both `CompressiveEncoding{ f5 InlineBitpacking{ bits = 64 } }`.
///
/// Reading the descriptor therefore turns "implement a general Lance reader" into "dispatch on what
/// is already in the file". This header is the parse; re-rooting decode onto it comes next.
///
/// TRUST. These bytes are untrusted input like everything else on the read path. The parser is
/// bounds-checked, rejects malformed varints and truncated submessages, and caps nesting depth so a
/// hostile descriptor cannot drive unbounded recursion. It allocates only small fixed-shape nodes --
/// no length field here drives an allocation.
namespace nano_lance::page_layout {

/// The set of compressive encodings Lance's `encodings_v2_1.proto` defines, as observed in real
/// files. Field numbers are the protobuf tags inside `CompressiveEncoding`; they are not guessed --
/// each one is the inverse of an encoder in `data_file_writer.cpp` that was itself verified
/// byte-for-byte against a `lance`-written file.
enum class CompressiveKind {
    kUnknown = 0,   ///< a variant this build does not model; decode must refuse, never guess
    kFlat = 1,      ///< f1  Flat{ f1 bits_per_value }
    kVariable = 2,  ///< f2  Variable{ f1 offsets }
    kBitpacked = 4,          ///< f4  Bitpacked{ f1 uncompressed_bits_per_value, f3 values }
    kInlineBitpacking = 5,   ///< f5  InlineBitpacking{ f1 uncompressed_bits_per_value }
    kRle = 8,                ///< f8  Rle{ f1 values, f2 lengths }
    kByteStreamSplit = 9,    ///< f9  ByteStreamSplit{ f1 values }
    kGeneral = 10,           ///< f10 General{ f1 BufferCompression{ f1 scheme }, f3 values }
};

/// Buffer-compression schemes inside `General`. Only the ones nanolance can actually decode are
/// named; anything else stays kUnknownScheme so the caller refuses rather than mis-decodes.
enum class BufferScheme {
    kNone = 0,
    kZstd = 2,
    kUnknownScheme = 255,
};

/// One node of the CompressiveEncoding tree. Children are owned; the tree is small (a handful of
/// nodes per page) and built once per page, not per value.
struct Compressive {
    CompressiveKind kind = CompressiveKind::kUnknown;
    /// Raw field number as it appeared on the wire. Preserved even for kUnknown so an error message
    /// can name the variant it refused ("unsupported CompressiveEncoding variant 7") instead of
    /// reporting a downstream size mismatch.
    std::uint32_t wire_field = 0;

    /// Flat / InlineBitpacking: bits per value (Flat) or uncompressed bits per value (bitpacking).
    std::uint32_t bits_per_value = 0;
    /// General: the buffer compression scheme applied to `values`.
    BufferScheme scheme = BufferScheme::kNone;
    /// The scheme's raw enum value as it appeared on the wire, kept for the same reason as
    /// `wire_field`: an unsupported scheme should be refused by number, not silently treated as raw.
    std::uint32_t wire_scheme = 0;

    /// Variable::offsets, ByteStreamSplit::values, General::values, Rle::values.
    std::unique_ptr<Compressive> values;
    /// Rle::lengths.
    std::unique_ptr<Compressive> lengths;
};

/// PageLayout f1. `value_compression` describes the chunk payload; `dictionary` is present for
/// dictionary-encoded pages, with `num_dictionary_items` entries.
struct MiniBlock {
    std::unique_ptr<Compressive> value_compression;
    std::unique_ptr<Compressive> dictionary;
    /// f2: how the repetition/definition layer is stored, when there is one. A simple nullable
    /// column uses `CompressiveEncoding` field 4 (bit-width wrapper) around `Flat(1)`; a column whose
    /// nulls come in runs uses `Rle{Flat(16), Flat(8)}`.
    std::unique_ptr<Compressive> repdef_compression;
    std::uint64_t num_dictionary_items = 0;
    std::uint64_t num_items = 0;
    std::uint32_t num_buffers = 0;
    bool has_large_chunk = false;
    /// f6 `layers`: [1] for a column with no nulls, [3] when a definition-level layer is present.
    /// Stored raw because it is a length-delimited field rather than a plain varint.
    std::vector<std::uint8_t> layers;
};

/// Does this layer set declare a definition-level layer? Lance writes [3] for a nullable column and
/// [1] for one with no nulls.
inline bool layers_have_definition_levels(const std::vector<std::uint8_t>& layers) {
    for (const auto layer : layers) {
        if (layer == 3U) {
            return true;
        }
    }
    return false;
}

/// PageLayout f2. A fixed-width constant carries its value inline in the descriptor (no data
/// buffers); a variable-width constant omits `inline_value` and stores the single value in a buffer.
struct Constant {
    std::optional<std::vector<std::uint8_t>> inline_value;
    /// f5 `layers`, raw. [3] with no inline value is how Lance spells an all-null column.
    std::vector<std::uint8_t> layers;
};

enum class LayoutKind { kNone, kMiniBlock, kConstant };

struct PageLayout {
    LayoutKind kind = LayoutKind::kNone;
    MiniBlock mini_block;
    Constant constant;
    /// Set when the descriptor named a layout this build does not model (FullZip, AllNull, ...), so
    /// the caller can refuse by name rather than misinterpret the page's buffers.
    std::uint32_t unknown_layout_field = 0;
};

/// Parse `ColumnPage::encoding` -- the `/lance.encodings21.PageLayout` type-url wrapper plus its
/// payload. Returns false with `error` set on malformed input. An empty `encoding` is NOT an error:
/// `out.kind` stays kNone so callers can fall back (older nanolance files predate the descriptor
/// being read, and it costs nothing to keep reading them).
bool decode_page_layout(const std::vector<std::uint8_t>& encoding, PageLayout& out, std::string& error);

/// Human-readable one-line summary, for error messages and the differential test's diagnostics.
std::string describe(const PageLayout& layout);

}  // namespace nano_lance::page_layout
