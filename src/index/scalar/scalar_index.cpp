// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Building Lance's scalar indices (scalar_index.hpp). The column is scanned with its row addresses
// (deleted rows left out, as Lance's training scan leaves them), its values -- or for a label list,
// its lists' elements -- sorted nulls first, then ascending, ties by row address, and written in
// Lance's index files; the index is committed in the manifest's index section as Lance commits one.

#include "base/float_text.hpp"
#include "index/index_build.hpp"
#include "nanolance/base_paths.hpp"
#include "nanolance/frag_reuse.hpp"
#include "nanolance/scalar_index.hpp"
#include "nanolance/vector_search.hpp"

#include "index/index_files.hpp"

#include "nanolance/dataset_commit.hpp"
#include "nanolance/deletion_vector.hpp"
#include "nanolance/index_maintenance.hpp"
#include "nanolance/index_search.hpp"
#include "nanolance/lance_file_writer.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/parallel.hpp"
#include "nanolance/roaring_bitmap.hpp"
#include "nanolance/row_ids.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <type_traits>
#include "nanolance/bloom_filter.hpp"
#include "nanolance/fts_tokenizer.hpp"
#include "nanolance/jsonb.hpp"

#include <array>
#include <cctype>
#include <cmath>
#include <chrono>
#include <charconv>
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

enum class KeyKind { Int, UInt, Float16, Float32, Float64, Bool, Bytes, Decimal128, Decimal256 };

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
    case NANOARROW_TYPE_DECIMAL256:
        kind = KeyKind::Decimal256;
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

/// Stable LSD radix sort of `v` by `bits(entry)`, an unsigned integer, a byte per pass; a byte all
/// keys share is skipped (ids 0..5M take 3 passes of 8). Entries that tie keep their order. A
/// comparison sort of 5M random int64 keys was most of a BTREE build.
template <typename T, typename Bits>
void radix_sort(std::vector<T>& v, Bits bits) {
    using U = decltype(bits(v.front()));
    constexpr std::size_t kBytes = sizeof(U);
    const std::size_t n = v.size();
    if (n < 2048U) {
        std::stable_sort(v.begin(), v.end(), [&](const T& a, const T& b) { return bits(a) < bits(b); });
        return;
    }
    // Keys already in order (row ids, timestamps appended in time order) stay as they are: the input is in
    // item order, so the sorted order, ties included, is the input's. One pass, stopping at the first descent.
    if (std::is_sorted(v.begin(), v.end(), [&](const T& a, const T& b) { return bits(a) < bits(b); })) {
        return;
    }
    std::vector<std::array<std::size_t, 256>> counts(kBytes);
    for (auto& c : counts) {
        c.fill(0);
    }
    for (const auto& e : v) {
        const U key = bits(e);
        for (std::size_t b = 0; b < kBytes; ++b) {
            ++counts[b][static_cast<std::size_t>((key >> (8U * b)) & 0xFFU)];
        }
    }
    std::vector<T> other(n);
    auto* from = &v;
    auto* to = &other;
    for (std::size_t b = 0; b < kBytes; ++b) {
        auto& c = counts[b];
        if (std::any_of(c.begin(), c.end(), [n](std::size_t k) { return k == n; })) {
            continue;  // every key has the same byte here
        }
        std::size_t at = 0;
        for (auto& k : c) {
            const auto here = k;
            k = at;
            at += here;
        }
        for (const auto& e : *from) {
            (*to)[c[static_cast<std::size_t>((bits(e) >> (8U * b)) & 0xFFU)]++] = e;
        }
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

/// A decimal256's value: the four 64-bit words of its two's-complement integer, least significant
/// first (the top word is signed, so the sign decides the order before the lower words do).
struct Int256 {
    std::array<std::uint64_t, 4> w{};
    bool operator<(const Int256& o) const {
        if (w[3] != o.w[3]) {
            return static_cast<std::int64_t>(w[3]) < static_cast<std::int64_t>(o.w[3]);
        }
        for (std::size_t k = 3; k-- > 0;) {
            if (w[k] != o.w[k]) {
                return w[k] < o.w[k];
            }
        }
        return false;
    }
    bool operator==(const Int256& o) const { return w == o.w; }
};

struct Int256Hash {
    std::size_t operator()(const Int256& v) const {
        std::uint64_t h = 0;
        for (const auto word : v.w) {
            h = (h ^ word) * 0x9E3779B97F4A7C15ULL;
        }
        return std::hash<std::uint64_t>()(h);
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
template <>
struct KeyHash<Int256> {
    using type = Int256Hash;
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
    // Items are in row-address order, so the item number breaks ties by address (a stable sort
    // keeps it).
    if constexpr (std::is_integral_v<K>) {
        using U = std::make_unsigned_t<K>;
        radix_sort(entries, [](const Entry& e) {
            // Signed keys with the sign bit flipped order as unsigned ones.
            return std::is_signed_v<K> ? static_cast<U>(static_cast<U>(e.key) ^ (U{1} << (sizeof(U) * 8U - 1U)))
                                       : static_cast<U>(e.key);
        });
    } else {
        parallel_sort(entries, [](const Entry& a, const Entry& b) {
            return a.key < b.key || (!(b.key < a.key) && a.item < b.item);
        });
    }
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
    case KeyKind::Decimal256:
        order_items<Int256>(
            items,
            [](const ArrowArrayView* v, std::int64_t i) {
                Int256 value;  // little-endian: the least significant word first
                std::memcpy(value.w.data(), fixed_value(v, i, 32), 32);
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

/// A pick that names no item: gather writes a null there (a zone with no minimum, say).
constexpr std::uint32_t kNoPick = UINT32_MAX;

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
            if (picks[r] == kNoPick || items.is_null(picks[r])) {
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
    std::vector<std::uint64_t> sorted;  // stable row ids are not in address order
    if (!std::is_sorted(addrs, addrs + n)) {
        sorted.assign(addrs, addrs + n);
        std::sort(sorted.begin(), sorted.end());
        addrs = sorted.data();
    }
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


// ── ngram postings ──────────────────────────────────────────────────────────────────────────────

/// ngram_postings.lance (ngram.rs): one row per distinct trigram token, ascending, with the rows holding
/// it as a RoaringTreemap of row ids. Token 0 holds the null rows; a value's trigrams are its
/// lower-cased, ASCII-folded text's (fts::ngram_tokens). `posting_list` is uncompressed, as Lance's own.
bool write_ngram(const std::filesystem::path& dir, const Items& items, const index_build::NGramPostings* base,
                 std::vector<WrittenFile>& files, std::string& error) {
    std::map<std::uint32_t, std::vector<std::uint64_t>> postings;
    if (base != nullptr) {
        for (const auto& [token, ids] : *base) {
            postings[token].insert(postings[token].end(), ids.begin(), ids.end());
        }
    }
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (items.is_null(k)) {
            postings[0].push_back(items.addr[k]);
            continue;
        }
        const auto sv = ArrowArrayViewGetStringUnsafe(items.views[items.view[k]], items.index[k]);
        const auto folded = fts::lower_and_fold(std::string_view(sv.data, static_cast<std::size_t>(sv.size_bytes)));
        for (const auto token : fts::ngram_tokens(folded)) {
            postings[token].push_back(items.addr[k]);
        }
    }
    std::vector<std::uint32_t> keys;
    std::vector<std::vector<std::uint8_t>> bitmaps;
    keys.reserve(postings.size());
    bitmaps.reserve(postings.size());
    for (auto& [token, ids] : postings) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        keys.push_back(token);
        bitmaps.push_back(roaring::encode_treemap(ids));
    }
    const auto n = static_cast<std::int64_t>(keys.size());
    OwnedSchema tokens_type;
    OwnedSchema binary;
    OwnedSchema schema;
    OwnedArray token_array;
    OwnedArray bitmap_array;
    OwnedArray batch;
    if (!type_schema(NANOARROW_TYPE_UINT32, tokens_type.s) || !type_schema(NANOARROW_TYPE_BINARY, binary.s) ||
        !struct_schema({{"tokens", &tokens_type.s, true}, {"posting_list", &binary.s, false}}, schema.s, error)) {
        return error.empty() ? (error = "out of memory", false) : false;
    }
    // struct_schema drops the children's metadata: posting_list keeps Lance's no-compression tag (POSTINGS_FIELD).
    ArrowBuffer meta;
    if (ArrowMetadataBuilderInit(&meta, nullptr) != NANOARROW_OK ||
        ArrowMetadataBuilderAppend(&meta, ArrowCharView("lance-encoding:compression"), ArrowCharView("none")) !=
            NANOARROW_OK ||
        ArrowSchemaSetMetadata(schema.s.children[1], reinterpret_cast<const char*>(meta.data)) != NANOARROW_OK) {
        ArrowBufferReset(&meta);
        error = "out of memory";
        return false;
    }
    ArrowBufferReset(&meta);
    LanceFileExtras extras;
    if (!uint_array(NANOARROW_TYPE_UINT32, keys.data(), n, sizeof(std::uint32_t), token_array.a, error) ||
        !binary_array(bitmaps, bitmap_array.a, error) ||
        !struct_batch({&token_array.a, &bitmap_array.a}, n, batch.a, error)) {
        return error.empty() ? (error = "out of memory", false) : false;
    }
    return write_file(dir, "ngram_postings.lance", schema.s, batch.a, std::move(extras), files, error);
}

// ── zonemaps ────────────────────────────────────────────────────────────────────────────────────

/// One zone (zoned.rs's ZoneTrainer): up to `rows_per_zone` rows of one fragment, nulls included.
/// Its span runs from its first row's offset to its last one's (deleted rows leave gaps in it).
struct Zone {
    std::uint64_t fragment = 0;
    std::uint64_t start = 0;
    std::uint64_t length = 0;
    std::uint32_t nulls = 0;
    std::uint32_t nans = 0;
    std::uint32_t min = kNoPick;  // the item holding the minimum (kNoPick: no value)
    std::uint32_t max = kNoPick;  // the item holding the maximum
    std::uint32_t nan = kNoPick;  // an item that is NaN
};

/// Cut the items (row-address order) into zones and count each zone's rows, keyed by `zone_of`.
std::vector<Zone> cut_zones(const Items& items, std::uint64_t rows_per_zone, std::vector<std::uint32_t>& zone_of) {
    std::vector<Zone> zones;
    zone_of.assign(items.size(), 0);
    std::uint64_t rows = 0;
    for (std::size_t k = 0; k < items.size(); ++k) {
        const auto fragment = items.addr[k] >> 32U;
        const auto offset = items.addr[k] & 0xFFFFFFFFU;
        if (zones.empty() || zones.back().fragment != fragment || rows == rows_per_zone) {
            Zone z;
            z.fragment = fragment;
            z.start = offset;
            zones.push_back(z);
            rows = 0;
        }
        Zone& z = zones.back();
        z.length = offset - z.start + 1;
        ++rows;
        if (items.is_null(k)) {
            ++z.nulls;
        }
        zone_of[k] = static_cast<std::uint32_t>(zones.size() - 1);
    }
    return zones;
}

/// The zones' NaNs and extreme values, the values compared as `extract`'s keys (nulls are counted by
/// cut_zones). NaN is no minimum or maximum (arrow_stats), and a null is neither.
template <typename K, typename Extract>
void zone_extrema(const Items& items, const std::vector<std::uint32_t>& zone_of, bool floating, Extract extract,
                  std::vector<Zone>& zones) {
    const auto key = [&](std::uint32_t k) { return extract(items.views[items.view[k]], items.index[k]); };
    for (std::size_t k = 0; k < items.size(); ++k) {
        Zone& z = zones[zone_of[k]];
        const auto item = static_cast<std::uint32_t>(k);
        if (items.is_null(k)) {
            continue;
        }
        if (floating && std::isnan(ArrowArrayViewGetDoubleUnsafe(items.views[items.view[k]], items.index[k]))) {
            ++z.nans;
            if (z.nan == kNoPick) {
                z.nan = item;
            }
            continue;
        }
        if (z.min == kNoPick || key(item) < key(z.min)) {
            z.min = item;
        }
        if (z.max == kNoPick || key(z.max) < key(item)) {
            z.max = item;
        }
    }
}

void zone_extrema_of(const Items& items, KeyKind kind, const std::vector<std::uint32_t>& zone_of,
                     std::vector<Zone>& zones) {
    switch (kind) {
    case KeyKind::Int:
    case KeyKind::Bool:
        zone_extrema<std::int64_t>(
            items, zone_of, false, [](const ArrowArrayView* v, std::int64_t i) { return ArrowArrayViewGetIntUnsafe(v, i); },
            zones);
        break;
    case KeyKind::UInt:
        zone_extrema<std::uint64_t>(
            items, zone_of, false, [](const ArrowArrayView* v, std::int64_t i) { return ArrowArrayViewGetUIntUnsafe(v, i); },
            zones);
        break;
    case KeyKind::Float16:
        zone_extrema<std::uint16_t>(
            items, zone_of, true,
            [](const ArrowArrayView* v, std::int64_t i) {
                std::uint16_t bits = 0;
                std::memcpy(&bits, fixed_value(v, i, 2), 2);
                return total_order<std::uint16_t>(bits, 16);
            },
            zones);
        break;
    case KeyKind::Float32:
        zone_extrema<std::uint32_t>(
            items, zone_of, true,
            [](const ArrowArrayView* v, std::int64_t i) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, fixed_value(v, i, 4), 4);
                return total_order<std::uint32_t>(bits, 32);
            },
            zones);
        break;
    case KeyKind::Float64:
        zone_extrema<std::uint64_t>(
            items, zone_of, true,
            [](const ArrowArrayView* v, std::int64_t i) {
                std::uint64_t bits = 0;
                std::memcpy(&bits, fixed_value(v, i, 8), 8);
                return total_order<std::uint64_t>(bits, 64);
            },
            zones);
        break;
    case KeyKind::Decimal128:
        zone_extrema<Int128>(
            items, zone_of, false,
            [](const ArrowArrayView* v, std::int64_t i) {
                Int128 value;
                std::memcpy(&value.lo, fixed_value(v, i, 16), 8);
                std::memcpy(&value.hi, fixed_value(v, i, 16) + 8, 8);
                return value;
            },
            zones);
        break;
    case KeyKind::Decimal256:
        zone_extrema<Int256>(
            items, zone_of, false,
            [](const ArrowArrayView* v, std::int64_t i) {
                Int256 value;
                std::memcpy(value.w.data(), fixed_value(v, i, 32), 32);
                return value;
            },
            zones);
        break;
    case KeyKind::Bytes:
        zone_extrema<StringKey>(
            items, zone_of, false, [](const ArrowArrayView* v, std::int64_t i) { return string_key(v, i, 0); }, zones);
        break;
    }
}

/// `n` nulls of `type` (a nested type: its children are null too, as the layout needs).
bool all_null_array(const ArrowSchema& type, std::int64_t n, ArrowArray& out, std::string& error) {
    ArrowError err;
    if (ArrowArrayInitFromSchema(&out, &type, &err) != NANOARROW_OK || ArrowArrayAppendNull(&out, n) != NANOARROW_OK ||
        ArrowArrayFinishBuildingDefault(&out, &err) != NANOARROW_OK) {
        error = std::string("zonemap: ") + err.message;
        return false;
    }
    return true;
}

/// zonemap.lance: min, max (the column's type), null_count, nan_count, fragment_id, zone_start,
/// zone_length; the null rows' addresses as global buffer 1 (rows_per_zone, null_bitmap in the
/// schema metadata). A zone whose values are all NaN has no minimum, and its maximum is NaN, as
/// Lance writes it (zonemap.rs's max_value_from_stats).
bool write_zonemap(const std::filesystem::path& dir, const ArrowSchema& type, const Items& items,
                   const std::vector<Zone>& zones, bool floating, bool nested, std::uint64_t rows_per_zone,
                   std::vector<WrittenFile>& files, std::string& error) {
    const auto n = static_cast<std::int64_t>(zones.size());
    std::vector<std::uint32_t> mins;
    std::vector<std::uint32_t> maxs;
    std::vector<std::uint32_t> nulls;
    std::vector<std::uint32_t> nans;
    std::vector<std::uint64_t> fragments;
    std::vector<std::uint64_t> starts;
    std::vector<std::uint64_t> lengths;
    for (const auto& z : zones) {
        mins.push_back(z.min);
        maxs.push_back(floating && z.nans > 0 ? z.nan : z.max);
        nulls.push_back(z.nulls);
        nans.push_back(z.nans);
        fragments.push_back(z.fragment);
        starts.push_back(z.start);
        lengths.push_back(z.length);
    }
    std::vector<std::uint64_t> null_addrs;
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (items.is_null(k)) {
            null_addrs.push_back(items.addr[k]);
        }
    }
    const auto null_bitmap = serialize_treemap(null_addrs.data(), null_addrs.size());

    OwnedSchema schema;
    OwnedSchema u32;
    OwnedSchema u64;
    OwnedArray min_array;
    OwnedArray max_array;
    OwnedArray null_array;
    OwnedArray nan_array;
    OwnedArray fragment_array;
    OwnedArray start_array;
    OwnedArray length_array;
    OwnedArray batch;
    LanceFileExtras extras;
    extras.schema_metadata["rows_per_zone"] = bytes_of(std::to_string(rows_per_zone));
    extras.schema_metadata["null_bitmap"] = bytes_of("1");
    extras.global_buffers.push_back(null_bitmap);
    if (!type_schema(NANOARROW_TYPE_UINT32, u32.s) || !type_schema(NANOARROW_TYPE_UINT64, u64.s) ||
        !struct_schema({{"min", &type, true}, {"max", &type, true}, {"null_count", &u32.s, false},
                        {"nan_count", &u32.s, false}, {"fragment_id", &u64.s, false},
                        {"zone_start", &u64.s, false}, {"zone_length", &u64.s, false}},
                       schema.s, error) ||
        !(nested ? all_null_array(type, n, min_array.a, error) && all_null_array(type, n, max_array.a, error)
                 : gather(type, items, mins, min_array.a, error) && gather(type, items, maxs, max_array.a, error)) ||
        !uint_array(NANOARROW_TYPE_UINT32, nulls.data(), n, 4, null_array.a, error) ||
        !uint_array(NANOARROW_TYPE_UINT32, nans.data(), n, 4, nan_array.a, error) ||
        !uint_array(NANOARROW_TYPE_UINT64, fragments.data(), n, 8, fragment_array.a, error) ||
        !uint_array(NANOARROW_TYPE_UINT64, starts.data(), n, 8, start_array.a, error) ||
        !uint_array(NANOARROW_TYPE_UINT64, lengths.data(), n, 8, length_array.a, error) ||
        !struct_batch({&min_array.a, &max_array.a, &null_array.a, &nan_array.a, &fragment_array.a, &start_array.a,
                       &length_array.a},
                      n, batch.a, error)) {
        return error.empty() ? (error = "out of memory", false) : false;
    }
    return write_file(dir, "zonemap.lance", schema.s, batch.a, std::move(extras), files, error);
}

// ── bloom filters ───────────────────────────────────────────────────────────────────────────────

/// bloomfilter.lance: per zone its fragment, span, whether it holds a null, and its bloom filter of
/// the non-null values (Lance's BloomFilterIndex layout). The null rows' addresses are global buffer 1;
/// number_of_items and probability are in the schema metadata.
bool write_bloomfilter(const std::filesystem::path& dir, const Items& items,
                       const std::vector<Zone>& zones, const std::vector<std::uint32_t>& zone_of,
                       std::uint8_t log2_bytes, std::uint64_t number_of_items, double probability,
                       std::size_t width, std::vector<WrittenFile>& files, std::string& error) {
    const auto n = static_cast<std::int64_t>(zones.size());
    const std::size_t num_bytes = std::size_t{1} << log2_bytes;
    std::vector<std::vector<std::uint8_t>> filters(zones.size(), bloom_filter::empty_filter(num_bytes));
    std::vector<std::uint64_t> null_addrs;
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (items.is_null(k)) {
            null_addrs.push_back(items.addr[k]);
            continue;
        }
        const auto hash = index_files::bloom_hash(items.views[items.view[k]], items.index[k], width);
        bloom_filter::insert(filters[zone_of[k]].data(), num_bytes, hash);
    }
    const auto null_bitmap = serialize_treemap(null_addrs.data(), null_addrs.size());

    std::vector<std::uint64_t> fragments;
    std::vector<std::uint64_t> starts;
    std::vector<std::uint64_t> lengths;
    for (const auto& z : zones) {
        fragments.push_back(z.fragment);
        starts.push_back(z.start);
        lengths.push_back(z.length);
    }

    OwnedSchema schema;
    OwnedSchema u64;
    OwnedSchema boolean;
    OwnedSchema binary;
    OwnedArray fragment_array;
    OwnedArray start_array;
    OwnedArray length_array;
    OwnedArray has_null_array;
    OwnedArray filter_array;
    OwnedArray batch;
    LanceFileExtras extras;
    // Lance's metadata: the probability as Rust prints an f64 (no exponent), read back as the same value.
    const std::string probability_text = float_text::to_text(probability, float_text::Format::Fixed);
    extras.schema_metadata["bloomfilter_item"] = bytes_of(std::to_string(number_of_items));
    extras.schema_metadata["bloomfilter_probability"] = bytes_of(probability_text);
    extras.schema_metadata["null_bitmap"] = bytes_of("1");
    extras.global_buffers.push_back(null_bitmap);
    ArrowError err;
    if (!type_schema(NANOARROW_TYPE_UINT64, u64.s) || !type_schema(NANOARROW_TYPE_BOOL, boolean.s) ||
        !type_schema(NANOARROW_TYPE_BINARY, binary.s) ||
        !struct_schema({{"fragment_id", &u64.s, false}, {"zone_start", &u64.s, false},
                        {"zone_length", &u64.s, false}, {"has_null", &boolean.s, false},
                        {"bloom_filter_data", &binary.s, false}},
                       schema.s, error) ||
        !uint_array(NANOARROW_TYPE_UINT64, fragments.data(), n, 8, fragment_array.a, error) ||
        !uint_array(NANOARROW_TYPE_UINT64, starts.data(), n, 8, start_array.a, error) ||
        !uint_array(NANOARROW_TYPE_UINT64, lengths.data(), n, 8, length_array.a, error) ||
        !binary_array(filters, filter_array.a, error)) {
        return error.empty() ? (error = "out of memory", false) : false;
    }
    if (ArrowArrayInitFromType(&has_null_array.a, NANOARROW_TYPE_BOOL) != NANOARROW_OK ||
        ArrowArrayStartAppending(&has_null_array.a) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    for (const auto& z : zones) {
        if (ArrowArrayAppendInt(&has_null_array.a, z.nulls > 0 ? 1 : 0) != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
    }
    if (ArrowArrayFinishBuildingDefault(&has_null_array.a, &err) != NANOARROW_OK ||
        !struct_batch({&fragment_array.a, &start_array.a, &length_array.a, &has_null_array.a, &filter_array.a}, n,
                      batch.a, error)) {
        return error.empty() ? (error = "out of memory", false) : false;
    }
    return write_file(dir, "bloomfilter.lance", schema.s, batch.a, std::move(extras), files, error);
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
    } else if (upper == "ZONEMAP") {
        type = ScalarIndexType::ZoneMap;
    } else if (upper == "BLOOMFILTER") {
        type = ScalarIndexType::BloomFilter;
    } else if (upper == "NGRAM") {
        type = ScalarIndexType::NGram;
    } else {
        return false;
    }
    return true;
}

namespace {

/// A ZONEMAP on a fixed-size list (the only nested type a zonemap is built on here; list columns take
/// LABEL_LIST).
bool zoned_nested(ScalarIndexType type, const ArrowSchemaView& view) {
    return type == ScalarIndexType::ZoneMap && view.type == NANOARROW_TYPE_FIXED_SIZE_LIST;
}

// ── JSON indexes: the values at a path of a JSON column, typed as Lance types them (json.rs) ──

/// The paths a JSON index takes: "" or "$" (the whole value), then an optional "$" and dot-separated
/// member names. A name is [A-Za-z0-9_]+; anything else is refused here, not sent to Lance's JSONPath.
bool parse_json_path(const std::string& path, std::vector<std::string>& names, std::string& error) {
    names.clear();
    std::string_view rest = path;
    const bool dollar = !rest.empty() && rest.front() == '$';
    if (dollar) {
        rest.remove_prefix(1);
    }
    bool ok = true;
    if (dollar && !rest.empty()) {
        ok = rest.front() == '.';
        rest.remove_prefix(ok ? 1 : 0);
    }
    while (ok && !rest.empty()) {
        const auto dot = rest.find('.');
        const auto name = rest.substr(0, dot);
        ok = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        });
        names.emplace_back(name);
        if (dot == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(dot + 1);
        ok = ok && !rest.empty();
    }
    if (!ok) {
        names.clear();
        error = "JSON index path '" + path + "' is not supported yet (supported: an optional '$', then dot-separated names)";
    }
    return ok;
}

/// A JSON value's type as Lance's JsonbType names it (json.rs).
enum class JsonbType { Boolean, Int64, Float64, String, Array, Object };
/// The type a path's values are indexed as: the Arrow type Lance infers from the first non-null value.
enum class JsonTarget { Boolean, Int64, Float64, String, Binary };

const char* jsonb_name(JsonbType t) {
    switch (t) {
    case JsonbType::Boolean: return "Boolean";
    case JsonbType::Int64: return "Int64";
    case JsonbType::Float64: return "Float64";
    case JsonbType::String: return "String";
    case JsonbType::Array: return "Array";
    case JsonbType::Object: return "Object";
    }
    return "";
}

const char* target_name(JsonTarget t) {
    switch (t) {
    case JsonTarget::Boolean: return "Boolean";
    case JsonTarget::Int64: return "Int64";
    case JsonTarget::Float64: return "Float64";
    case JsonTarget::String: return "Utf8";
    case JsonTarget::Binary: return "LargeBinary";
    }
    return "";
}

JsonTarget target_of(JsonbType t) {
    switch (t) {
    case JsonbType::Boolean: return JsonTarget::Boolean;
    case JsonbType::Int64: return JsonTarget::Int64;
    case JsonbType::Float64: return JsonTarget::Float64;
    case JsonbType::String: return JsonTarget::String;
    case JsonbType::Array:
    case JsonbType::Object: return JsonTarget::Binary;
    }
    return JsonTarget::Binary;
}

/// Lance's validate (json.rs): an Int64 value fits a Float64 path; nothing else converts.
bool json_compatible(JsonTarget target, JsonbType actual) {
    switch (target) {
    case JsonTarget::Boolean: return actual == JsonbType::Boolean;
    case JsonTarget::Int64: return actual == JsonbType::Int64;
    case JsonTarget::Float64: return actual == JsonbType::Int64 || actual == JsonbType::Float64;
    case JsonTarget::String: return actual == JsonbType::String;
    case JsonTarget::Binary: return actual == JsonbType::Array || actual == JsonbType::Object;
    }
    return false;
}

/// The value at `names` in row `row` of a JSON column. `has` is false for a null row, a missing member
/// or a JSON null (Lance's null).
bool json_value_at(const ArrowArrayView* column, std::int64_t row, const std::vector<std::string>& names,
                   jsonb::Item& item, JsonbType& type, bool& has, std::string& error) {
    has = false;
    if (ArrowArrayViewIsNull(column, row)) {
        return true;
    }
    const auto bytes = ArrowArrayViewGetBytesUnsafe(column, row);
    if (!jsonb::Item::root(std::string_view(bytes.data.as_char, static_cast<std::size_t>(bytes.size_bytes)), item)) {
        error = "a JSON index column holds a value that is not JSONB";
        return false;
    }
    for (const auto& name : names) {
        jsonb::Item next;
        if (item.type() != jsonb::Item::Type::Object || !item.get(name, next)) {
            return true;
        }
        item = next;
    }
    switch (item.type()) {
    case jsonb::Item::Type::Null:
        return true;
    case jsonb::Item::Type::True:
    case jsonb::Item::Type::False:
        type = JsonbType::Boolean;
        break;
    case jsonb::Item::Type::Number: {
        jsonb::Number n;
        if (!item.number(n)) {
            error = "a JSON index column holds an invalid number";
            return false;
        }
        const bool integral = n.kind == jsonb::Number::Kind::Int ||
                              (n.kind == jsonb::Number::Kind::UInt && n.u <= static_cast<std::uint64_t>(INT64_MAX));
        type = integral ? JsonbType::Int64 : JsonbType::Float64;
        break;
    }
    case jsonb::Item::Type::String:
        type = JsonbType::String;
        break;
    case jsonb::Item::Type::Array:
        type = JsonbType::Array;
        break;
    case jsonb::Item::Type::Object:
        type = JsonbType::Object;
        break;
    case jsonb::Item::Type::Other:
        error = "a JSON index does not take values of JSON extension types";
        return false;
    }
    has = true;
    return true;
}

/// Writes the JsonIndexDetails of a JSON index (lance index.proto): path = 1, target_details = 2, an Any
/// naming the BTree details (which are empty).
std::vector<std::uint8_t> json_index_details(const std::string& path) {
    const auto put_len = [](std::vector<std::uint8_t>& out, std::uint32_t field, std::string_view bytes) {
        std::uint64_t v = (static_cast<std::uint64_t>(field) << 3U) | 2U;
        const auto varint = [&](std::uint64_t x) {
            while (x >= 0x80U) {
                out.push_back(static_cast<std::uint8_t>((x & 0x7FU) | 0x80U));
                x >>= 7U;
            }
            out.push_back(static_cast<std::uint8_t>(x));
        };
        varint(v);
        varint(bytes.size());
        out.insert(out.end(), bytes.begin(), bytes.end());
    };
    std::vector<std::uint8_t> any;  // google.protobuf.Any: type_url = 1
    put_len(any, 1, "/lance.table.BTreeIndexDetails");
    std::vector<std::uint8_t> out;
    if (!path.empty()) {
        put_len(out, 1, path);
    }
    put_len(out, 2, std::string_view(reinterpret_cast<const char*>(any.data()), any.size()));
    return out;
}

/// The values of a JSON index, one array per scanned batch, typed as Lance types the path: the first
/// non-null value's type, Int64 values allowed in a Float64 path, Utf8 when every value is null. The
/// values are checked in row order and the first mismatch is the error, as Lance words it.
bool extract_json_values(const std::vector<ArrowArray>& batches, const std::vector<std::size_t>& path_index,
                         const ArrowSchema& column_type, const std::vector<std::string>& names,
                         const std::string& path, OwnedSchema& type, std::vector<ArrowArray>& out,
                         std::string& error) {
    const auto column_of = [&](const ArrowArray& batch) {
        const ArrowArray* arr = batch.children[0];
        for (const auto c : path_index) {
            arr = arr->children[c];
        }
        return arr;
    };
    OwnedViews views;
    const auto view_of = [&](const ArrowArray* arr, const ArrowSchema& schema) -> const ArrowArrayView* {
        auto view = std::make_unique<ArrowArrayView>();
        ArrowError err;
        if (ArrowArrayViewInitFromSchema(view.get(), &schema, &err) != NANOARROW_OK ||
            ArrowArrayViewSetArray(view.get(), arr, &err) != NANOARROW_OK) {
            ArrowArrayViewReset(view.get());
            error = err.message;
            return nullptr;
        }
        views.v.push_back(std::move(view));
        return views.v.back().get();
    };
    // Pass 1: the type, and every value checked.
    std::optional<JsonTarget> target;
    for (const auto& batch : batches) {
        const ArrowArray* arr = column_of(batch);
        const ArrowArrayView* view = view_of(arr, column_type);
        if (view == nullptr) {
            return false;
        }
        for (std::int64_t r = 0; r < arr->length; ++r) {
            jsonb::Item item;
            JsonbType actual{};
            bool has = false;
            if (!json_value_at(view, r, names, item, actual, has, error)) {
                return false;
            }
            if (!has) {
                continue;
            }
            if (!target) {
                target = target_of(actual);
            } else if (!json_compatible(*target, actual)) {
                error = "Invalid user input: JSON path '" + path + "' expected " + target_name(*target) + ", but batch row " +
                        std::to_string(r) + " has JSON type " + jsonb_name(actual);
                return false;
            }
        }
    }
    const JsonTarget type_of = target.value_or(JsonTarget::String);
    const ArrowType arrow_type = type_of == JsonTarget::Boolean   ? NANOARROW_TYPE_BOOL
                                 : type_of == JsonTarget::Int64   ? NANOARROW_TYPE_INT64
                                 : type_of == JsonTarget::Float64 ? NANOARROW_TYPE_DOUBLE
                                 : type_of == JsonTarget::String  ? NANOARROW_TYPE_STRING
                                                                  : NANOARROW_TYPE_LARGE_BINARY;
    ArrowSchemaInit(&type.s);
    if (ArrowSchemaSetType(&type.s, arrow_type) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    // Pass 2: the values, typed.
    for (const auto& batch : batches) {
        const ArrowArray* arr = column_of(batch);
        const ArrowArrayView* view = view_of(arr, column_type);
        if (view == nullptr) {
            return false;
        }
        ArrowArray built;
        if (ArrowArrayInitFromType(&built, arrow_type) != NANOARROW_OK ||
            ArrowArrayStartAppending(&built) != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
        for (std::int64_t r = 0; r < arr->length; ++r) {
            jsonb::Item item;
            JsonbType actual{};
            bool has = false;
            if (!json_value_at(view, r, names, item, actual, has, error)) {
                built.release(&built);
                return false;
            }
            ArrowErrorCode code = NANOARROW_OK;
            if (!has) {
                code = ArrowArrayAppendNull(&built, 1);
            } else if (type_of == JsonTarget::Boolean) {
                code = ArrowArrayAppendInt(&built, item.type() == jsonb::Item::Type::True ? 1 : 0);
            } else if (type_of == JsonTarget::Int64) {
                jsonb::Number n;
                item.number(n);
                code = ArrowArrayAppendInt(&built, n.kind == jsonb::Number::Kind::Int ? n.i : static_cast<std::int64_t>(n.u));
            } else if (type_of == JsonTarget::Float64) {
                jsonb::Number n;
                item.number(n);
                code = ArrowArrayAppendDouble(&built, n.as_double());
            } else if (type_of == JsonTarget::String) {
                const auto s = item.string();
                ArrowBufferView v;
                v.data.as_char = s.data();
                v.size_bytes = static_cast<std::int64_t>(s.size());
                code = ArrowArrayAppendBytes(&built, v);
            } else {
                const auto doc = item.document();
                ArrowBufferView v;
                v.data.as_char = doc.data();
                v.size_bytes = static_cast<std::int64_t>(doc.size());
                code = ArrowArrayAppendBytes(&built, v);
            }
            if (code != NANOARROW_OK) {
                built.release(&built);
                error = "out of memory";
                return false;
            }
        }
        if (ArrowArrayFinishBuildingDefault(&built, nullptr) != NANOARROW_OK) {
            built.release(&built);
            error = "out of memory";
            return false;
        }
        out.push_back(built);
    }
    return true;
}

bool create_scalar_index(const std::filesystem::path& dataset_path, const std::string& column, ScalarIndexType type,
                         const ScalarIndexOptions& options, const index_build::SegmentTarget* target,
                         std::uint64_t& new_version, std::string& error) {
    error.clear();
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (target != nullptr && target->manifest != nullptr) {
        manifest = *target->manifest;
        version = target->version;
    } else if (!load_read_version(dataset_path, options.read_version, manifest, version, error)) {
        return false;
    }
    const bool commit = target == nullptr || target->out == nullptr;
    AddressToRowId to_row_id;  // stable row ids go into the index, row addresses otherwise
    if (!AddressToRowId::build(manifest, to_row_id, error)) {
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
    const index_build::Progress* progress = target != nullptr ? target->progress : nullptr;
    OwnedSchema scanned;
    OwnedBatches batches;
    if (!index_build::stage(progress, "load_data", 0, "rows",
                            [&] { return lance_dataset_scan(dataset_path, request, scanned.s, batches.v, error); })) {
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
    // A JSON index: the values at its path, typed as Lance types them. Its BTree is built over those.
    const bool json = options.json_path.has_value();
    OwnedSchema json_type;
    OwnedBatches json_values;
    if (json) {
        std::vector<std::string> json_names;
        if (type != ScalarIndexType::BTree || !parse_json_path(*options.json_path, json_names, error)) {
            return false;
        }
        const bool binary = col_view.storage_type == NANOARROW_TYPE_BINARY ||
                            col_view.storage_type == NANOARROW_TYPE_LARGE_BINARY;
        if (!binary) {
            error = "Invalid user input: A JSON index can only be created on a Binary or LargeBinary field.";
            return false;
        }
        if (col_view.extension_name.data == nullptr ||
            std::string(col_view.extension_name.data, static_cast<std::size_t>(col_view.extension_name.size_bytes)) !=
                "lance.json") {
            error = "a JSON index needs a JSON column; '" + column + "' is " + ArrowTypeString(col_view.type);
            return false;
        }
        if (!extract_json_values(batches.v, path_index, *col_type, json_names, *options.json_path, json_type,
                                 json_values.v, error)) {
            return false;
        }
        col_type = &json_type.s;
    }
    // The scanned column's array in a batch: the JSON values, or the column below its struct path.
    const auto column_of = [&](std::size_t b) -> const ArrowArray* {
        if (json) {
            return &json_values.v[b];
        }
        const ArrowArray* arr = batches.v[b].children[0];
        for (const auto c : path_index) {
            arr = arr->children[c];
        }
        return arr;
    };
    const bool list = col_view.type == NANOARROW_TYPE_LIST || col_view.type == NANOARROW_TYPE_LARGE_LIST;
    if (type == ScalarIndexType::LabelList && !list) {
        error = "a LABEL_LIST index needs a list column; '" + column + "' is " + ArrowTypeString(col_view.type);
        return false;
    }
    if (type != ScalarIndexType::LabelList && list) {
        error = "a list column takes a LABEL_LIST index, not a " +
                std::string(type == ScalarIndexType::BTree         ? "BTREE"
                            : type == ScalarIndexType::ZoneMap      ? "ZONEMAP"
                            : type == ScalarIndexType::NGram       ? "NGRAM"
                            : type == ScalarIndexType::BloomFilter ? "BLOOMFILTER"
                                                                   : "BITMAP");
        return false;
    }
    if (type == ScalarIndexType::ZoneMap && options.rows_per_zone > UINT32_MAX) {
        error = "rows_per_zone is over 4 billion";
        return false;
    }
    const std::uint64_t rows_per_zone = options.rows_per_zone != 0 ? options.rows_per_zone : 8192;  // Lance's default
    const ArrowSchema* key_type = list ? col_type->children[0] : col_type;
    ArrowSchemaView key_view;
    KeyKind kind{};
    if (ArrowSchemaViewInit(&key_view, key_type, &err) != NANOARROW_OK) {
        error = err.message;
        return false;
    }
    // A uuid (pa.uuid(), the arrow.uuid extension over fixed_size_binary(16)) is keyed by its storage
    // type, as pylance writes it: its index files hold fixed_size_binary values, not the extension.
    const bool uuid_key =
        key_view.extension_name.data != nullptr &&
        std::string(key_view.extension_name.data, static_cast<std::size_t>(key_view.extension_name.size_bytes)) ==
            "arrow.uuid" &&
        key_view.type == NANOARROW_TYPE_FIXED_SIZE_BINARY && !list;
    OwnedSchema uuid_storage;
    if (uuid_key) {
        ArrowSchemaInit(&uuid_storage.s);
        ArrowSchemaSetTypeFixedSize(&uuid_storage.s, NANOARROW_TYPE_FIXED_SIZE_BINARY, key_view.fixed_size);
        key_type = &uuid_storage.s;
        if (ArrowSchemaViewInit(&key_view, key_type, &err) != NANOARROW_OK) {
            error = err.message;
            return false;
        }
    }
    if (type == ScalarIndexType::NGram && key_view.type != NANOARROW_TYPE_STRING &&
        key_view.type != NANOARROW_TYPE_LARGE_STRING) {
        error = "an NGRAM index needs a string column; '" + column + "' is " + ArrowTypeString(key_view.type);
        return false;
    }
    // A zonemap of a fixed-size list keeps only the null counts (zonemap.rs: nested types have no min or max).
    // So does one of a fixed-size binary (uuids included): pylance's zonemap writes null min and max
    // for fixed_size_binary, measured on 12.0.0.
    const bool nested = zoned_nested(type, key_view);
    const bool no_extrema =
        nested || (type == ScalarIndexType::ZoneMap && key_view.type == NANOARROW_TYPE_FIXED_SIZE_BINARY);
    const bool bloom = type == ScalarIndexType::BloomFilter;
    std::size_t value_width = 0;
    if (bloom) {
        if (key_view.extension_name.data != nullptr || !index_files::bloom_value_width(key_view, value_width, error)) {
            if (error.empty()) {
                error = "a BLOOMFILTER index on an extension type is not supported";
            }
            return false;
        }
    } else if (!nested && (key_view.extension_name.data != nullptr || !key_kind_of(key_view, kind, error))) {
        if (error.empty()) {
            error = "a scalar index on an extension type is not supported";
        }
        return false;
    }
    // BLOOMFILTER: Lance's BloomFilterIndexBuilderParams defaults, checked as its filter checks them.
    const std::uint64_t number_of_items = options.number_of_items != 0 ? options.number_of_items : 8192;
    const double probability = options.probability != 0.0 ? options.probability : 0.00057;
    if (bloom && !(probability > 0.0 && probability < 1.0)) {
        error = "a BLOOMFILTER probability must be between 0 and 1";
        return false;
    }

    // The values (or list elements) in row-address order; a label list's null lists aside. A zonemap
    // keeps the row addresses themselves (its zones are of fragments and offsets), not stable row ids.
    const bool zoned = type == ScalarIndexType::ZoneMap || bloom;
    Items items;
    OwnedViews views;
    std::vector<std::uint64_t> null_lists;
    std::size_t capacity = 0;
    for (std::size_t b = 0; b < batches.v.size(); ++b) {
        const ArrowArray* arr = column_of(b);
        capacity += static_cast<std::size_t>(list ? arr->children[0]->length : arr->length);
    }
    items.view.reserve(capacity);
    items.index.reserve(capacity);
    items.addr.reserve(capacity);
    for (std::size_t b = 0; b < batches.v.size(); ++b) {
        const auto& batch = batches.v[b];
        const ArrowArray* arr = column_of(b);
        const ArrowArray* addr_arr = batch.children[batch.n_children - 1];
        const auto* addrs = static_cast<const std::uint64_t*>(addr_arr->buffers[1]) + addr_arr->offset;
        auto view = std::make_unique<ArrowArrayView>();
        // The values are read through the storage type: an extension's view has no element width.
        if (ArrowArrayViewInitFromSchema(view.get(), uuid_key ? &uuid_storage.s : col_type, &err) != NANOARROW_OK ||
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
                items.push(vi, r, zoned ? addrs[r] : to_row_id(addrs[r]));
            }
            continue;
        }
        const auto vi = static_cast<std::uint32_t>(items.views.size());
        items.views.push_back(v->children[0]);
        for (std::int64_t r = 0; r < batch.length; ++r) {
            if (ArrowArrayViewIsNull(v, r)) {
                null_lists.push_back(to_row_id(addrs[r]));
                continue;
            }
            const auto begin = ArrowArrayViewListChildOffset(v, r);
            const auto end = ArrowArrayViewListChildOffset(v, r + 1);
            for (auto e = begin; e < end; ++e) {
                items.push(vi, e, to_row_id(addrs[r]));
            }
        }
    }
    if (items.size() > UINT32_MAX) {
        error = "over 4 billion values to index";
        return false;
    }
    Sorted sorted;
    std::vector<std::uint32_t> zone_of;
    std::vector<Zone> zones;
    if (zoned) {
        zones = cut_zones(items, bloom ? number_of_items : rows_per_zone, zone_of);
        if (!no_extrema && !bloom) {
            zone_extrema_of(items, kind, zone_of, zones);
        }
    } else if (type != ScalarIndexType::NGram) {
        sort_items(items, kind, type != ScalarIndexType::BTree, sorted);
    }

    const auto uuid = target != nullptr && target->uuid != nullptr ? *target->uuid : new_uuid();
    const auto dir = dataset_path / "_indices" / pb::uuid_string(uuid);
    if (!index_files::create_index_dir(dir, error)) {
        return false;
    }
    std::vector<WrittenFile> files;
    std::string details;
    std::vector<std::uint8_t> details_value;  // a JSON index's JsonIndexDetails
    std::uint32_t index_version = 0;
    bool written = false;
    const char* write_stage = type == ScalarIndexType::BTree    ? "write_lookup_file"
                              : type == ScalarIndexType::Bitmap ? "write_bitmap_index"
                              : type == ScalarIndexType::ZoneMap ? "write_zonemap_index"
                              : type == ScalarIndexType::NGram  ? "write_ngram_index"
                              : bloom                            ? "write_bloomfilter_index"
                                                                 : "write_label_list_index";
    if (progress != nullptr) {
        (*progress)(0, write_stage, 1, "files", 0);
    }
    switch (type) {
    case ScalarIndexType::BTree:
        written = write_btree(dir, *key_type, items, sorted, files, error);
        details = json ? "/lance.index.pb.JsonIndexDetails" : "/lance.table.BTreeIndexDetails";
        if (json) {
            details_value = json_index_details(*options.json_path);
        }
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
    case ScalarIndexType::ZoneMap:
        written = write_zonemap(dir, *key_type, items, zones,
                                kind == KeyKind::Float16 || kind == KeyKind::Float32 || kind == KeyKind::Float64,
                                no_extrema, rows_per_zone, files, error);
        details = "/lance.table.ZoneMapIndexDetails";
        break;
    case ScalarIndexType::NGram:
        written = write_ngram(dir, items, target != nullptr ? target->postings : nullptr, files, error);
        details = "/lance.table.NGramIndexDetails";
        break;
    case ScalarIndexType::BloomFilter:
        written = write_bloomfilter(dir, items, zones, zone_of,
                                    bloom_filter::log2_bytes_for(number_of_items, probability), number_of_items,
                                    probability, value_width, files, error);
        details = "/lance.index.pb.BloomFilterIndexDetails";
        break;
    }
    if (!written) {
        index_files::remove_index_dir(dir);
        return false;
    }
    if (progress != nullptr) {
        (*progress)(2, write_stage, 0, "", 0);
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
                                                       index_version, static_cast<std::uint64_t>(now), index_files, details_value);
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
        index_files::remove_index_dir(dir);
        return false;
    }
    return true;
}

}  // namespace

bool dataset_create_scalar_index(const std::filesystem::path& dataset_path, const std::string& column,
                                 ScalarIndexType type, const ScalarIndexOptions& options,
                                 std::uint64_t& new_version, std::string& error) {
    // Built from the version the caller read; a retryable conflict re-runs the build on the latest.
    return retry_from_read_version(
        options.read_version,
        [&](ReadVersion from) {
            ScalarIndexOptions o = options;
            o.read_version = from;
            return create_scalar_index(dataset_path, column, type, o, nullptr, new_version, error);
        },
        error);
}

bool index_build::build_scalar_segment(const std::filesystem::path& dataset_path, const std::string& column,
                                       ScalarIndexType type, const ScalarIndexOptions& options,
                                       const SegmentTarget& target, std::string& error) {
    std::uint64_t unused = 0;
    return create_scalar_index(dataset_path, column, type, options, &target, unused, error);
}

bool index_build::load_ngram_postings(const std::filesystem::path& dir, NGramPostings& out, std::string& error) {
    out.clear();
    index_files::FileTable table;
    const std::vector<std::string> columns = {"tokens", "posting_list"};
    if (!index_files::read_table(dir / "ngram_postings.lance", &columns, LanceRowRange{}, table, error)) {
        return false;
    }
    if (table.schema.n_children != 2) {
        error = "ngram index: ngram_postings.lance has no tokens and posting_list columns";
        return false;
    }
    for (const auto& batch : table.batches) {
        ArrowArrayView tokens;
        ArrowArrayView lists;
        ArrowError e{};
        if (ArrowArrayViewInitFromSchema(&tokens, &table.type(0), &e) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&tokens, batch.children[0], &e) != NANOARROW_OK ||
            ArrowArrayViewInitFromSchema(&lists, &table.type(1), &e) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&lists, batch.children[1], &e) != NANOARROW_OK) {
            ArrowArrayViewReset(&tokens);
            ArrowArrayViewReset(&lists);
            error = std::string("ngram index: ") + e.message;
            return false;
        }
        bool ok = true;
        for (std::int64_t r = 0; ok && r < batch.length; ++r) {
            if (ArrowArrayViewIsNull(&tokens, r) || ArrowArrayViewIsNull(&lists, r)) {
                continue;
            }
            const auto token = static_cast<std::uint32_t>(ArrowArrayViewGetUIntUnsafe(&tokens, r));
            const auto bytes = ArrowArrayViewGetBytesUnsafe(&lists, r);
            std::vector<std::uint64_t> ids;
            ok = roaring::decode_treemap(bytes.data.as_uint8, static_cast<std::size_t>(bytes.size_bytes), ids, error);
            if (ok) {
                auto& to = out[token];
                to.insert(to.end(), ids.begin(), ids.end());
            }
        }
        ArrowArrayViewReset(&tokens);
        ArrowArrayViewReset(&lists);
        if (!ok) {
            return false;
        }
    }
    return true;
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
    std::shared_ptr<const FragReuse> frag_reuse;
    if (!FragReuse::load(dataset_path, manifest, frag_reuse, error)) {
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
        // The fragment-reuse and MemWAL indexes are listed as Lance lists them; other system indexes are not.
        if (is_system_index(index) && index.name != "__lance_frag_reuse" && index.name != "__lance_mem_wal") {
            continue;
        }
        IndexInfo info;
        info.name = index.name;
        info.uuid = pb::uuid_string(index.uuid);
        if (!index_base_id(index, info.base_id, error)) {
            return false;
        }
        std::filesystem::path index_dir;
        if (!index_dir_of(dataset_path, manifest, index, index_dir, error)) {
            return false;
        }
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
                vector_index_type(index_dir, index.details_value);
            if (!type.empty()) {
                info.type = type;
            }
        }
        for (const auto id : index.fields) {
            info.fields.push_back(path_of(id));
        }
        info.fragment_ids = index.fragment_ids;
        if (frag_reuse != nullptr) {
            frag_reuse->remap_coverage(info.fragment_ids);  // coverage as Lance reads it
        }
        info.dataset_version = index.dataset_version;
        info.index_version = index.index_version;
        info.type_url = index.details_type_url;
        info.details_value = index.details_value;
        info.field_ids = index.fields;
        info.created_at = index.created_at;
        for (const auto& f : index.files) {
            info.size_bytes += f.size;
        }
        for (const auto& f : manifest.fragments) {
            if (std::binary_search(info.fragment_ids.begin(), info.fragment_ids.end(),
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

bool dataset_count_from_index(const std::filesystem::path& dataset_path, bool has_version, std::uint64_t version,
                              const std::string& filter, IndexCount& out, std::string& error) {
    error.clear();
    out = IndexCount{};
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
    out.lines = candidates.used;
    out.exact = candidates.exact;
    for (const auto& f : manifest.fragments) {
        const auto it = candidates.rows.find(f.id);
        if (!out.exact || it == candidates.rows.end()) {
            out.uncovered.push_back(f.id);
            continue;
        }
        // The index lists deleted rows too: take out those the fragment's deletion vector holds. Both
        // lists are ascending physical offsets.
        std::vector<std::uint32_t> deleted;
        if (!read_manifest_deletion_vector(dataset_path, manifest, f.id, f.deletion_file, deleted, error)) {
            return false;
        }
        std::size_t hits = 0;
        std::size_t next = 0;
        for (const auto row : it->second) {
            while (next < deleted.size() && deleted[next] < row) {
                ++next;
            }
            if (next < deleted.size() && deleted[next] == row) {
                ++hits;
            }
        }
        out.count += static_cast<std::uint64_t>(it->second.size() - hits);
    }
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
