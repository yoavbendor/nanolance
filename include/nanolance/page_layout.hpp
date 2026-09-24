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
    kFsst = 6,               ///< f6  Fsst{ f1 symbol_table, f2 values }
    kRle = 8,                ///< f8  Rle{ f1 values, f2 lengths }
    kByteStreamSplit = 9,    ///< f9  ByteStreamSplit{ f1 values }
    kGeneral = 10,           ///< f10 General{ f1 BufferCompression{ f1 scheme }, f3 values }
    /// f11 FixedSizeList{ f1 items_per_value, f2 values, f3 has_validity }. How Lance stores an Arrow
    /// fixed_size_list: NOT with repetition levels, but as a wrapper saying "every N consecutive
    /// values are one row". The schema has no child field for it either.
    kFixedSizeList = 11,
};

/// Buffer-compression schemes inside `General`, numbered as Lance's `CompressionScheme` enum does.
/// Only the ones nanolance can actually decode are named; anything else stays kUnknownScheme so the
/// caller refuses rather than mis-decodes.
enum class BufferScheme {
    kNone = 0,
    kLz4 = 1,
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

    /// Fsst: the serialized symbol table (a fixed 2312 bytes; see nanolance/fsst.hpp). Carried in
    /// the descriptor rather than the page, because one table covers the whole page.
    std::vector<std::uint8_t> symbol_table;

    /// FixedSizeList: values per row, and whether the list itself carries a validity buffer.
    std::uint64_t items_per_value = 0;
    bool has_validity = false;

    /// Variable::offsets, ByteStreamSplit::values, General::values, Rle::values, Fsst::values,
    /// FixedSizeList::values.
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
    /// f1 `rep_compression` was present. A chunk header carries a `u16 rep_size` slot when it
    /// exists, so missing it would shift every later field by two bytes.
    bool has_repetition = false;
    /// f1 itself: how the repetition levels are stored (`Bitpacked{16, Flat(bits)}` for a list).
    std::unique_ptr<Compressive> rep_compression;
    /// f8 `repetition_index_depth`: how many list levels the page's repetition index covers.
    std::uint32_t repetition_index_depth = 0;
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
///
/// A constant page can ALSO be nullable -- the same value in every non-null row -- and then it
/// carries definition levels in a buffer of their own. Lance's buffer map (see
/// `ConstantPageScheduler::try_new`) is decided by the inline value's presence and the buffer count
/// together, and there are exactly four legal combinations:
///
///     inline, 0 buffers  -> the value, no levels
///     inline, 2 buffers  -> the value; buffer 0 = rep, buffer 1 = def
///     no inline, 1       -> buffer 0 = the value, no levels
///     no inline, 3       -> buffer 0 = the value; buffer 1 = rep, buffer 2 = def
///
/// A zero-length rep or def buffer means that layer is absent.
struct Constant {
    std::optional<std::vector<std::uint8_t>> inline_value;
    /// f5 `layers`, raw. [3] with no inline value is how Lance spells an all-null column.
    std::vector<std::uint8_t> layers;
    /// f7 / f8: how the repetition / definition buffers are compressed. Absent means the levels are
    /// stored as raw u16 values -- which is a spelling the miniblock path never has to handle,
    /// because there a `CompressiveEncoding` is always present.
    std::unique_ptr<Compressive> rep_compression;
    std::unique_ptr<Compressive> def_compression;
    /// f9 / f10: how many levels each buffer holds once decompressed.
    std::uint64_t num_rep_values = 0;
    std::uint64_t num_def_values = 0;
};

/// PageLayout f3. The layout Lance uses when a page's values are large: any value of 256 bytes or
/// more (`MINIBLOCK_MAX_BYTE_LENGTH_PER_VALUE`), which is one long string in a column, or a float32
/// fixed_size_list of 64+ dimensions. Values are stored row by row, each preceded by a control word
/// holding its repetition/definition levels, instead of being packed into mini-blocks.
struct FullZip {
    std::uint32_t bits_rep = 0;
    std::uint32_t bits_def = 0;
    /// Exactly one of these is set: a fixed-width page stores `bits_per_value`, a variable-width one
    /// stores the width of each value's length prefix in `bits_per_offset`.
    std::uint32_t bits_per_value = 0;
    std::uint32_t bits_per_offset = 0;
    std::uint64_t num_items = 0;
    std::uint64_t num_visible_items = 0;
    std::unique_ptr<Compressive> value_compression;
    std::vector<std::uint8_t> layers;
};

enum class LayoutKind { kNone, kMiniBlock, kConstant, kFullZip };

struct PageLayout {
    LayoutKind kind = LayoutKind::kNone;
    MiniBlock mini_block;
    Constant constant;
    FullZip full_zip;
    /// Set when the descriptor named a layout this build does not model (Blob, Sparse, ...), so
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

/// The same, for one encoding subtree -- so a refusal deep in the decode path can name the exact
/// node it does not implement instead of describing the symptom.
std::string describe_encoding(const Compressive& node);

}  // namespace nano_lance::page_layout
