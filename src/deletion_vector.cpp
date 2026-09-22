// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/deletion_vector.hpp"

#include "nanolance/read_safety.hpp"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>

namespace nano_lance {
namespace {

// --- little-endian readers, all bounds-checked --------------------------------------------------
//
// Every one takes the whole buffer and an absolute offset rather than a pointer, so a short or
// hostile file is a `false` rather than an over-read. These files come from disk and the manifest
// that names them is untrusted, exactly like the data files.

bool read_u16(const std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t& out) {
    if (at + 2U > b.size()) {
        return false;
    }
    out = static_cast<std::uint16_t>(b[at]) | static_cast<std::uint16_t>(b[at + 1U] << 8U);
    return true;
}

bool read_u32(const std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t& out) {
    if (at + 4U > b.size()) {
        return false;
    }
    out = static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1U]) << 8U) |
          (static_cast<std::uint32_t>(b[at + 2U]) << 16U) | (static_cast<std::uint32_t>(b[at + 3U]) << 24U);
    return true;
}

bool read_i32(const std::vector<std::uint8_t>& b, std::size_t at, std::int32_t& out) {
    std::uint32_t raw = 0;
    if (!read_u32(b, at, raw)) {
        return false;
    }
    std::memcpy(&out, &raw, sizeof(out));
    return true;
}

bool read_i64(const std::vector<std::uint8_t>& b, std::size_t at, std::int64_t& out) {
    if (at + 8U > b.size()) {
        return false;
    }
    std::uint64_t raw = 0;
    for (int i = 0; i < 8; ++i) {
        raw |= static_cast<std::uint64_t>(b[at + static_cast<std::size_t>(i)]) << (8U * static_cast<unsigned>(i));
    }
    std::memcpy(&out, &raw, sizeof(out));
    return true;
}

// --- minimal flatbuffer table reader -------------------------------------------------------------
//
// Arrow IPC metadata is flatbuffers. Only two tables are needed (Message and RecordBatch) and only a
// handful of their fields, so this reads the vtable directly rather than pulling in a generator.
//
// A flatbuffer table is: [soffset_t to vtable][field data...]. The vtable is
// [u16 vtable_bytes][u16 table_bytes][u16 offset per field], where a 0 offset means "absent, use the
// default". Field ids are positional and stable, which is what makes this safe to hand-roll.

/// Absolute offset of `field_id` within the table at `table`, or 0 when the field is absent.
bool flatbuffer_field(const std::vector<std::uint8_t>& b, std::size_t table, std::size_t field_id,
                      std::size_t& out_absolute) {
    out_absolute = 0;
    std::int32_t vtable_delta = 0;
    if (!read_i32(b, table, vtable_delta)) {
        return false;
    }
    // The soffset points BACKWARDS from the table to its vtable.
    const auto signed_table = static_cast<std::int64_t>(table);
    const auto vtable_signed = signed_table - vtable_delta;
    if (vtable_signed < 0 || static_cast<std::uint64_t>(vtable_signed) > b.size()) {
        return false;
    }
    const auto vtable = static_cast<std::size_t>(vtable_signed);
    std::uint16_t vtable_bytes = 0;
    if (!read_u16(b, vtable, vtable_bytes) || vtable_bytes < 4U) {
        return false;
    }
    const std::size_t slot = 4U + field_id * 2U;
    if (slot + 2U > vtable_bytes) {
        return true;  // absent: the vtable is simply shorter than this field id
    }
    std::uint16_t field_offset = 0;
    if (!read_u16(b, vtable + slot, field_offset)) {
        return false;
    }
    if (field_offset == 0U) {
        return true;  // explicitly absent
    }
    out_absolute = table + field_offset;
    return out_absolute <= b.size();
}

/// Follow a uoffset stored at `at` (vectors and sub-tables are referenced indirectly).
bool flatbuffer_indirect(const std::vector<std::uint8_t>& b, std::size_t at, std::size_t& out) {
    std::uint32_t delta = 0;
    if (!read_u32(b, at, delta)) {
        return false;
    }
    out = at + delta;
    return out <= b.size();
}

constexpr std::size_t kArrowIpcAlignment = 8U;
constexpr std::uint32_t kIpcContinuation = 0xFFFFFFFFU;
constexpr std::int8_t kCompressionZstd = 1;  // Arrow CompressionType: 0 = LZ4_FRAME, 1 = ZSTD

/// One Arrow IPC buffer, which may carry an int64 uncompressed-length prefix.
///
/// With body compression on, Arrow prefixes every buffer with its uncompressed length; a length of
/// -1 means "this one did not compress, the rest is raw". Lance always asks for ZSTD, but small
/// buffers routinely come back as -1, so both spellings occur in practice.
bool materialize_ipc_buffer(const std::vector<std::uint8_t>& body, std::size_t offset, std::size_t length,
                            bool compressed, std::vector<std::uint8_t>& out, std::string& error) {
    if (offset > body.size() || length > body.size() - offset) {
        error = "deletion file buffer runs past the message body";
        return false;
    }
    if (!compressed) {
        out.assign(body.begin() + static_cast<std::ptrdiff_t>(offset),
                   body.begin() + static_cast<std::ptrdiff_t>(offset + length));
        return true;
    }
    if (length == 0U) {
        out.clear();
        return true;
    }
    std::int64_t uncompressed = 0;
    if (length < 8U || !read_i64(body, offset, uncompressed)) {
        error = "deletion file buffer is too short for its compression prefix";
        return false;
    }
    const auto payload = offset + 8U;
    const auto payload_len = length - 8U;
    if (uncompressed < 0) {
        out.assign(body.begin() + static_cast<std::ptrdiff_t>(payload),
                   body.begin() + static_cast<std::ptrdiff_t>(payload + payload_len));
        return true;
    }
    const auto& limits = default_read_limits();
    if (static_cast<std::uint64_t>(uncompressed) > limits.max_uncompressed_bytes) {
        error = "deletion file declares an implausible uncompressed size";
        return false;
    }
    out.assign(static_cast<std::size_t>(uncompressed), 0U);
    const auto produced = ZSTD_decompress(out.data(), out.size(), body.data() + payload, payload_len);
    if (ZSTD_isError(produced) || produced != out.size()) {
        error = "failed to decompress a deletion file buffer";
        return false;
    }
    return true;
}

}  // namespace

bool parse_arrow_ipc_uint32_column(const std::vector<std::uint8_t>& bytes,
                                   std::vector<std::uint32_t>& out_values, std::string& error) {
    out_values.clear();
    error.clear();

    // An Arrow IPC *file* is "ARROW1\0\0", then the same encapsulated messages a stream uses, then a
    // footer. Only the first RecordBatch is needed, so the footer is never reached.
    static constexpr std::uint8_t kMagic[] = {'A', 'R', 'R', 'O', 'W', '1', 0, 0};
    if (bytes.size() < sizeof(kMagic) || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0) {
        error = "deletion file is not an Arrow IPC file";
        return false;
    }

    // The magic is followed by zero padding to the writer's alignment -- pyarrow and arrow-rs pad to
    // 64, not the 8 the format minimally requires -- so the first message is not at a fixed offset.
    // Skip forward over ZEROS only (never arbitrary bytes) to the first continuation marker.
    std::size_t pos = sizeof(kMagic);
    while (pos + 8U <= bytes.size()) {
        std::uint32_t peek = 0;
        if (!read_u32(bytes, pos, peek) || peek == kIpcContinuation) {
            break;
        }
        if (peek != 0U) {
            error = "unexpected bytes between the Arrow IPC magic and its first message";
            return false;
        }
        pos += 8U;
    }

    while (pos + 8U <= bytes.size()) {
        std::uint32_t continuation = 0;
        if (!read_u32(bytes, pos, continuation)) {
            error = "truncated Arrow IPC message";
            return false;
        }
        if (continuation != kIpcContinuation) {
            break;  // the footer, or end of the message stream
        }
        std::uint32_t metadata_len = 0;
        if (!read_u32(bytes, pos + 4U, metadata_len)) {
            error = "truncated Arrow IPC message header";
            return false;
        }
        if (metadata_len == 0U) {
            break;  // end-of-stream marker
        }
        const std::size_t metadata_at = pos + 8U;
        if (metadata_len > bytes.size() - metadata_at) {
            error = "Arrow IPC message header runs past the file";
            return false;
        }

        std::size_t message = 0;
        if (!flatbuffer_indirect(bytes, metadata_at, message)) {
            error = "malformed Arrow IPC message";
            return false;
        }
        // Message: version(0), header_type(1), header(2), bodyLength(3).
        std::size_t header_type_at = 0;
        if (!flatbuffer_field(bytes, message, 1U, header_type_at)) {
            error = "malformed Arrow IPC message header";
            return false;
        }
        const std::uint8_t header_type = header_type_at == 0U ? 0U : bytes[header_type_at];

        std::size_t body_length_at = 0;
        std::int64_t body_length = 0;
        if (!flatbuffer_field(bytes, message, 3U, body_length_at)) {
            error = "malformed Arrow IPC body length";
            return false;
        }
        if (body_length_at != 0U && !read_i64(bytes, body_length_at, body_length)) {
            error = "malformed Arrow IPC body length";
            return false;
        }
        if (body_length < 0) {
            error = "negative Arrow IPC body length";
            return false;
        }

        const std::size_t padded_metadata =
            (metadata_at + metadata_len + kArrowIpcAlignment - 1U) & ~(kArrowIpcAlignment - 1U);
        if (padded_metadata > bytes.size() ||
            static_cast<std::uint64_t>(body_length) > bytes.size() - padded_metadata) {
            error = "Arrow IPC message body runs past the file";
            return false;
        }

        // MessageHeader: 1 = Schema, 3 = RecordBatch. Anything else (dictionaries, tensors) is not
        // something Lance writes here.
        if (header_type == 3U) {
            std::size_t record_batch = 0;
            std::size_t header_at = 0;
            if (!flatbuffer_field(bytes, message, 2U, header_at) || header_at == 0U ||
                !flatbuffer_indirect(bytes, header_at, record_batch)) {
                error = "malformed Arrow IPC record batch";
                return false;
            }

            // RecordBatch: length(0), nodes(1), buffers(2), compression(3).
            std::size_t length_at = 0;
            std::int64_t rows = 0;
            if (!flatbuffer_field(bytes, record_batch, 0U, length_at) || length_at == 0U ||
                !read_i64(bytes, length_at, rows) || rows < 0) {
                error = "malformed Arrow IPC record batch length";
                return false;
            }

            bool compressed = false;
            std::size_t compression_at = 0;
            if (!flatbuffer_field(bytes, record_batch, 3U, compression_at)) {
                error = "malformed Arrow IPC compression";
                return false;
            }
            if (compression_at != 0U) {
                std::size_t compression = 0;
                if (!flatbuffer_indirect(bytes, compression_at, compression)) {
                    error = "malformed Arrow IPC compression";
                    return false;
                }
                std::size_t codec_at = 0;
                if (!flatbuffer_field(bytes, compression, 0U, codec_at)) {
                    error = "malformed Arrow IPC compression codec";
                    return false;
                }
                const auto codec = codec_at == 0U ? 0 : static_cast<std::int8_t>(bytes[codec_at]);
                if (codec != kCompressionZstd) {
                    error = "deletion file uses an Arrow IPC compression codec nanolance does not read";
                    return false;
                }
                compressed = true;
            }

            std::size_t buffers_at = 0;
            if (!flatbuffer_field(bytes, record_batch, 2U, buffers_at) || buffers_at == 0U) {
                error = "Arrow IPC record batch has no buffers";
                return false;
            }
            std::size_t buffers = 0;
            if (!flatbuffer_indirect(bytes, buffers_at, buffers)) {
                error = "malformed Arrow IPC buffer vector";
                return false;
            }
            std::uint32_t buffer_count = 0;
            if (!read_u32(bytes, buffers, buffer_count) || buffer_count < 2U) {
                // validity + values for one column; Lance writes the column non-null, but Arrow still
                // reserves the validity slot.
                error = "Arrow IPC record batch does not look like one uint32 column";
                return false;
            }

            // Buffer is an inline struct {int64 offset, int64 length}; the values buffer is index 1.
            const std::size_t values_entry = buffers + 4U + 16U;
            std::int64_t values_offset = 0;
            std::int64_t values_length = 0;
            if (!read_i64(bytes, values_entry, values_offset) ||
                !read_i64(bytes, values_entry + 8U, values_length) || values_offset < 0 || values_length < 0) {
                error = "malformed Arrow IPC values buffer descriptor";
                return false;
            }

            const std::vector<std::uint8_t> body(
                bytes.begin() + static_cast<std::ptrdiff_t>(padded_metadata),
                bytes.begin() + static_cast<std::ptrdiff_t>(padded_metadata + static_cast<std::size_t>(body_length)));
            std::vector<std::uint8_t> values;
            if (!materialize_ipc_buffer(body, static_cast<std::size_t>(values_offset),
                                        static_cast<std::size_t>(values_length), compressed, values, error)) {
                return false;
            }
            if (values.size() < static_cast<std::uint64_t>(rows) * 4U) {
                error = "Arrow IPC values buffer is shorter than its row count";
                return false;
            }
            out_values.resize(static_cast<std::size_t>(rows));
            for (std::int64_t i = 0; i < rows; ++i) {
                std::uint32_t v = 0;
                std::memcpy(&v, values.data() + static_cast<std::size_t>(i) * 4U, sizeof(v));
                out_values[static_cast<std::size_t>(i)] = v;
            }
            return true;
        }

        pos = padded_metadata + static_cast<std::size_t>(body_length);
    }

    error = "Arrow IPC file holds no record batch";
    return false;
}

bool parse_roaring_bitmap(const std::vector<std::uint8_t>& bytes,
                          std::vector<std::uint32_t>& out_sorted_values, std::string& error) {
    out_sorted_values.clear();
    error.clear();

    // Portable roaring serialization. Two cookies exist: SERIAL_COOKIE carries the container count in
    // its high half and is followed by a run-container bitmap; SERIAL_COOKIE_NO_RUNCONTAINER is
    // followed by a u32 count and never has run containers.
    constexpr std::uint32_t kCookieWithRuns = 12347U;
    constexpr std::uint32_t kCookieNoRuns = 12346U;
    constexpr std::uint32_t kNoOffsetThreshold = 4U;
    constexpr std::size_t kBitmapContainerBytes = 8192U;

    std::uint32_t cookie = 0;
    if (!read_u32(bytes, 0, cookie)) {
        error = "deletion bitmap is truncated";
        return false;
    }
    std::size_t pos = 4U;
    std::uint32_t container_count = 0;
    std::vector<std::uint8_t> run_flags;
    const bool has_runs = (cookie & 0xFFFFU) == kCookieWithRuns;
    if (has_runs) {
        container_count = ((cookie >> 16U) & 0xFFFFU) + 1U;
        const std::size_t flag_bytes = (container_count + 7U) / 8U;
        if (pos + flag_bytes > bytes.size()) {
            error = "deletion bitmap run header is truncated";
            return false;
        }
        run_flags.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                         bytes.begin() + static_cast<std::ptrdiff_t>(pos + flag_bytes));
        pos += flag_bytes;
    } else if (cookie == kCookieNoRuns) {
        if (!read_u32(bytes, pos, container_count)) {
            error = "deletion bitmap is truncated";
            return false;
        }
        pos += 4U;
    } else {
        error = "deletion bitmap has an unrecognized cookie";
        return false;
    }

    // A fragment cannot have more deleted rows than u32 can address, so the container count is
    // bounded by 65536 (one per high-16 key) regardless of what the header claims.
    if (container_count > 65536U) {
        error = "deletion bitmap declares an implausible container count";
        return false;
    }

    struct Descriptor {
        std::uint16_t key = 0;
        std::uint32_t cardinality = 0;
    };
    std::vector<Descriptor> descriptors(container_count);
    for (std::uint32_t i = 0; i < container_count; ++i) {
        std::uint16_t key = 0;
        std::uint16_t cardinality_minus_one = 0;
        if (!read_u16(bytes, pos, key) || !read_u16(bytes, pos + 2U, cardinality_minus_one)) {
            error = "deletion bitmap descriptor header is truncated";
            return false;
        }
        descriptors[i].key = key;
        descriptors[i].cardinality = static_cast<std::uint32_t>(cardinality_minus_one) + 1U;
        pos += 4U;
    }

    // The offset header is present for the no-run cookie always, and for the run cookie only once
    // there are enough containers to be worth indexing. Containers follow in order either way, so it
    // is skipped rather than used.
    if (!has_runs || container_count >= kNoOffsetThreshold) {
        const std::size_t offset_bytes = static_cast<std::size_t>(container_count) * 4U;
        if (offset_bytes > bytes.size() - std::min(pos, bytes.size())) {
            error = "deletion bitmap offset header is truncated";
            return false;
        }
        pos += offset_bytes;
    }

    for (std::uint32_t i = 0; i < container_count; ++i) {
        const std::uint32_t base = static_cast<std::uint32_t>(descriptors[i].key) << 16U;
        const bool is_run =
            has_runs && (run_flags[i / 8U] & static_cast<std::uint8_t>(1U << (i % 8U))) != 0U;
        if (is_run) {
            std::uint16_t run_count = 0;
            if (!read_u16(bytes, pos, run_count)) {
                error = "deletion bitmap run container is truncated";
                return false;
            }
            pos += 2U;
            for (std::uint16_t r = 0; r < run_count; ++r) {
                std::uint16_t start = 0;
                std::uint16_t length_minus_one = 0;
                if (!read_u16(bytes, pos, start) || !read_u16(bytes, pos + 2U, length_minus_one)) {
                    error = "deletion bitmap run container is truncated";
                    return false;
                }
                pos += 4U;
                for (std::uint32_t v = start; v <= static_cast<std::uint32_t>(start) + length_minus_one; ++v) {
                    out_sorted_values.push_back(base | v);
                }
            }
        } else if (descriptors[i].cardinality > 4096U) {
            // Bitmap container: a fixed 8192-byte bitset, one bit per value in the 16-bit key space.
            if (pos + kBitmapContainerBytes > bytes.size()) {
                error = "deletion bitmap container is truncated";
                return false;
            }
            for (std::uint32_t bit = 0; bit < 65536U; ++bit) {
                if ((bytes[pos + (bit / 8U)] & static_cast<std::uint8_t>(1U << (bit % 8U))) != 0U) {
                    out_sorted_values.push_back(base | bit);
                }
            }
            pos += kBitmapContainerBytes;
        } else {
            // Array container: the values themselves, as sorted u16.
            const std::size_t needed = static_cast<std::size_t>(descriptors[i].cardinality) * 2U;
            if (pos + needed > bytes.size()) {
                error = "deletion bitmap array container is truncated";
                return false;
            }
            for (std::uint32_t v = 0; v < descriptors[i].cardinality; ++v) {
                std::uint16_t value = 0;
                if (!read_u16(bytes, pos + static_cast<std::size_t>(v) * 2U, value)) {
                    error = "deletion bitmap array container is truncated";
                    return false;
                }
                out_sorted_values.push_back(base | value);
            }
            pos += needed;
        }
    }

    // Containers are stored in key order and values within them are sorted, so this is already
    // sorted -- but the file is untrusted, and the caller relies on the ordering.
    std::sort(out_sorted_values.begin(), out_sorted_values.end());
    out_sorted_values.erase(std::unique(out_sorted_values.begin(), out_sorted_values.end()),
                            out_sorted_values.end());
    return true;
}

bool read_deletion_vector(const std::filesystem::path& dataset_path, std::uint64_t fragment_id,
                          const pb::DeletionFile& deletion_file,
                          std::vector<std::uint32_t>& out_sorted_offsets, std::string& error) {
    out_sorted_offsets.clear();
    error.clear();
    if (!deletion_file.present) {
        return true;
    }

    const bool bitmap = deletion_file.file_type == 1U;
    const std::string name = std::to_string(fragment_id) + "-" + std::to_string(deletion_file.read_version) +
                             "-" + std::to_string(deletion_file.id) + (bitmap ? ".bin" : ".arrow");
    const auto path = dataset_path / "_deletions" / name;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "failed to open deletion file " + name;
        return false;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.eof() && in.fail()) {
        error = "failed to read deletion file " + name;
        return false;
    }

    if (bitmap) {
        if (!parse_roaring_bitmap(bytes, out_sorted_offsets, error)) {
            return false;
        }
    } else {
        if (!parse_arrow_ipc_uint32_column(bytes, out_sorted_offsets, error)) {
            return false;
        }
        std::sort(out_sorted_offsets.begin(), out_sorted_offsets.end());
        out_sorted_offsets.erase(std::unique(out_sorted_offsets.begin(), out_sorted_offsets.end()),
                                 out_sorted_offsets.end());
    }

    // The manifest states the count; a disagreement means one of the two is wrong and we must not
    // guess which. Filtering by a vector that does not match the manifest would silently return the
    // wrong number of rows, which is the whole failure class this work exists to close.
    if (deletion_file.num_deleted_rows != 0U &&
        out_sorted_offsets.size() != deletion_file.num_deleted_rows) {
        error = "deletion file holds " + std::to_string(out_sorted_offsets.size()) +
                " offsets but the manifest claims " + std::to_string(deletion_file.num_deleted_rows);
        return false;
    }
    return true;
}

}  // namespace nano_lance
