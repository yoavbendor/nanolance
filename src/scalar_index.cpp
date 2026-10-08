// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Building Lance's scalar indices (scalar_index.hpp). The column is scanned with its row addresses
// (deleted rows left out, as Lance's training scan leaves them), its values -- or for a label list,
// its lists' elements -- sorted nulls first, then ascending, ties by row address, and written in
// Lance's index files; the index is committed in the manifest's index section as Lance commits one.

#include "index_build.hpp"
#include "nanolance/scalar_index.hpp"
#include "nanolance/vector_search.hpp"

#include "index_files.hpp"

#include "nanolance/dataset_commit.hpp"
#include "nanolance/index_maintenance.hpp"
#include "nanolance/index_search.hpp"
#include "nanolance/lance_file_writer.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/parallel.hpp"
#include "nanolance/roaring_bitmap.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace nano_lance {
namespace {


constexpr std::uint64_t kBTreePageRows = 4096;  // Lance's DEFAULT_BTREE_BATCH_SIZE

// ── Arrow ownership ─────────────────────────────────────────────────────────────────────────────

using index_files::OwnedArray;
using index_files::OwnedBatches;
using index_files::OwnedSchema;
using index_files::OwnedViews;
using index_files::WrittenFile;
using index_files::bytes_of;
using index_files::find_field;
using index_files::new_uuid;
using index_files::struct_batch;
using index_files::struct_schema;
using index_files::type_schema;
using index_files::uint_array;
using index_files::write_file;

// ── the values to index ─────────────────────────────────────────────────────────────────────────

/// The values to index, in row-address order: item `k` is element `index[k]` of `views[view[k]]`,
/// from the row at `addr[k]`.
struct Items {
    std::vector<const ArrowArrayView*> views;
    std::vector<std::uint32_t> view;
    std::vector<std::int64_t> index;
    std::vector<std::uint64_t> addr;

    void push(std::uint32_t v, std::int64_t i, std::uint64_t a) {
        view.push_back(v);
        index.push_back(i);
        addr.push_back(a);
    }
    std::size_t size() const { return addr.size(); }
    bool is_null(std::size_t k) const { return ArrowArrayViewIsNull(views[view[k]], index[k]); }
};

enum class KeyKind { Int, UInt, Float16, Float32, Float64, Bool, Bytes, Decimal128 };

bool key_kind_of(const ArrowSchemaView& type, KeyKind& kind, std::string& error) {
    switch (type.storage_type) {
    case NANOARROW_TYPE_INT8:
    case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32:
    case NANOARROW_TYPE_INT64:
        kind = KeyKind::Int;
        return true;
    case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32:
    case NANOARROW_TYPE_UINT64:
        kind = KeyKind::UInt;
        return true;
    case NANOARROW_TYPE_HALF_FLOAT:
        kind = KeyKind::Float16;
        return true;
    case NANOARROW_TYPE_FLOAT:
        kind = KeyKind::Float32;
        return true;
    case NANOARROW_TYPE_DOUBLE:
        kind = KeyKind::Float64;
        return true;
    case NANOARROW_TYPE_BOOL:
        kind = KeyKind::Bool;
        return true;
    case NANOARROW_TYPE_STRING:
    case NANOARROW_TYPE_LARGE_STRING:
    case NANOARROW_TYPE_BINARY:
    case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_FIXED_SIZE_BINARY:
        kind = KeyKind::Bytes;
        return true;
    case NANOARROW_TYPE_DECIMAL128:
        kind = KeyKind::Decimal128;
        return true;
    default:
        error = std::string("a scalar index on a column of type ") + ArrowTypeString(type.type) +
                " is not supported";
        return false;
    }
}

/// Floats in IEEE total order (Rust's total_cmp, which Arrow's sort uses): -NaN < -inf < ... < -0 <
/// +0 < ... < +inf < NaN, as unsigned integers.
template <typename U>
U total_order(U bits, unsigned width) {
    const U sign = static_cast<U>(U{1} << (width - 1U));
    return (bits & sign) != 0U ? static_cast<U>(~bits) : static_cast<U>(bits | sign);
}

const std::uint8_t* fixed_value(const ArrowArrayView* view, std::int64_t i, std::size_t width) {
    return view->buffer_views[1].data.as_uint8 + static_cast<std::size_t>(view->offset + i) * width;
}

/// The sort: `order` lists the items null first (in row-address order), then by value, ties by row
/// address; `group_starts` the position in `order` where each distinct value (null included) begins.
struct Sorted {
    std::vector<std::uint32_t> order;
    std::vector<std::size_t> group_starts;
    std::size_t nulls = 0;
};

/// std::sort over `parallel::threads()` slices side by side, then merged pairwise.
template <typename T, typename Less>
void parallel_sort(std::vector<T>& v, Less less) {
    std::size_t parts = 1;
    while (parts * 2 <= parallel::threads() && v.size() / (parts * 2) >= (1U << 15U)) {
        parts *= 2;
    }
    if (parts == 1) {
        std::sort(v.begin(), v.end(), less);
        return;
    }
    std::vector<std::size_t> bounds(parts + 1);
    for (std::size_t p = 0; p <= parts; ++p) {
        bounds[p] = v.size() * p / parts;
    }
    parallel::for_each(parts, [&](std::size_t p) {
        std::sort(v.begin() + static_cast<std::ptrdiff_t>(bounds[p]), v.begin() + static_cast<std::ptrdiff_t>(bounds[p + 1]),
                  less);
    });
    std::vector<T> other(v.size());
    auto* from = &v;
    auto* to = &other;
    for (std::size_t width = 1; width < parts; width *= 2) {
        const std::size_t merges = parts / (width * 2);
        parallel::for_each(merges, [&](std::size_t m) {
            const auto a = bounds[m * width * 2];
            const auto mid = bounds[m * width * 2 + width];
            const auto b = bounds[m * width * 2 + width * 2];
            std::merge(from->begin() + static_cast<std::ptrdiff_t>(a), from->begin() + static_cast<std::ptrdiff_t>(mid),
                       from->begin() + static_cast<std::ptrdiff_t>(mid), from->begin() + static_cast<std::ptrdiff_t>(b),
                       to->begin() + static_cast<std::ptrdiff_t>(a), less);
        });
        std::swap(from, to);
    }
    if (from != &v) {
        v.swap(other);
    }
}

/// A string key: its first 8 bytes, big-endian and zero-padded, decide most comparisons without
/// touching the string; the whole string decides the rest.
struct StringKey {
    std::uint64_t prefix;
    std::string_view s;
    bool operator<(const StringKey& o) const { return prefix != o.prefix ? prefix < o.prefix : s < o.s; }
    bool operator==(const StringKey& o) const { return prefix == o.prefix && s == o.s; }
};

/// The key of string `i`, its prefix taken `skip` bytes in (past a prefix every key shares).
StringKey string_key(const ArrowArrayView* v, std::int64_t i, std::size_t skip) {
    const auto b = ArrowArrayViewGetBytesUnsafe(v, i);
    const auto n = static_cast<std::size_t>(b.size_bytes);
    std::uint64_t prefix = 0;
    for (std::size_t k = skip; k < skip + 8; ++k) {
        prefix = (prefix << 8U) | (k < n ? static_cast<std::uint8_t>(b.data.as_char[k]) : 0U);
    }
    return {prefix, std::string_view(b.data.as_char, n)};
}

/// How many leading bytes every non-null string item shares.
std::size_t common_prefix(const Items& items) {
    std::string_view first;
    bool any = false;
    std::size_t len = 0;
    for (std::size_t k = 0; k < items.size() && (!any || len > 0); ++k) {
        if (items.is_null(k)) {
            continue;
        }
        const auto b = ArrowArrayViewGetBytesUnsafe(items.views[items.view[k]], items.index[k]);
        const std::string_view s(b.data.as_char, static_cast<std::size_t>(b.size_bytes));
        if (!any) {
            first = s;
            len = s.size();
            any = true;
            continue;
        }
        std::size_t j = 0;
        const auto limit = std::min(len, s.size());
        while (j < limit && first[j] == s[j]) {
            ++j;
        }
        len = j;
    }
    return len;
}

struct StringKeyHash {
    std::size_t operator()(const StringKey& k) const { return std::hash<std::string_view>()(k.s); }
};

/// A decimal128's value, as two halves (no compiler has to offer a 128-bit integer): ordered by its
/// signed high half, then its low half.
struct Int128 {
    std::int64_t hi = 0;
    std::uint64_t lo = 0;
    bool operator<(const Int128& o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }
    bool operator==(const Int128& o) const { return hi == o.hi && lo == o.lo; }
};

struct Int128Hash {
    std::size_t operator()(const Int128& v) const {
        return std::hash<std::uint64_t>()(v.lo ^ (static_cast<std::uint64_t>(v.hi) * 0x9E3779B97F4A7C15ULL));
    }
};

template <typename K>
struct KeyHash {
    using type = std::hash<K>;
};
template <>
struct KeyHash<StringKey> {
    using type = StringKeyHash;
};
template <>
struct KeyHash<Int128> {
    using type = Int128Hash;
};

/// BTree order: every value sorted, ties by row address.
template <typename K, typename Extract>
void sort_all(const Items& items, Extract extract, Sorted& out) {
    struct Entry {
        K key;
        std::uint32_t item;
    };
    std::vector<Entry> entries;
    entries.reserve(items.size());
    out.order.clear();
    out.group_starts.clear();
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (items.is_null(k)) {
            out.order.push_back(static_cast<std::uint32_t>(k));
        } else {
            entries.push_back({extract(items.views[items.view[k]], items.index[k]), static_cast<std::uint32_t>(k)});
        }
    }
    out.nulls = out.order.size();
    if (out.nulls > 0U) {
        out.group_starts.push_back(0);
    }
    // Items are in row-address order, so the item number breaks ties by address.
    parallel_sort(entries, [](const Entry& a, const Entry& b) {
        return a.key < b.key || (!(b.key < a.key) && a.item < b.item);
    });
    out.order.reserve(items.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i == 0 || entries[i - 1].key < entries[i].key) {
            out.group_starts.push_back(out.order.size());
        }
        out.order.push_back(entries[i].item);
    }
}

/// Bitmap order: the same order, found by grouping equal values (a hash table of the distinct ones)
/// and sorting only those -- a bitmap index is for columns with few.
template <typename K, typename Extract>
void group_all(const Items& items, Extract extract, Sorted& out) {
    const std::uint32_t kNull = 0;  // group 0: the nulls, whether or not there are any
    std::unordered_map<K, std::uint32_t, typename KeyHash<K>::type> ids;
    std::vector<K> keys;
    std::vector<std::uint32_t> group(items.size());
    std::vector<std::size_t> counts(1, 0);
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (items.is_null(k)) {
            group[k] = kNull;
            ++counts[0];
            continue;
        }
        const K key = extract(items.views[items.view[k]], items.index[k]);
        const auto [it, inserted] = ids.try_emplace(key, static_cast<std::uint32_t>(keys.size() + 1));
        if (inserted) {
            keys.push_back(key);
            counts.push_back(0);
        }
        group[k] = it->second;
        ++counts[it->second];
    }
    std::vector<std::uint32_t> by_key(keys.size());
    for (std::size_t g = 0; g < by_key.size(); ++g) {
        by_key[g] = static_cast<std::uint32_t>(g + 1);
    }
    std::sort(by_key.begin(), by_key.end(), [&](std::uint32_t a, std::uint32_t b) { return keys[a - 1] < keys[b - 1]; });
    std::vector<std::size_t> start(counts.size());
    out.group_starts.clear();
    out.nulls = counts[0];
    std::size_t at = 0;
    if (counts[0] > 0) {
        out.group_starts.push_back(0);
    }
    start[0] = 0;
    at = counts[0];
    for (const auto g : by_key) {
        out.group_starts.push_back(at);
        start[g] = at;
        at += counts[g];
    }
    out.order.assign(items.size(), 0);
    for (std::size_t k = 0; k < items.size(); ++k) {
        out.order[start[group[k]]++] = static_cast<std::uint32_t>(k);  // in item (row-address) order
    }
}

template <typename K, typename Extract>
void order_items(const Items& items, Extract extract, bool group, Sorted& out) {
    if (group) {
        group_all<K>(items, extract, out);
    } else {
        sort_all<K>(items, extract, out);
    }
}

void sort_items(const Items& items, KeyKind kind, bool group, Sorted& out) {
    switch (kind) {
    case KeyKind::Int:
    case KeyKind::Bool:
        order_items<std::int64_t>(
            items, [](const ArrowArrayView* v, std::int64_t i) { return ArrowArrayViewGetIntUnsafe(v, i); }, group, out);
        break;
    case KeyKind::UInt:
        order_items<std::uint64_t>(
            items, [](const ArrowArrayView* v, std::int64_t i) { return ArrowArrayViewGetUIntUnsafe(v, i); }, group, out);
        break;
    case KeyKind::Float16:
        order_items<std::uint16_t>(
            items,
            [](const ArrowArrayView* v, std::int64_t i) {
                std::uint16_t bits = 0;
                std::memcpy(&bits, fixed_value(v, i, 2), 2);
                return total_order<std::uint16_t>(bits, 16);
            },
            group, out);
        break;
    case KeyKind::Float32:
        order_items<std::uint32_t>(
            items,
            [](const ArrowArrayView* v, std::int64_t i) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, fixed_value(v, i, 4), 4);
                return total_order<std::uint32_t>(bits, 32);
            },
            group, out);
        break;
    case KeyKind::Float64:
        order_items<std::uint64_t>(
            items,
            [](const ArrowArrayView* v, std::int64_t i) {
                std::uint64_t bits = 0;
                std::memcpy(&bits, fixed_value(v, i, 8), 8);
                return total_order<std::uint64_t>(bits, 64);
            },
            group, out);
        break;
    case KeyKind::Decimal128:
        order_items<Int128>(
            items,
            [](const ArrowArrayView* v, std::int64_t i) {
                Int128 value;  // little-endian: the low half first
                std::memcpy(&value.lo, fixed_value(v, i, 16), 8);
                std::memcpy(&value.hi, fixed_value(v, i, 16) + 8, 8);
                return value;
            },
            group, out);
        break;
    case KeyKind::Bytes: {
        // Keys sharing their first bytes ("user_000123") would all have one prefix.
        const auto skip = group ? 0 : common_prefix(items);
        order_items<StringKey>(
            items, [skip](const ArrowArrayView* v, std::int64_t i) { return string_key(v, i, skip); }, group, out);
        break;
    }
    }
}

// ── building arrays ─────────────────────────────────────────────────────────────────────────────

/// Rows [begin, end) of `picks` in chunks a multiple of 64 rows long (so that bitmaps split on byte
/// boundaries), side by side.
void for_chunks(std::size_t n, const std::function<void(std::size_t, std::size_t)>& task) {
    constexpr std::size_t kChunk = 1U << 16U;
    const auto chunks = (n + kChunk - 1) / kChunk;
    parallel::for_each(chunks, [&](std::size_t c) { task(c * kChunk, std::min(n, (c + 1) * kChunk)); });
}

/// The items at `picks` as one array of `type` (a plain Arrow type: no dictionary, no children).
bool gather(const ArrowSchema& type, const Items& items, const std::vector<std::uint32_t>& picks, ArrowArray& out,
            std::string& error) {
    ArrowError err;
    ArrowSchemaView sv;
    if (ArrowSchemaViewInit(&sv, &type, &err) != NANOARROW_OK ||
        ArrowArrayInitFromSchema(&out, &type, &err) != NANOARROW_OK) {
        error = std::string("index values: ") + err.message;
        return false;
    }
    const auto n = picks.size();
    const auto rows = static_cast<std::int64_t>(n);
    const auto bitmap_bytes = static_cast<std::int64_t>((n + 7) / 8);
    // Validity first: it says which items to copy.
    std::vector<std::uint8_t> valid((n + 7) / 8, 0);
    std::vector<std::int64_t> chunk_nulls((n + (1U << 16U) - 1) >> 16U, 0);
    for_chunks(n, [&](std::size_t begin, std::size_t end) {
        std::int64_t nulls = 0;
        for (auto r = begin; r < end; ++r) {
            if (items.is_null(picks[r])) {
                ++nulls;
            } else {
                valid[r >> 3U] = static_cast<std::uint8_t>(valid[r >> 3U] | (1U << (r & 7U)));
            }
        }
        chunk_nulls[begin >> 16U] = nulls;
    });
    std::int64_t nulls = 0;
    for (const auto c : chunk_nulls) {
        nulls += c;
    }
    const auto is_valid = [&](std::size_t r) { return (valid[r >> 3U] >> (r & 7U)) & 1U; };
    ArrowBuffer* data = ArrowArrayBuffer(&out, 1);
    const auto storage = sv.storage_type;
    if (storage == NANOARROW_TYPE_BOOL) {
        if (ArrowBufferResize(data, bitmap_bytes, 0) != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
        std::memset(data->data, 0, static_cast<std::size_t>(bitmap_bytes));
        for_chunks(n, [&](std::size_t begin, std::size_t end) {
            for (auto r = begin; r < end; ++r) {
                const auto k = picks[r];
                const auto* v = items.views[items.view[k]];
                if (is_valid(r) && ArrowBitGet(v->buffer_views[1].data.as_uint8, v->offset + items.index[k])) {
                    data->data[r >> 3U] = static_cast<std::uint8_t>(data->data[r >> 3U] | (1U << (r & 7U)));
                }
            }
        });
    } else if (storage == NANOARROW_TYPE_STRING || storage == NANOARROW_TYPE_BINARY ||
               storage == NANOARROW_TYPE_LARGE_STRING || storage == NANOARROW_TYPE_LARGE_BINARY) {
        const bool large = storage == NANOARROW_TYPE_LARGE_STRING || storage == NANOARROW_TYPE_LARGE_BINARY;
        ArrowBuffer* bytes = ArrowArrayBuffer(&out, 2);
        std::vector<std::int64_t> offsets(n + 1, 0);
        for_chunks(n, [&](std::size_t begin, std::size_t end) {
            for (auto r = begin; r < end; ++r) {
                offsets[r + 1] = is_valid(r) ? ArrowArrayViewGetBytesUnsafe(items.views[items.view[picks[r]]],
                                                                           items.index[picks[r]])
                                                   .size_bytes
                                             : 0;
            }
        });
        for (std::size_t r = 0; r < n; ++r) {
            offsets[r + 1] += offsets[r];
        }
        const auto total = offsets[n];
        if (!large && total > INT32_MAX) {
            error = "index values over 2 GiB in one string array";
            return false;
        }
        if (ArrowBufferResize(data, (rows + 1) * (large ? 8 : 4), 0) != NANOARROW_OK ||
            ArrowBufferResize(bytes, total, 0) != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
        if (large) {
            std::memcpy(data->data, offsets.data(), (n + 1) * 8);
        } else {
            auto* o32 = reinterpret_cast<std::int32_t*>(data->data);
            for (std::size_t r = 0; r <= n; ++r) {
                o32[r] = static_cast<std::int32_t>(offsets[r]);
            }
        }
        for_chunks(n, [&](std::size_t begin, std::size_t end) {
            for (auto r = begin; r < end; ++r) {
                if (offsets[r + 1] != offsets[r]) {
                    const auto b = ArrowArrayViewGetBytesUnsafe(items.views[items.view[picks[r]]], items.index[picks[r]]);
                    std::memcpy(bytes->data + offsets[r], b.data.data, static_cast<std::size_t>(b.size_bytes));
                }
            }
        });
    } else {
        const auto bits = sv.layout.element_size_bits[1];
        if (bits <= 0 || bits % 8 != 0) {
            error = "index values of an unsupported layout";
            return false;
        }
        const auto width = static_cast<std::size_t>(bits / 8);
        if (ArrowBufferResize(data, rows * static_cast<std::int64_t>(width), 0) != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
        for_chunks(n, [&](std::size_t begin, std::size_t end) {
            for (auto r = begin; r < end; ++r) {
                auto* to = data->data + r * width;
                if (is_valid(r)) {
                    const auto k = picks[r];
                    std::memcpy(to, fixed_value(items.views[items.view[k]], items.index[k], width), width);
                } else {
                    std::memset(to, 0, width);
                }
            }
        });
    }
    if (nulls > 0) {
        ArrowBitmap* validity = ArrowArrayValidityBitmap(&out);
        if (ArrowBitmapReserve(validity, rows) != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
        std::memcpy(validity->buffer.data, valid.data(), valid.size());
        validity->buffer.size_bytes = bitmap_bytes;
        validity->size_bits = rows;
    }
    out.length = rows;
    out.null_count = nulls;
    if (ArrowArrayFinishBuildingDefault(&out, &err) != NANOARROW_OK) {
        error = std::string("index values: ") + err.message;
        return false;
    }
    return true;
}

bool binary_array(const std::vector<std::vector<std::uint8_t>>& values, ArrowArray& out, std::string& error) {
    if (ArrowArrayInitFromType(&out, NANOARROW_TYPE_BINARY) != NANOARROW_OK ||
        ArrowArrayStartAppending(&out) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    for (const auto& v : values) {
        ArrowBufferView view;
        view.data.data = v.data();
        view.size_bytes = static_cast<std::int64_t>(v.size());
        if (ArrowArrayAppendBytes(&out, view) != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
    }
    return ArrowArrayFinishBuildingDefault(&out, nullptr) == NANOARROW_OK || (error = "bad array", false);
}

// ── row bitmaps ─────────────────────────────────────────────────────────────────────────────────

/// Lance's RowAddrTreeMap serialization: [u32 fragments] then per fragment, ascending, [u32 id]
/// [u32 size][a Roaring bitmap of the rows' offsets]. `addrs` ascending (repeats allowed).
std::vector<std::uint8_t> serialize_treemap(const std::uint64_t* addrs, std::size_t n) {
    std::vector<std::uint8_t> out(4, 0);
    std::uint32_t fragments = 0;
    std::vector<std::uint32_t> rows;
    for (std::size_t i = 0; i < n;) {
        const auto frag = static_cast<std::uint32_t>(addrs[i] >> 32U);
        rows.clear();
        for (; i < n && static_cast<std::uint32_t>(addrs[i] >> 32U) == frag; ++i) {
            const auto row = static_cast<std::uint32_t>(addrs[i]);
            if (rows.empty() || rows.back() != row) {
                rows.push_back(row);
            }
        }
        const auto bitmap = roaring::encode(rows);
        const std::uint32_t header[2] = {frag, static_cast<std::uint32_t>(bitmap.size())};
        const auto* h = reinterpret_cast<const std::uint8_t*>(header);
        out.insert(out.end(), h, h + 8);
        out.insert(out.end(), bitmap.begin(), bitmap.end());
        ++fragments;
    }
    std::memcpy(out.data(), &fragments, 4);
    return out;
}

// ── the index files ─────────────────────────────────────────────────────────────────────────────

bool write_btree(const std::filesystem::path& dir, const ArrowSchema& type, const Items& items, const Sorted& sorted,
                 std::vector<WrittenFile>& files, std::string& error) {
    const auto n = sorted.order.size();
    {
        OwnedSchema schema;
        OwnedSchema ids_type;
        OwnedArray values;
        OwnedArray ids;
        OwnedArray batch;
        std::vector<std::uint64_t> addrs(n);
        for (std::size_t i = 0; i < n; ++i) {
            addrs[i] = items.addr[sorted.order[i]];
        }
        if (!type_schema(NANOARROW_TYPE_UINT64, ids_type.s) ||
            !struct_schema({{"values", &type, true}, {"ids", &ids_type.s, false}}, schema.s, error) ||
            !gather(type, items, sorted.order, values.a, error) ||
            !uint_array(NANOARROW_TYPE_UINT64, addrs.data(), static_cast<std::int64_t>(n), 8, ids.a, error) ||
            !struct_batch({&values.a, &ids.a}, static_cast<std::int64_t>(n), batch.a, error) ||
            !write_file(dir, "page_data.lance", schema.s, batch.a, {}, files, error)) {
            return error.empty() ? (error = "out of memory", false) : false;
        }
    }
    const auto pages = (n + kBTreePageRows - 1) / kBTreePageRows;
    std::vector<std::uint32_t> firsts;
    std::vector<std::uint32_t> lasts;
    std::vector<std::uint32_t> null_counts;
    std::vector<std::uint32_t> page_idx;
    for (std::size_t p = 0; p < pages; ++p) {
        const auto begin = p * kBTreePageRows;
        const auto end = std::min<std::size_t>(n, begin + kBTreePageRows);
        firsts.push_back(sorted.order[begin]);
        lasts.push_back(sorted.order[end - 1]);
        // Nulls sort first: a page's nulls are the part of it before the first value.
        null_counts.push_back(static_cast<std::uint32_t>(begin < sorted.nulls ? std::min(end, sorted.nulls) - begin : 0));
        page_idx.push_back(static_cast<std::uint32_t>(p));
    }
    OwnedSchema schema;
    OwnedSchema u32;
    OwnedArray mins;
    OwnedArray maxs;
    OwnedArray counts;
    OwnedArray idx;
    OwnedArray batch;
    LanceFileExtras extras;
    extras.schema_metadata["batch_size"] = bytes_of(std::to_string(kBTreePageRows));
    extras.schema_metadata["range_partitioned"] = bytes_of("false");
    const auto np = static_cast<std::int64_t>(pages);
    if (!type_schema(NANOARROW_TYPE_UINT32, u32.s) ||
        !struct_schema({{"min", &type, true}, {"max", &type, true}, {"null_count", &u32.s, false},
                        {"page_idx", &u32.s, false}},
                       schema.s, error) ||
        !gather(type, items, firsts, mins.a, error) || !gather(type, items, lasts, maxs.a, error) ||
        !uint_array(NANOARROW_TYPE_UINT32, null_counts.data(), np, 4, counts.a, error) ||
        !uint_array(NANOARROW_TYPE_UINT32, page_idx.data(), np, 4, idx.a, error) ||
        !struct_batch({&mins.a, &maxs.a, &counts.a, &idx.a}, np, batch.a, error)) {
        return error.empty() ? (error = "out of memory", false) : false;
    }
    return write_file(dir, "page_lookup.lance", schema.s, batch.a, std::move(extras), files, error);
}

/// Bitmap and label-list indices: one row per distinct value, its rows as a serialized RowAddrTreeMap.
bool write_bitmap(const std::filesystem::path& dir, const ArrowSchema& type, const Items& items, const Sorted& sorted,
                  const std::vector<std::uint8_t>* list_nulls, std::vector<WrittenFile>& files, std::string& error) {
    const auto groups = sorted.group_starts.size();
    std::vector<std::uint32_t> keys;
    std::vector<std::vector<std::uint8_t>> bitmaps;
    keys.reserve(groups);
    bitmaps.reserve(groups);
    std::vector<std::uint64_t> addrs;
    for (std::size_t g = 0; g < groups; ++g) {
        const auto begin = sorted.group_starts[g];
        const auto end = g + 1 < groups ? sorted.group_starts[g + 1] : sorted.order.size();
        keys.push_back(sorted.order[begin]);
        addrs.clear();
        for (auto i = begin; i < end; ++i) {
            addrs.push_back(items.addr[sorted.order[i]]);
        }
        bitmaps.push_back(serialize_treemap(addrs.data(), addrs.size()));
    }
    OwnedSchema schema;
    OwnedSchema binary;
    OwnedArray key_array;
    OwnedArray bitmap_array;
    OwnedArray batch;
    LanceFileExtras extras;
    extras.schema_metadata["lance:index_stats"] = bytes_of("{\"num_bitmaps\":" + std::to_string(groups) + "}");
    if (list_nulls != nullptr) {
        extras.global_buffers.push_back(*list_nulls);
        extras.schema_metadata["lance:label_list_nulls"] = bytes_of("1");
    }
    if (!type_schema(NANOARROW_TYPE_BINARY, binary.s) ||
        !struct_schema({{"keys", &type, true}, {"bitmaps", &binary.s, true}}, schema.s, error) ||
        !gather(type, items, keys, key_array.a, error) || !binary_array(bitmaps, bitmap_array.a, error) ||
        !struct_batch({&key_array.a, &bitmap_array.a}, static_cast<std::int64_t>(groups), batch.a, error)) {
        return error.empty() ? (error = "out of memory", false) : false;
    }
    return write_file(dir, "bitmap_page_lookup.lance", schema.s, batch.a, std::move(extras), files, error);
}

// ── the dataset ─────────────────────────────────────────────────────────────────────────────────

}  // namespace

bool parse_scalar_index_type(const std::string& name, ScalarIndexType& type) {
    std::string upper;
    for (const char c : name) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (upper == "BTREE") {
        type = ScalarIndexType::BTree;
    } else if (upper == "BITMAP") {
        type = ScalarIndexType::Bitmap;
    } else if (upper == "LABEL_LIST" || upper == "LABELLIST") {
        type = ScalarIndexType::LabelList;
    } else {
        return false;
    }
    return true;
}

namespace {

bool create_scalar_index(const std::filesystem::path& dataset_path, const std::string& column, ScalarIndexType type,
                         const ScalarIndexOptions& options, const index_build::SegmentTarget* target,
                         std::uint64_t& new_version, std::string& error) {
    error.clear();
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (target != nullptr && target->manifest != nullptr) {
        manifest = *target->manifest;
        version = target->version;
    } else if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        return false;
    }
    const bool commit = target == nullptr || target->out == nullptr;
    if ((manifest.reader_feature_flags & pb::kFlagStableRowIds) != 0U) {
        error = "a scalar index on a dataset with stable row ids is not supported";
        return false;
    }
    std::vector<std::string> parts;
    const auto* field = find_field(manifest, column, parts);
    if (field == nullptr) {
        error = "column '" + column + "' not found";
        return false;
    }
    const std::string name = options.name.empty() ? column + "_idx" : options.name;
    const auto existing = std::find_if(manifest.indices.begin(), manifest.indices.end(),
                                       [&](const pb::IndexMetadata& i) { return i.name == name; });
    if (commit && existing != manifest.indices.end() && !options.replace) {
        error = "index '" + name + "' already exists";
        return false;
    }

    // The column, with each row's address, deleted rows left out.
    LanceScanRequest request;
    request.has_version = true;
    request.version = version;
    const std::vector<std::string> columns = {parts.front()};
    request.columns = &columns;
    request.with_row_address = true;
    request.fragment_ids = target != nullptr ? target->fragments : nullptr;
    OwnedSchema scanned;
    OwnedBatches batches;
    if (!lance_dataset_scan(dataset_path, request, scanned.s, batches.v, error)) {
        return false;
    }
    const ArrowSchema* col_type = scanned.s.children[0];
    std::vector<std::size_t> path_index;  // struct children down to the column
    for (std::size_t p = 1; p < parts.size(); ++p) {
        std::int64_t at = -1;
        for (std::int64_t c = 0; c < col_type->n_children; ++c) {
            if (col_type->children[c]->name != nullptr && parts[p] == col_type->children[c]->name) {
                at = c;
            }
        }
        if (at < 0) {
            error = "column '" + column + "' not found";
            return false;
        }
        path_index.push_back(static_cast<std::size_t>(at));
        col_type = col_type->children[at];
    }
    ArrowError err;
    ArrowSchemaView col_view;
    if (ArrowSchemaViewInit(&col_view, col_type, &err) != NANOARROW_OK) {
        error = err.message;
        return false;
    }
    const bool list = col_view.type == NANOARROW_TYPE_LIST || col_view.type == NANOARROW_TYPE_LARGE_LIST;
    if (type == ScalarIndexType::LabelList && !list) {
        error = "a LABEL_LIST index needs a list column; '" + column + "' is " + ArrowTypeString(col_view.type);
        return false;
    }
    if (type != ScalarIndexType::LabelList && list) {
        error = "a list column takes a LABEL_LIST index, not a " +
                std::string(type == ScalarIndexType::BTree ? "BTREE" : "BITMAP");
        return false;
    }
    const ArrowSchema* key_type = list ? col_type->children[0] : col_type;
    ArrowSchemaView key_view;
    KeyKind kind{};
    if (ArrowSchemaViewInit(&key_view, key_type, &err) != NANOARROW_OK) {
        error = err.message;
        return false;
    }
    if (key_view.extension_name.data != nullptr || !key_kind_of(key_view, kind, error)) {
        if (error.empty()) {
            error = "a scalar index on an extension type is not supported";
        }
        return false;
    }

    // The values (or list elements) in row-address order; a label list's null lists aside.
    Items items;
    OwnedViews views;
    std::vector<std::uint64_t> null_lists;
    std::size_t capacity = 0;
    for (const auto& batch : batches.v) {
        const ArrowArray* arr = batch.children[0];
        for (const auto c : path_index) {
            arr = arr->children[c];
        }
        capacity += static_cast<std::size_t>(list ? arr->children[0]->length : arr->length);
    }
    items.view.reserve(capacity);
    items.index.reserve(capacity);
    items.addr.reserve(capacity);
    for (const auto& batch : batches.v) {
        const ArrowArray* arr = batch.children[0];
        for (const auto c : path_index) {
            arr = arr->children[c];
        }
        const ArrowArray* addr_arr = batch.children[batch.n_children - 1];
        const auto* addrs = static_cast<const std::uint64_t*>(addr_arr->buffers[1]) + addr_arr->offset;
        auto view = std::make_unique<ArrowArrayView>();
        if (ArrowArrayViewInitFromSchema(view.get(), col_type, &err) != NANOARROW_OK ||
            ArrowArrayViewSetArray(view.get(), arr, &err) != NANOARROW_OK) {
            ArrowArrayViewReset(view.get());
            error = err.message;
            return false;
        }
        const ArrowArrayView* v = view.get();
        views.v.push_back(std::move(view));
        if (!list) {
            const auto vi = static_cast<std::uint32_t>(items.views.size());
            items.views.push_back(v);
            for (std::int64_t r = 0; r < batch.length; ++r) {
                items.push(vi, r, addrs[r]);
            }
            continue;
        }
        const auto vi = static_cast<std::uint32_t>(items.views.size());
        items.views.push_back(v->children[0]);
        for (std::int64_t r = 0; r < batch.length; ++r) {
            if (ArrowArrayViewIsNull(v, r)) {
                null_lists.push_back(addrs[r]);
                continue;
            }
            const auto begin = ArrowArrayViewListChildOffset(v, r);
            const auto end = ArrowArrayViewListChildOffset(v, r + 1);
            for (auto e = begin; e < end; ++e) {
                items.push(vi, e, addrs[r]);
            }
        }
    }
    if (items.size() > UINT32_MAX) {
        error = "over 4 billion values to index";
        return false;
    }
    Sorted sorted;
    sort_items(items, kind, type != ScalarIndexType::BTree, sorted);

    const auto uuid = new_uuid();
    const auto dir = dataset_path / "_indices" / pb::uuid_string(uuid);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }
    std::vector<WrittenFile> files;
    std::string details;
    std::uint32_t index_version = 0;
    bool written = false;
    switch (type) {
    case ScalarIndexType::BTree:
        written = write_btree(dir, *key_type, items, sorted, files, error);
        details = "/lance.table.BTreeIndexDetails";
        break;
    case ScalarIndexType::Bitmap:
        written = write_bitmap(dir, *key_type, items, sorted, nullptr, files, error);
        details = "/lance.table.BitmapIndexDetails";
        break;
    case ScalarIndexType::LabelList: {
        const auto nulls = serialize_treemap(null_lists.data(), null_lists.size());
        written = write_bitmap(dir, *key_type, items, sorted, &nulls, files, error);
        details = "/lance.table.LabelListIndexDetails";
        index_version = 1;
        break;
    }
    }
    if (!written) {
        std::filesystem::remove_all(dir, ec);
        return false;
    }

    std::vector<std::uint32_t> fragment_ids;
    if (target != nullptr && (target->coverage != nullptr || target->fragments != nullptr)) {
        for (const auto id : target->coverage != nullptr ? *target->coverage : *target->fragments) {
            fragment_ids.push_back(static_cast<std::uint32_t>(id));
        }
    } else {
        for (const auto& f : manifest.fragments) {
            fragment_ids.push_back(static_cast<std::uint32_t>(f.id));
        }
    }
    std::sort(fragment_ids.begin(), fragment_ids.end());
    std::vector<pb::IndexMetadata::File> index_files;
    for (const auto& f : files) {
        index_files.push_back({f.name, f.size});
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    auto entry = pb::make_index_metadata(uuid, {field->id}, name, version, fragment_ids, details,
                                                       index_version, static_cast<std::uint64_t>(now), index_files);
    if (!commit) {
        *target->out = std::move(entry);
        return true;
    }
    if (existing != manifest.indices.end()) {
        // Every segment of the replaced index goes.
        manifest.indices.erase(std::remove_if(manifest.indices.begin(), manifest.indices.end(),
                                              [&](const pb::IndexMetadata& i) { return i.name == name; }),
                               manifest.indices.end());
    }
    manifest.indices.push_back(std::move(entry));
    if (!commit_next_version(dataset_path, std::move(manifest), new_version, error)) {
        std::filesystem::remove_all(dir, ec);
        return false;
    }
    return true;
}

}  // namespace

bool dataset_create_scalar_index(const std::filesystem::path& dataset_path, const std::string& column,
                                 ScalarIndexType type, const ScalarIndexOptions& options,
                                 std::uint64_t& new_version, std::string& error) {
    return create_scalar_index(dataset_path, column, type, options, nullptr, new_version, error);
}

bool index_build::build_scalar_segment(const std::filesystem::path& dataset_path, const std::string& column,
                                       ScalarIndexType type, const ScalarIndexOptions& options,
                                       const SegmentTarget& target, std::string& error) {
    std::uint64_t unused = 0;
    return create_scalar_index(dataset_path, column, type, options, &target, unused, error);
}

bool dataset_list_indices(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                          std::vector<IndexInfo>& out, std::string& error) {
    error.clear();
    out.clear();
    pb::Manifest manifest;
    if (has_version) {
        if (!load_manifest_version(dataset_path, version, manifest, error)) {
            return false;
        }
    } else if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        return false;
    }
    std::map<std::int32_t, const pb::Field*> by_id;
    for (const auto& f : manifest.fields) {
        by_id[f.id] = &f;
    }
    const auto path_of = [&](std::int32_t id) {
        std::string path;
        for (auto it = by_id.find(id); it != by_id.end(); it = by_id.find(it->second->parent_id)) {
            path = path.empty() ? it->second->name : it->second->name + "." + path;
        }
        return path;
    };
    for (const auto& index : manifest.indices) {
        if (is_system_index(index)) {
            continue;
        }
        IndexInfo info;
        info.name = index.name;
        info.uuid = pb::uuid_string(index.uuid);
        const auto& url = index.details_type_url;
        const auto dot = url.rfind('.');
        std::string details = dot == std::string::npos ? url : url.substr(dot + 1);
        const std::string suffix = "IndexDetails";
        if (details.size() > suffix.size() && details.compare(details.size() - suffix.size(), suffix.size(), suffix) == 0) {
            details.resize(details.size() - suffix.size());
        }
        info.type = details;
        if (is_vector_index_url(url)) {
            const auto type =  // IVF_PQ, IVF_FLAT, ...
                vector_index_type(dataset_path / "_indices" / pb::uuid_string(index.uuid), index.details_value);
            if (!type.empty()) {
                info.type = type;
            }
        }
        for (const auto id : index.fields) {
            info.fields.push_back(path_of(id));
        }
        info.fragment_ids = index.fragment_ids;
        info.dataset_version = index.dataset_version;
        info.index_version = index.index_version;
        info.type_url = index.details_type_url;
        info.field_ids = index.fields;
        info.created_at = index.created_at;
        for (const auto& f : index.files) {
            info.size_bytes += f.size;
        }
        for (const auto& f : manifest.fragments) {
            if (std::binary_search(index.fragment_ids.begin(), index.fragment_ids.end(),
                                   static_cast<std::uint32_t>(f.id))) {
                info.rows_indexed +=
                    f.physical_rows - (f.deletion_file.present ? f.deletion_file.num_deleted_rows : 0U);
            }
        }
        out.push_back(std::move(info));
    }
    return true;
}

bool dataset_explain_filter(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                            const std::string& filter, std::vector<std::string>& lines, std::string& error) {
    error.clear();
    lines.clear();
    pb::Manifest manifest;
    if (has_version) {
        if (!load_manifest_version(dataset_path, version, manifest, error)) {
            return false;
        }
    } else if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        return false;
    }
    expr::Expression parsed;
    if (!expr::Expression::parse(filter, parsed, error)) {
        return false;
    }
    IndexCandidates candidates;
    if (!index_candidates(dataset_path, manifest, parsed, candidates, error)) {
        return false;
    }
    lines = std::move(candidates.used);
    return true;
}

bool dataset_drop_index(const std::filesystem::path& dataset_path, const std::string& name,
                        std::uint64_t& new_version, std::string& error) {
    error.clear();
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        return false;
    }
    const auto before = manifest.indices.size();
    manifest.indices.erase(std::remove_if(manifest.indices.begin(), manifest.indices.end(),
                                          [&](const pb::IndexMetadata& i) {
                                              return i.name == name && !is_system_index(i);
                                          }),
                           manifest.indices.end());
    if (manifest.indices.size() == before) {
        error = "index '" + name + "' not found";
        return false;
    }
    return commit_next_version(dataset_path, std::move(manifest), new_version, error);
}

}  // namespace nano_lance
