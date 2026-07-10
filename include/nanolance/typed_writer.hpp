// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Compile-time-typed writer facade over the C API. The schema is a TYPE, so everything it determines
// happens at compile time: Arrow format mapping, per-column encoding declarations (which skip the
// commit-time detection scans -- see nano_lance_writer_set_column_encoding), and hint/type
// compatibility (a bad declaration is a compile error here, not a runtime commit failure). This is a
// thin header-only layer: it builds non-owning Arrow structs over the caller's typed spans and calls
// the exact same C pipeline as every other writer -- one encode path, no divergence.
//
//   using PacketSchema = nano_lance::typed::schema<
//       nano_lance::typed::column<std::uint64_t, "ts",     nano_lance::typed::encoding::bitpack>,
//       nano_lance::typed::column<std::uint32_t, "caplen", nano_lance::typed::encoding::bitpack>,
//       nano_lance::typed::column<double,        "gain",   nano_lance::typed::encoding::bss_zstd>,
//       nano_lance::typed::column<std::string_view, "uri">>;
//
//   nano_lance::typed::writer<PacketSchema> w("out.lance");
//   if (!w.write_batch(ts_span, caplen_span, gain_span, uri_span)) { /* w.last_error() */ }
//   if (!w.commit()) { /* ... */ }
//
// Buffer lifetime: with options::borrow_buffers (the default), fixed-width column spans are encoded
// zero-copy straight from the caller's memory and MUST remain valid and unmodified until commit().
// Set borrow_buffers=false to copy at write_batch time instead (the classic contract). String and
// bool columns are always copied at write_batch time regardless (they need Arrow-layout staging).

#pragma once

#include "nanolance/nano_lance_writer.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Canonical Arrow C data interface definitions, guarded exactly as the Arrow specification prescribes
// so this header composes with nanoarrow (or any other Arrow implementation) in either include order.
#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE 2
#define ARROW_FLAG_MAP_KEYS_SORTED 4

struct ArrowSchema {
    // Array type description
    const char* format;
    const char* name;
    const char* metadata;
    int64_t flags;
    int64_t n_children;
    struct ArrowSchema** children;
    struct ArrowSchema* dictionary;

    // Release callback
    void (*release)(struct ArrowSchema*);
    // Opaque producer-specific data
    void* private_data;
};

struct ArrowArray {
    // Array data description
    int64_t length;
    int64_t null_count;
    int64_t offset;
    int64_t n_buffers;
    int64_t n_children;
    const void** buffers;
    struct ArrowArray** children;
    struct ArrowArray* dictionary;

    // Release callback
    void (*release)(struct ArrowArray*);
    // Opaque producer-specific data
    void* private_data;
};

#endif  // ARROW_C_DATA_INTERFACE

namespace nano_lance::typed {

/// NTTP string so column names live in the schema TYPE.
template <std::size_t N>
struct fixed_string {
    char value[N]{};
    constexpr fixed_string(const char (&s)[N]) {  // NOLINT(google-explicit-constructor)
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = s[i];
        }
    }
    [[nodiscard]] constexpr const char* c_str() const { return value; }
};

enum class encoding {
    auto_detect,  // default: commit-time detection scans pick the encoding
    plain,        // flat pages, no structural encoding, no zstd
    bitpack,      // FastLanes InlineBitpacking (integers)
    bss_zstd,     // byte-stream-split + zstd (float/double)
    zstd,         // zstd'd variable-width pages (strings; needs options::compress)
};

namespace detail {

template <class T>
struct arrow_format;  // primary template intentionally undefined: unsupported column type

// clang-format off
template <> struct arrow_format<std::int8_t>      { static constexpr const char* value = "c"; };
template <> struct arrow_format<std::uint8_t>     { static constexpr const char* value = "C"; };
template <> struct arrow_format<std::int16_t>     { static constexpr const char* value = "s"; };
template <> struct arrow_format<std::uint16_t>    { static constexpr const char* value = "S"; };
template <> struct arrow_format<std::int32_t>     { static constexpr const char* value = "i"; };
template <> struct arrow_format<std::uint32_t>    { static constexpr const char* value = "I"; };
template <> struct arrow_format<std::int64_t>     { static constexpr const char* value = "l"; };
template <> struct arrow_format<std::uint64_t>    { static constexpr const char* value = "L"; };
template <> struct arrow_format<float>            { static constexpr const char* value = "f"; };
template <> struct arrow_format<double>           { static constexpr const char* value = "g"; };
template <> struct arrow_format<bool>             { static constexpr const char* value = "b"; };
template <> struct arrow_format<std::string_view> { static constexpr const char* value = "u"; };
// clang-format on

template <class T>
inline constexpr bool is_bitpackable_v = std::is_integral_v<T> && !std::is_same_v<T, bool>;

template <class T, encoding Enc>
constexpr bool check_encoding() {
    if constexpr (Enc == encoding::bitpack) {
        static_assert(is_bitpackable_v<T>, "encoding::bitpack requires an integer column type");
    } else if constexpr (Enc == encoding::bss_zstd) {
        static_assert(std::is_same_v<T, float> || std::is_same_v<T, double>,
                      "encoding::bss_zstd requires a float or double column type");
    } else if constexpr (Enc == encoding::zstd) {
        static_assert(std::is_same_v<T, std::string_view>,
                      "encoding::zstd requires a std::string_view column type");
    }
    return true;
}

constexpr const char* encoding_name(encoding e) {
    switch (e) {
        case encoding::plain:
            return "plain";
        case encoding::bitpack:
            return "bitpack";
        case encoding::bss_zstd:
            return "bss-zstd";
        case encoding::zstd:
            return "zstd";
        case encoding::auto_detect:
            break;
    }
    return "auto";
}

}  // namespace detail

template <class T, fixed_string Name, encoding Enc = encoding::auto_detect>
struct column {
    using value_type = T;
    static constexpr auto name = Name;
    static constexpr encoding enc = Enc;
    // Type/encoding compatibility is enforced HERE, at schema-definition time.
    static constexpr bool checked = detail::check_encoding<T, Enc>();
};

template <class... Columns>
struct schema {
    static constexpr std::size_t column_count = sizeof...(Columns);
};

template <class Schema>
class writer;

template <class... Columns>
class writer<schema<Columns...>> {
    static constexpr std::size_t kColumns = sizeof...(Columns);
    static_assert(kColumns > 0, "schema needs at least one column");

public:
    struct options {
        int compression_level = 3;
        bool compress = false;        // zstd for declared-zstd strings / auto-detected string paths
        bool structural = true;       // bitpack/constant/RLE/dict detection for auto_detect columns
        bool borrow_buffers = true;   // zero-copy fixed-width ingest; spans must outlive commit()
        bool append = false;          // open an existing dataset instead of creating
    };

    explicit writer(const char* dataset_path, options opts = {}) : opts_(opts) {
        const int rc = opts.append ? nano_lance_writer_init_append(&w_, dataset_path, opts.compression_level)
                                   : nano_lance_writer_init(&w_, dataset_path, opts.compression_level);
        ok_ = rc == NANO_LANCE_OK;
        ok_ = ok_ && nano_lance_writer_set_ignore_nullability(&w_, true) == NANO_LANCE_OK;
        ok_ = ok_ && nano_lance_writer_set_compression(&w_, opts.compress) == NANO_LANCE_OK;
        ok_ = ok_ && nano_lance_writer_set_structural_encoding(&w_, opts.structural) == NANO_LANCE_OK;
        ok_ = ok_ && nano_lance_writer_set_borrow_buffers(&w_, opts.borrow_buffers) == NANO_LANCE_OK;
        // Compile-time-declared encodings become per-field declarations, skipping the detection scans.
        (apply_declaration<Columns>(), ...);
    }

    writer(const writer&) = delete;
    writer& operator=(const writer&) = delete;

    ~writer() {
        if (!closed_) {
            nano_lance_writer_close(&w_);
        }
    }

    /// Append one batch; all spans must have equal length. Returns false on error (see last_error()).
    /// With options::borrow_buffers, fixed-width spans must stay valid and unmodified until commit().
    bool write_batch(std::span<const typename Columns::value_type>... cols) {
        if (!ok_) {
            return false;
        }
        const std::size_t lengths[] = {cols.size()...};
        for (std::size_t i = 1; i < kColumns; ++i) {
            if (lengths[i] != lengths[0]) {
                ok_ = false;
                return false;
            }
        }

        BatchStorage storage;
        std::size_t index = 0;
        (fill_column<Columns>(cols, storage, index), ...);
        for (std::size_t i = 0; i < kColumns; ++i) {
            storage.schema_children[i] = &storage.child_schemas[i];
            storage.array_children[i] = &storage.child_arrays[i];
        }

        ArrowSchema root_schema{};
        root_schema.format = "+s";
        root_schema.name = "";
        root_schema.n_children = static_cast<std::int64_t>(kColumns);
        root_schema.children = storage.schema_children.data();

        const void* root_buffers[1] = {nullptr};
        ArrowArray root_array{};
        root_array.length = static_cast<std::int64_t>(lengths[0]);
        root_array.n_buffers = 1;
        root_array.buffers = root_buffers;
        root_array.n_children = static_cast<std::int64_t>(kColumns);
        root_array.children = storage.array_children.data();

        ok_ = nano_lance_write_batch(&w_, &root_array, &root_schema) == NANO_LANCE_OK;
        return ok_;
    }

    bool commit() {
        if (!ok_) {
            return false;
        }
        ok_ = nano_lance_writer_commit(&w_, opts_.append) == NANO_LANCE_OK;
        return ok_;
    }

    bool close() {
        closed_ = true;
        return nano_lance_writer_close(&w_) == NANO_LANCE_OK;
    }

    [[nodiscard]] const char* last_error() const { return nano_lance_writer_last_error(&w_); }
    [[nodiscard]] bool ok() const { return ok_; }

private:
    /// Per-batch Arrow structs plus staging for columns that need layout conversion (strings: offsets
    /// + contiguous data; bool: bit-packed). Fixed-size slots (no reallocation -- the Arrow structs
    /// hold pointers into these). Lives for the duration of write_batch only: the C ingest copies
    /// string/bool columns unconditionally, and either copies or (borrow mode) records the span
    /// pointer for fixed-width columns, so nothing here needs to outlive the call.
    struct BatchStorage {
        std::array<ArrowSchema, kColumns> child_schemas{};
        std::array<ArrowArray, kColumns> child_arrays{};
        std::array<ArrowSchema*, kColumns> schema_children{};
        std::array<ArrowArray*, kColumns> array_children{};
        std::array<std::array<const void*, 3>, kColumns> buffers{};
        std::array<std::vector<std::int32_t>, kColumns> string_offsets;
        std::array<std::string, kColumns> string_data;
        std::array<std::vector<std::uint8_t>, kColumns> bool_bits;
    };

    template <class Column>
    void apply_declaration() {
        if constexpr (Column::enc != encoding::auto_detect) {
            ok_ = ok_ && nano_lance_writer_set_column_encoding(&w_, Column::name.c_str(),
                                                               detail::encoding_name(Column::enc)) ==
                             NANO_LANCE_OK;
        }
    }

    template <class Column, class T>
    void fill_column(std::span<const T> values, BatchStorage& storage, std::size_t& index) {
        auto& sch = storage.child_schemas[index];
        auto& arr = storage.child_arrays[index];
        auto& bufs = storage.buffers[index];
        sch.format = detail::arrow_format<T>::value;
        sch.name = Column::name.c_str();
        arr.length = static_cast<std::int64_t>(values.size());
        bufs[0] = nullptr;

        if constexpr (std::is_same_v<T, std::string_view>) {
            auto& offsets = storage.string_offsets[index];
            auto& data = storage.string_data[index];
            offsets.reserve(values.size() + 1U);
            offsets.push_back(0);
            std::size_t total = 0;
            for (const auto& v : values) {
                total += v.size();
            }
            data.reserve(total);
            for (const auto& v : values) {
                data.append(v.data(), v.size());
                offsets.push_back(static_cast<std::int32_t>(data.size()));
            }
            bufs[1] = offsets.data();
            bufs[2] = data.data();
            arr.n_buffers = 3;
        } else if constexpr (std::is_same_v<T, bool>) {
            auto& bits = storage.bool_bits[index];
            bits.assign((values.size() + 7U) / 8U, 0U);
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (values[i]) {
                    bits[i / 8U] |= static_cast<std::uint8_t>(1U << (i % 8U));
                }
            }
            bufs[1] = bits.data();
            arr.n_buffers = 2;
        } else {
            bufs[1] = values.data();
            arr.n_buffers = 2;
        }
        arr.buffers = bufs.data();
        ++index;
    }

    NanoLanceWriter w_{};
    options opts_{};
    bool ok_ = false;
    bool closed_ = false;
};

}  // namespace nano_lance::typed
