// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Answering a filter's predicates from Lance's scalar index files (index_search.hpp).
//
//   BTree      page_lookup.lance: per page of `batch_size` sorted values, its first value (min, null
//              when the page starts with nulls), last value (max) and null count. A predicate keeps
//              the pages whose bounds allow a match -- evaluated on the bounds with the filter's own
//              semantics -- and is then evaluated on those pages' values (page_data.lance), whose
//              row ids are the answer.
//   Bitmap     bitmap_page_lookup.lance: per distinct value, a serialized RowAddrTreeMap of its rows.
//              The predicate is evaluated on the values; the matching values' bitmaps are the answer.
//   LabelList  the same over a list column's elements, plus the rows whose list is null (global
//              buffer 1): array_has_any is the union of the named elements' rows, array_has_all
//              their intersection.
//   ZoneMap    zonemap.lance: per zone, its min, max and null count; the bounds keep the zones that
//              may match, and a kept zone's rows (its fragment's offsets over its span, deleted rows
//              among them) are the answer. Zones hold row addresses, not stable row ids.
//   BloomFilter bloomfilter.lance: per zone, its span, whether it holds a null and its bloom filter.
//              `=` and IN keep the zones whose filter may hold a constant (probed with its bytes in
//              the column's type); IS NULL keeps the zones with a null. Zones hold row addresses.
//   NGram      ngram_postings.lance: for contains(col, 'x'), the rows holding every trigram of the
//              needle (lower-cased, folded), read from the posting lists; a needle shorter than three
//              characters, or with no trigram, says nothing; a trigram the index lacks means no row.
//
// Each index covers the fragments of its fragment bitmap: an answer says nothing about the others.
// AND intersects answers where both apply; OR unites them where both apply; NOT and anything
// else answer nothing.

#include "nanolance/base_paths.hpp"
#include "nanolance/index_search.hpp"
#include "nanolance/work_stats.hpp"

#include "index/index_files.hpp"

#include "nanolance/data_file_reader.hpp"
#include "nanolance/fts_tokenizer.hpp"
#include "nanolance/frag_reuse.hpp"
#include "nanolance/index_maintenance.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/bloom_filter.hpp"
#include "nanolance/roaring_bitmap.hpp"

#include <bit>
#include "nanolance/schema_mapper.hpp"
#include "nanolance/overlays.hpp"
#include "nanolance/row_ids.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <iterator>
#include <set>
#include <system_error>
#include <unordered_map>

namespace nano_lance {
namespace {

// ── loaded index files ──────────────────────────────────────────────────────────────────────────

using index_files::FileTable;
using index_files::IndexCache;
using index_files::file_key;
using index_files::read_table;
using index_files::schema_metadata;
using index_files::take_table;

struct BTreeIndex {
    FileTable lookup;  // min, max, null_count, page_idx
    std::uint64_t batch_size = 4096;
    std::filesystem::path page_data;
};

struct BitmapIndex {
    FileTable keys;  // keys
    std::filesystem::path file;
    std::vector<std::uint8_t> list_nulls;  // LabelList: the null lists' serialized RowAddrTreeMap
    bool has_list_nulls = false;
};

bool load_btree(const std::filesystem::path& dir, std::shared_ptr<const BTreeIndex>& out, std::string& error) {
    static IndexCache<BTreeIndex> cache;
    const auto lookup_path = dir / "page_lookup.lance";
    const auto key = file_key(lookup_path);
    if ((out = cache.find(key)) != nullptr) {
        work_stats::count_index_cache(true);
        return true;
    }
    work_stats::count_index_cache(false);
    auto index = std::make_shared<BTreeIndex>();
    index->page_data = dir / "page_data.lance";
    std::string value;
    bool found = false;
    LanceDataFileFooterLayout layout{};
    if (!schema_metadata(lookup_path, "batch_size", value, found, layout, error)) {
        return false;
    }
    if (found) {
        index->batch_size = std::strtoull(value.c_str(), nullptr, 10);
    }
    if (index->batch_size == 0U) {
        error = "btree index: batch_size 0";
        return false;
    }
    if (!read_table(lookup_path, nullptr, LanceRowRange{}, index->lookup, error)) {
        return false;
    }
    const auto format = [](const ArrowSchema& schema, std::int64_t c) {
        return std::string(schema.children[c]->format == nullptr ? "" : schema.children[c]->format);
    };
    if (index->lookup.schema.n_children != 4 || format(index->lookup.schema, 2) != "I" ||
        format(index->lookup.schema, 3) != "I") {
        error = "btree index: page_lookup.lance is not min, max, null_count (uint32), page_idx (uint32)";
        return false;
    }
    out = index;
    cache.put(key, out);
    return true;
}

bool load_bitmap(const std::filesystem::path& dir, std::shared_ptr<const BitmapIndex>& out, bool& cached,
                 std::string& error) {
    static IndexCache<BitmapIndex> cache;
    const auto path = dir / "bitmap_page_lookup.lance";
    const auto key = file_key(path);
    if ((out = cache.find(key)) != nullptr) {
        cached = true;
        return true;
    }
    cached = false;
    auto index = std::make_shared<BitmapIndex>();
    index->file = path;
    std::string value;
    bool found = false;
    LanceDataFileFooterLayout layout{};
    if (!schema_metadata(path, "lance:label_list_nulls", value, found, layout, error)) {
        return false;
    }
    if (found) {
        const auto buffer = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        if (!read_lance_file_global_buffer(path, layout, buffer, index->list_nulls, error)) {
            return false;
        }
        index->has_list_nulls = true;
    }
    const std::vector<std::string> keys = {"keys"};
    if (!read_table(path, &keys, LanceRowRange{}, index->keys, error)) {
        return false;
    }
    if (index->keys.schema.n_children != 1) {
        error = "bitmap index: no keys column";
        return false;
    }
    out = index;
    cache.put(key, out);
    return true;
}

struct ZoneMapIndex {
    FileTable zones;  // min, max, null_count, nan_count, fragment_id, zone_start, zone_length
};

bool load_zonemap(const std::filesystem::path& dir, std::shared_ptr<const ZoneMapIndex>& out, std::string& error) {
    static IndexCache<ZoneMapIndex> cache;
    const auto path = dir / "zonemap.lance";
    const auto key = file_key(path);
    if ((out = cache.find(key)) != nullptr) {
        work_stats::count_index_cache(true);
        return true;
    }
    work_stats::count_index_cache(false);
    auto index = std::make_shared<ZoneMapIndex>();
    if (!read_table(path, nullptr, LanceRowRange{}, index->zones, error)) {
        return false;
    }
    const auto format = [&](std::int64_t c) {
        return std::string(index->zones.schema.children[c]->format == nullptr ? "" : index->zones.schema.children[c]->format);
    };
    if (index->zones.schema.n_children != 7 || format(2) != "I" || format(3) != "I" || format(4) != "L" ||
        format(5) != "L" || format(6) != "L") {
        error = "zonemap index: zonemap.lance is not min, max, null_count, nan_count (uint32), fragment_id, "
                "zone_start, zone_length (uint64)";
        return false;
    }
    out = index;
    cache.put(key, out);
    return true;
}

// ── row sets ────────────────────────────────────────────────────────────────────────────────────

/// An answer: for each fragment in `covered`, the rows (ascending) that may pass; none for a covered
/// fragment `rows` does not list.
struct RowSet {
    std::set<std::uint32_t> covered;
    std::map<std::uint32_t, std::vector<std::uint32_t>> rows;
    /// Every covered fragment's rows are exactly the rows that pass (a BTREE or BITMAP answer), so a
    /// count of them needs no read. Zonemaps and bloom filters only prune: their rows are a superset.
    bool exact = false;
};

/// `offsets` ascending, each once: a bitset over their range when they are dense enough (a range predicate's
/// rows, in no order), else a sort.
void sort_unique_offsets(std::vector<std::uint32_t>& offsets) {
    if (offsets.size() < 64U) {
        std::sort(offsets.begin(), offsets.end());
        offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
        return;
    }
    const auto [lo, hi] = std::minmax_element(offsets.begin(), offsets.end());
    const std::uint32_t base = *lo;
    const std::uint64_t span = static_cast<std::uint64_t>(*hi) - base + 1U;
    if (span / 64U > 4U * offsets.size()) {
        std::sort(offsets.begin(), offsets.end());
        offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
        return;
    }
    std::vector<std::uint64_t> bits(static_cast<std::size_t>((span + 63U) / 64U), 0U);
    for (const auto o : offsets) {
        const std::uint32_t d = o - base;
        bits[d / 64U] |= std::uint64_t{1} << (d % 64U);
    }
    offsets.clear();
    for (std::size_t w = 0; w < bits.size(); ++w) {
        for (std::uint64_t word = bits[w]; word != 0U; word &= word - 1U) {
            offsets.push_back(base + static_cast<std::uint32_t>(w * 64U) +
                              static_cast<std::uint32_t>(std::countr_zero(word)));
        }
    }
}

void add_addresses(RowSet& set, std::vector<std::uint64_t>& addrs) {
    // By fragment (a dataset has few), then each fragment's offsets in order.
    std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> by_fragment;
    std::size_t at = 0;
    for (const auto a : addrs) {
        const auto fragment = static_cast<std::uint32_t>(a >> 32U);
        if (at >= by_fragment.size() || by_fragment[at].first != fragment) {
            at = 0;
            while (at < by_fragment.size() && by_fragment[at].first != fragment) {
                ++at;
            }
            if (at == by_fragment.size()) {
                if (by_fragment.size() == 32U) {  // many fragments: one sort of the addresses instead
                    std::sort(addrs.begin(), addrs.end());
                    addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
                    for (const auto b : addrs) {
                        set.rows[static_cast<std::uint32_t>(b >> 32U)].push_back(static_cast<std::uint32_t>(b));
                    }
                    return;
                }
                by_fragment.emplace_back(fragment, std::vector<std::uint32_t>{});
            }
        }
        by_fragment[at].second.push_back(static_cast<std::uint32_t>(a));
    }
    for (auto& [fragment, offsets] : by_fragment) {
        sort_unique_offsets(offsets);
        auto& rows = set.rows[fragment];
        if (rows.empty()) {
            rows = std::move(offsets);
        } else {
            rows.insert(rows.end(), offsets.begin(), offsets.end());
        }
    }
}

std::vector<std::uint32_t> unite(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    std::vector<std::uint32_t> out;
    out.reserve(a.size() + b.size());
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

std::vector<std::uint32_t> intersect(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    std::vector<std::uint32_t> out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

RowSet both(const RowSet& a, const RowSet& b) {
    RowSet out;
    out.covered = a.covered;
    out.covered.insert(b.covered.begin(), b.covered.end());
    for (const auto f : out.covered) {
        const bool in_a = a.covered.count(f) != 0U;
        const bool in_b = b.covered.count(f) != 0U;
        const auto ra = a.rows.find(f);
        const auto rb = b.rows.find(f);
        static const std::vector<std::uint32_t> none;
        const auto& va = ra == a.rows.end() ? none : ra->second;
        const auto& vb = rb == b.rows.end() ? none : rb->second;
        auto rows = in_a && in_b ? intersect(va, vb) : (in_a ? va : vb);
        if (!rows.empty()) {
            out.rows[f] = std::move(rows);
        }
    }
    // A fragment only one side covers keeps that side's rows, a superset: exact only when both agree.
    out.exact = a.exact && b.exact && a.covered == b.covered;
    return out;
}

RowSet either(const RowSet& a, const RowSet& b) {
    RowSet out;
    for (const auto f : a.covered) {
        if (b.covered.count(f) == 0U) {
            continue;
        }
        out.covered.insert(f);
        const auto ra = a.rows.find(f);
        const auto rb = b.rows.find(f);
        static const std::vector<std::uint32_t> none;
        auto rows = unite(ra == a.rows.end() ? none : ra->second, rb == b.rows.end() ? none : rb->second);
        if (!rows.empty()) {
            out.rows[f] = std::move(rows);
        }
    }
    out.exact = a.exact && b.exact;
    return out;
}

/// A serialized RowAddrTreeMap; a fragment stored with no bitmap is all of its rows.
bool read_treemap(const std::uint8_t* data, std::size_t size, const std::map<std::uint32_t, std::uint64_t>& physical,
                  RowSet& into, std::string& error) {
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        std::memcpy(&v, data + at, 4);
        return v;
    };
    if (size < 4) {
        error = "row bitmap is truncated";
        return false;
    }
    const auto count = u32(0);
    std::size_t at = 4;
    std::vector<std::uint32_t> rows;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (size - at < 8) {
            error = "row bitmap is truncated";
            return false;
        }
        const auto frag = u32(at);
        const auto bytes = u32(at + 4);
        at += 8;
        if (size - at < bytes) {
            error = "row bitmap is truncated";
            return false;
        }
        rows.clear();
        if (bytes == 0U) {
            const auto it = physical.find(frag);
            for (std::uint64_t r = 0; it != physical.end() && r < it->second; ++r) {
                rows.push_back(static_cast<std::uint32_t>(r));
            }
        } else if (!roaring::decode(data + at, bytes, rows, error)) {
            return false;
        }
        at += bytes;
        auto& to = into.rows[frag];
        to = to.empty() ? rows : unite(to, rows);
    }
    return true;
}

// ── searching one index ─────────────────────────────────────────────────────────────────────────

using Test = expr::Predicate::Test;

/// pass[i] |= the predicate on row i of column `c` of each batch of `table`, the batches in order.
bool evaluate(const expr::Predicate& p, const FileTable& table, std::size_t c, std::vector<std::uint8_t>& pass,
              std::string& error) {
    pass.clear();
    std::vector<std::uint8_t> part;
    for (const auto& b : table.batches) {
        if (!p.filter(table.type(c), *b.children[c], part, error)) {
            return false;
        }
        pass.insert(pass.end(), part.begin(), part.end());
    }
    return true;
}

/// Rows of column `c` across `table`'s batches: whether each is null, and (floats) NaN.
void nulls_of(const FileTable& table, std::size_t c, std::vector<std::uint8_t>& is_null, std::vector<std::uint8_t>& is_nan) {
    is_null.clear();
    is_nan.clear();
    ArrowSchemaView sv;
    ArrowSchemaViewInit(&sv, &table.type(c), nullptr);
    const bool floating = sv.type == NANOARROW_TYPE_FLOAT || sv.type == NANOARROW_TYPE_DOUBLE ||
                          sv.type == NANOARROW_TYPE_HALF_FLOAT;
    for (const auto& b : table.batches) {
        ArrowArrayView view;
        if (ArrowArrayViewInitFromSchema(&view, &table.type(c), nullptr) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, b.children[c], nullptr) != NANOARROW_OK) {
            ArrowArrayViewReset(&view);
            for (std::int64_t r = 0; r < b.length; ++r) {
                is_null.push_back(1);  // unknown: treated as no bound
                is_nan.push_back(0);
            }
            continue;
        }
        for (std::int64_t r = 0; r < b.length; ++r) {
            const bool null = ArrowArrayViewIsNull(&view, r);
            is_null.push_back(null ? 1 : 0);
            is_nan.push_back(!null && floating && std::isnan(ArrowArrayViewGetDoubleUnsafe(&view, r)) ? 1 : 0);
        }
        ArrowArrayViewReset(&view);
    }
}

std::vector<std::uint32_t> u32_column(const FileTable& table, std::size_t c) {
    std::vector<std::uint32_t> out;
    for (const auto& b : table.batches) {
        const ArrowArray* a = b.children[c];
        const auto* v = static_cast<const std::uint32_t*>(a->buffers[1]) + a->offset;
        out.insert(out.end(), v, v + a->length);
    }
    return out;
}

std::vector<std::uint64_t> u64_column(const FileTable& table, std::size_t c) {
    std::vector<std::uint64_t> out;
    for (const auto& b : table.batches) {
        const ArrowArray* a = b.children[c];
        const auto* v = static_cast<const std::uint64_t*>(a->buffers[1]) + a->offset;
        out.insert(out.end(), v, v + a->length);
    }
    return out;
}

/// The bounded units (pages, zones) of `bounds` -- its min and max (columns 0 and 1) and each one's
/// null count -- that may hold a row the predicate passes. `handled` is false for a predicate these
/// bounds do not decide (nothing is kept then).
bool prune_by_bounds(const expr::Predicate& p, const FileTable& bounds, const std::vector<std::uint32_t>& null_counts,
                     std::vector<std::uint8_t>& keep, bool& handled, std::string& error) {
    handled = true;
    const std::size_t pages = null_counts.size();
    std::vector<std::uint8_t> min_null;
    std::vector<std::uint8_t> min_nan;
    std::vector<std::uint8_t> max_null;
    std::vector<std::uint8_t> max_nan;
    nulls_of(bounds, 0, min_null, min_nan);
    nulls_of(bounds, 1, max_null, max_nan);
    keep.assign(pages, 0);
    // A unit may hold a match where its bounds allow one. Its min may be a null (it starts with
    // nulls): no lower bound then. A NaN min (-NaN sorts first) needs no care -- filters order -NaN
    // below every value too -- but is still treated as unbounded (conservative), and so is a NaN max.
    std::vector<std::uint8_t> pass;
    const auto bound = [&](const expr::Predicate& q, std::size_t column, std::vector<std::uint8_t>& ok) {
        if (!evaluate(q, bounds, column, ok, error)) {
            return false;
        }
        for (std::size_t i = 0; i < pages; ++i) {
            if (column == 0 && (min_null[i] != 0 || min_nan[i] != 0)) {
                ok[i] = 1;
            }
            if (column == 1 && max_nan[i] != 0) {
                ok[i] = 1;  // a NaN maximum (NaN sorts last) says nothing about the others
            }
            if (max_null[i] != 0) {
                ok[i] = 0;  // all null: no value matches
            }
        }
        return true;
    };
    const auto value_range = [&](std::size_t k_lo, const char* lo_op, std::size_t k_hi, const char* hi_op,
                                 std::vector<std::uint8_t>& ok) {
        std::vector<std::uint8_t> a;
        std::vector<std::uint8_t> b;
        if ((lo_op != nullptr && !bound(p.compare_with(lo_op, k_lo), 1, a)) ||
            (hi_op != nullptr && !bound(p.compare_with(hi_op, k_hi), 0, b))) {
            return false;
        }
        ok.assign(pages, 1);
        for (std::size_t i = 0; i < pages; ++i) {
            ok[i] = static_cast<std::uint8_t>((lo_op == nullptr || a[i] != 0) && (hi_op == nullptr || b[i] != 0) &&
                                              max_null[i] == 0);
        }
        return true;
    };
    switch (p.test()) {
        case Test::IsNull:
            for (std::size_t i = 0; i < pages; ++i) {
                keep[i] = null_counts[i] > 0U ? 1 : 0;
            }
            break;
        case Test::IsNotNull:
            for (std::size_t i = 0; i < pages; ++i) {
                keep[i] = max_null[i] == 0 ? 1 : 0;
            }
            break;
        case Test::Compare: {
            const auto& op = p.op();
            bool ok = true;
            if (op == "=") {
                ok = value_range(0, ">=", 0, "<=", keep);  // max >= c and min <= c
            } else if (op == "<" || op == "<=") {
                ok = value_range(0, nullptr, 0, op.c_str(), keep);  // min < c
            } else if (op == ">" || op == ">=") {
                ok = value_range(0, op.c_str(), 0, nullptr, keep);  // max > c
            } else {
                ok = value_range(0, nullptr, 0, nullptr, keep);  // !=: any page with values
            }
            if (!ok) {
                return false;
            }
            break;
        }
        case Test::Between:
            if (p.constants() != 2U) {
                handled = false;
                return true;
            }
            if (!value_range(0, ">=", 1, "<=", keep)) {
                return false;
            }
            break;
        case Test::In:
            for (std::size_t k = 0; k < p.constants(); ++k) {
                std::vector<std::uint8_t> one;
                if (!value_range(k, ">=", k, "<=", one)) {
                    return false;
                }
                for (std::size_t i = 0; i < pages; ++i) {
                    keep[i] = static_cast<std::uint8_t>(keep[i] | one[i]);
                }
            }
            break;
        default:
            handled = false;
            return true;
    }
    return true;

}

bool search_btree(const std::filesystem::path& dir, const expr::Predicate& p, RowSet& out, bool& answered,
                  std::string& error) {
    answered = false;
    if (p.test() == Test::HasAny || p.test() == Test::HasAll || p.test() == Test::Has || p.test() == Test::Contains) {
        return true;
    }
    std::shared_ptr<const BTreeIndex> index;
    if (!load_btree(dir, index, error)) {
        return false;
    }
    const auto& lookup = index->lookup;
    const auto null_counts = u32_column(lookup, 2);
    const auto page_idx = u32_column(lookup, 3);
    const auto pages = page_idx.size();
    std::vector<std::uint8_t> keep;
    bool handled = false;
    if (!prune_by_bounds(p, lookup, null_counts, keep, handled, error)) {
        return false;
    }
    if (!handled) {
        return true;
    }

    // The kept pages' values, the predicate on each: their row ids are the answer. Adjacent pages
    // are read as one range.
    std::vector<std::uint64_t> addrs;
    std::vector<std::uint8_t> pass;
    for (std::size_t i = 0; i < pages;) {
        if (keep[i] == 0) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j + 1 < pages && keep[j + 1] != 0 && page_idx[j + 1] == page_idx[j] + 1U) {
            ++j;
        }
        LanceRowRange range;
        range.offset = static_cast<std::uint64_t>(page_idx[i]) * index->batch_size;
        range.length = static_cast<std::uint64_t>(page_idx[j] - page_idx[i] + 1U) * index->batch_size;
        FileTable values;
        if (!read_table(index->page_data, nullptr, range, values, error)) {
            return false;
        }
        if (values.schema.n_children != 2 || values.schema.children[1]->format == nullptr ||
            std::string(values.schema.children[1]->format) != "L") {
            error = "btree index: page_data.lance is not values, ids (uint64)";
            return false;
        }
        if (!evaluate(p, values, 0, pass, error)) {
            return false;
        }
        std::size_t r = 0;
        for (const auto& b : values.batches) {
            const ArrowArray* ids = b.children[1];
            const auto* v = static_cast<const std::uint64_t*>(ids->buffers[1]) + ids->offset;
            for (std::int64_t k = 0; k < ids->length; ++k, ++r) {
                if (pass[r] != 0) {
                    addrs.push_back(v[k]);
                }
            }
        }
        i = j + 1;
    }
    add_addresses(out, addrs);
    answered = true;
    return true;
}

/// A JSON index answers a JSON test of its own path and the accessor's type, as its BTree does over the
/// values at the path (the same pages, the same comparisons). Any other JSON test is not its to answer.
bool search_json(const std::filesystem::path& dir, const pb::IndexMetadata& index, const expr::Predicate& p,
                 RowSet& out, bool& answered, std::string& error) {
    answered = false;
    std::string path;
    if (!index_files::json_index_path(index.details_value, path)) {
        error = "json index: malformed JsonIndexDetails";
        return false;
    }
    if (!p.json().has_value() || path != p.json()->path || index.files.size() != 2U) {
        return true;
    }
    std::shared_ptr<const BTreeIndex> btree;
    if (!load_btree(dir, btree, error)) {
        return false;
    }
    if (!p.routes_to_json_index(btree->lookup.schema.children[0]->format)) {
        return true;
    }
    return search_btree(dir, p, out, answered, error);
}

/// A zonemap answers with the rows of its kept zones: each zone's span of offsets in its fragment.
bool search_zonemap(const std::filesystem::path& dir, const expr::Predicate& p, RowSet& out, bool& answered,
                    std::string& error) {
    answered = false;
    if (p.test() == Test::HasAny || p.test() == Test::HasAll || p.test() == Test::Has || p.test() == Test::Contains) {
        return true;
    }
    std::shared_ptr<const ZoneMapIndex> index;
    if (!load_zonemap(dir, index, error)) {
        return false;
    }
    const auto& zones = index->zones;
    // A nested column's zones have no bounds (min and max are null): only their null counts answer. A
    // fixed-size binary's are null too (pylance writes them so), and every zone may match a predicate.
    const char* min_format = zones.schema.children[0]->format;
    const bool nested = min_format != nullptr && min_format[0] == '+';
    const bool fixed_binary = min_format != nullptr && std::string_view(min_format).substr(0, 2) == "w:";
    if (nested && p.test() != Test::IsNull) {
        return true;
    }
    const auto null_counts = u32_column(zones, 2);
    const auto fragments = u64_column(zones, 4);
    const auto starts = u64_column(zones, 5);
    const auto lengths = u64_column(zones, 6);
    std::vector<std::uint8_t> keep;
    bool handled = false;
    if (fixed_binary && p.test() != Test::IsNull) {
        keep.assign(null_counts.size(), 1);
        handled = true;
    } else if (!prune_by_bounds(p, zones, null_counts, keep, handled, error)) {
        return false;
    }
    if (!handled) {
        return true;
    }
    for (std::size_t z = 0; z < keep.size(); ++z) {
        if (keep[z] == 0) {
            continue;
        }
        auto& rows = out.rows[static_cast<std::uint32_t>(fragments[z])];
        for (std::uint64_t r = starts[z]; r < starts[z] + lengths[z]; ++r) {
            rows.push_back(static_cast<std::uint32_t>(r));
        }
    }
    answered = true;
    return true;
}

// ── bloom filters ───────────────────────────────────────────────────────────────────────────────

struct BloomFilterIndex {
    FileTable zones;  // fragment_id, zone_start, zone_length, has_null, bloom_filter_data
};

bool load_bloomfilter(const std::filesystem::path& dir, std::shared_ptr<const BloomFilterIndex>& out,
                      std::string& error) {
    static IndexCache<BloomFilterIndex> cache;
    const auto path = dir / "bloomfilter.lance";
    const auto key = file_key(path);
    if ((out = cache.find(key)) != nullptr) {
        work_stats::count_index_cache(true);
        return true;
    }
    work_stats::count_index_cache(false);
    auto index = std::make_shared<BloomFilterIndex>();
    if (!read_table(path, nullptr, LanceRowRange{}, index->zones, error)) {
        return false;
    }
    const auto format = [&](std::int64_t c) {
        return std::string(index->zones.schema.children[c]->format == nullptr ? "" : index->zones.schema.children[c]->format);
    };
    if (index->zones.schema.n_children != 5 || format(0) != "L" || format(1) != "L" || format(2) != "L" ||
        format(3) != "b" || format(4) != "z") {
        error = "bloomfilter index: bloomfilter.lance is not fragment_id, zone_start, zone_length (uint64), "
                "has_null (bool), bloom_filter_data (binary)";
        return false;
    }
    out = index;
    cache.put(key, out);
    return true;
}

/// The Arrow type of a manifest field, as its format (a leaf column's type; a bloom filter's values).
bool arrow_column_type(const pb::Field& field, ArrowSchema& out, std::string& error) {
    std::string format;
    if (!lance_arrow_format_for_logical_type(field.logical_type, format, error)) {
        return false;
    }
    ArrowSchemaInit(&out);
    if (ArrowSchemaSetFormat(&out, format.c_str()) != NANOARROW_OK) {
        error = "bloom filter index: cannot read the column's type";
        return false;
    }
    return true;
}

/// The zones that may hold a row the predicate passes. `=` and IN probe each zone's filter with the
/// constants' bytes (a constant is cast to the column's type first); a constant that does not convert
/// leaves the predicate to the scan. Zones of a float column: a zero or NaN constant may match values of
/// other bit patterns, and dates and times are not probed: every zone is kept then.
bool search_bloomfilter(const std::filesystem::path& dir, const ArrowSchema& column, const expr::Predicate& p,
                        RowSet& out, bool& answered, std::string& error) {
    answered = false;
    const bool equal = p.test() == Test::In || (p.test() == Test::Compare && p.op() == "=");
    if (p.test() != Test::IsNull && !equal) {
        return true;
    }
    std::shared_ptr<const BloomFilterIndex> index;
    if (!load_bloomfilter(dir, index, error)) {
        return false;
    }
    const auto& zones = index->zones;
    const auto fragments = u64_column(zones, 0);
    const auto starts = u64_column(zones, 1);
    const auto lengths = u64_column(zones, 2);
    std::vector<std::uint8_t> keep(fragments.size(), 0);
    if (p.test() == Test::IsNull) {
        std::size_t z = 0;
        for (const auto& b : zones.batches) {
            const ArrowArray* a = b.children[3];
            for (std::int64_t r = 0; r < a->length; ++r, ++z) {
                keep[z] = static_cast<std::uint8_t>(ArrowBitGet(static_cast<const std::uint8_t*>(a->buffers[1]), a->offset + r));
            }
        }
        answered = true;
    } else {
        ArrowSchemaView column_view;
        ArrowError aerr;
        if (ArrowSchemaViewInit(&column_view, &column, &aerr) != NANOARROW_OK) {
            error = std::string("bloom filter index: ") + aerr.message;
            return false;
        }
        const auto type = column_view.type;
        const bool floating = type == NANOARROW_TYPE_FLOAT || type == NANOARROW_TYPE_DOUBLE;
        const bool temporal = type == NANOARROW_TYPE_DATE32 || type == NANOARROW_TYPE_DATE64 ||
                              type == NANOARROW_TYPE_TIME32 || type == NANOARROW_TYPE_TIME64 ||
                              type == NANOARROW_TYPE_TIMESTAMP;
        std::size_t width = 0;
        if (!index_files::bloom_value_width(column_view, width, error)) {
            return false;
        }
        bool keep_all = temporal;
        std::vector<std::uint64_t> hashes;
        for (std::size_t k = 0; !keep_all && k < p.constants(); ++k) {
            ArrowArray value{};
            std::string convert_error;
            if (!p.constant_as(k, column, value, convert_error)) {
                return true;  // the scan evaluates the filter, and reports what it cannot convert
            }
            ArrowArrayView view;
            if (ArrowArrayViewInitFromSchema(&view, &column, &aerr) != NANOARROW_OK ||
                ArrowArrayViewSetArray(&view, &value, &aerr) != NANOARROW_OK) {
                ArrowArrayViewReset(&view);
                if (value.release != nullptr) {
                    value.release(&value);
                }
                error = std::string("bloom filter index: ") + aerr.message;
                return false;
            }
            if (!ArrowArrayViewIsNull(&view, 0)) {
                const std::uint8_t* bytes = view.buffer_views[1].data.as_uint8 + static_cast<std::size_t>(view.offset) * width;
                if (floating && width == 4) {
                    float f = 0.0F;
                    std::memcpy(&f, bytes, 4);
                    keep_all = keep_all || f == 0.0F || std::isnan(f);
                } else if (floating) {
                    double d = 0.0;
                    std::memcpy(&d, bytes, 8);
                    keep_all = keep_all || d == 0.0 || std::isnan(d);
                }
                hashes.push_back(index_files::bloom_hash(&view, 0, width));
            }
            ArrowArrayViewReset(&view);
            if (value.release != nullptr) {
                value.release(&value);
            }
        }
        if (keep_all) {
            keep.assign(keep.size(), 1);
        } else {
            std::size_t z = 0;
            for (const auto& b : zones.batches) {
                ArrowArrayView bv;
                if (ArrowArrayViewInitFromSchema(&bv, &zones.type(4), &aerr) != NANOARROW_OK ||
                    ArrowArrayViewSetArray(&bv, b.children[4], &aerr) != NANOARROW_OK) {
                    ArrowArrayViewReset(&bv);
                    error = std::string("bloom filter index: ") + aerr.message;
                    return false;
                }
                for (std::int64_t r = 0; r < b.length; ++r, ++z) {
                    const auto filter = ArrowArrayViewGetBytesUnsafe(&bv, r);
                    const auto size = static_cast<std::size_t>(filter.size_bytes);
                    if (size < 32U || (size & (size - 1U)) != 0U) {
                        ArrowArrayViewReset(&bv);
                        error = "bloom filter index: a zone's filter is not a power of two of at least 32 bytes";
                        return false;
                    }
                    const auto* data = reinterpret_cast<const std::uint8_t*>(filter.data.as_char);
                    for (const auto hash : hashes) {
                        if (bloom_filter::may_contain(data, size, hash)) {
                            keep[z] = 1;
                            break;
                        }
                    }
                }
                ArrowArrayViewReset(&bv);
            }
        }
        answered = true;
    }
    for (std::size_t z = 0; z < keep.size(); ++z) {
        if (keep[z] == 0) {
            continue;
        }
        auto& rows = out.rows[static_cast<std::uint32_t>(fragments[z])];
        for (std::uint64_t r = starts[z]; r < starts[z] + lengths[z]; ++r) {
            rows.push_back(static_cast<std::uint32_t>(r));
        }
    }
    return true;
}

/// The rows of the keys at `key_rows` (their bitmaps, united).
bool bitmap_rows(const BitmapIndex& index, const std::vector<std::uint64_t>& key_rows,
                 const std::map<std::uint32_t, std::uint64_t>& physical, RowSet& out, std::string& error) {
    if (key_rows.empty()) {
        return true;
    }
    const std::vector<std::string> bitmaps = {"bitmaps"};
    FileTable taken;
    if (!take_table(index.file, &bitmaps, key_rows, taken, error)) {
        return false;
    }
    const char* format = taken.schema.n_children == 1 ? taken.schema.children[0]->format : nullptr;
    if (format == nullptr || (std::string(format) != "z" && std::string(format) != "Z")) {
        error = "bitmap index: bitmaps is not a binary column";
        return false;
    }
    for (const auto& b : taken.batches) {
        ArrowArrayView view;
        ArrowError aerr;
        if (ArrowArrayViewInitFromSchema(&view, &taken.type(0), &aerr) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, b.children[0], &aerr) != NANOARROW_OK) {
            ArrowArrayViewReset(&view);
            error = std::string("bitmap index: ") + aerr.message;
            return false;
        }
        bool ok = true;
        for (std::int64_t r = 0; ok && r < b.length; ++r) {
            if (ArrowArrayViewIsNull(&view, r)) {
                continue;
            }
            const auto bytes = ArrowArrayViewGetBytesUnsafe(&view, r);
            ok = read_treemap(bytes.data.as_uint8, static_cast<std::size_t>(bytes.size_bytes), physical, out, error);
        }
        ArrowArrayViewReset(&view);
        if (!ok) {
            return false;
        }
    }
    return true;
}

// ── NGRAM ───────────────────────────────────────────────────────────────────────────────────────

/// ngram_postings.lance: `tokens` (the trigram ids, uint32) and `posting_list` (a RoaringTreemap of row
/// ids per token). Only the tokens are read up front; a query takes the posting lists it needs.
struct NGramIndex {
    FileTable tokens;
    std::filesystem::path file;
    std::unordered_map<std::uint32_t, std::uint64_t> rows;  // token -> its row in the file
};

bool load_ngram(const std::filesystem::path& dir, std::shared_ptr<const NGramIndex>& out, std::string& error) {
    static IndexCache<NGramIndex> cache;
    const auto path = dir / "ngram_postings.lance";
    const auto key = file_key(path);
    if ((out = cache.find(key)) != nullptr) {
        work_stats::count_index_cache(true);
        return true;
    }
    work_stats::count_index_cache(false);
    auto index = std::make_shared<NGramIndex>();
    index->file = path;
    const std::vector<std::string> columns = {"tokens"};
    if (!read_table(path, &columns, LanceRowRange{}, index->tokens, error)) {
        return false;
    }
    if (index->tokens.schema.n_children != 1 || index->tokens.schema.children[0]->format == nullptr ||
        std::string(index->tokens.schema.children[0]->format) != "I") {
        error = "ngram index: ngram_postings.lance has no uint32 tokens column";
        return false;
    }
    std::uint64_t row = 0;
    for (const auto& b : index->tokens.batches) {
        ArrowArrayView view;
        ArrowError aerr;
        if (ArrowArrayViewInitFromSchema(&view, &index->tokens.type(0), &aerr) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, b.children[0], &aerr) != NANOARROW_OK) {
            ArrowArrayViewReset(&view);
            error = std::string("ngram index: ") + aerr.message;
            return false;
        }
        for (std::int64_t r = 0; r < b.length; ++r, ++row) {
            if (!ArrowArrayViewIsNull(&view, r)) {
                index->rows.emplace(static_cast<std::uint32_t>(ArrowArrayViewGetUIntUnsafe(&view, r)), row);
            }
        }
        ArrowArrayViewReset(&view);
    }
    out = index;
    cache.put(key, out);
    return true;
}

/// contains(col, 'needle'): the rows holding every trigram of the needle, as Lance answers it (ngram.rs
/// StringContains). A needle of under three characters, or with no trigram, is not answered (every row
/// is read); a trigram the index lacks means no row holds the needle (an answer with none); otherwise the
/// intersection of the trigrams' posting lists is the candidates, which the scan still checks.
bool search_ngram(const std::filesystem::path& dir, const expr::Predicate& p, RowSet& out, bool& answered,
                  std::string& error) {
    answered = false;
    if (p.test() != Test::Contains || p.constants() != 1U) {
        return true;
    }
    ArrowSchema text_type;
    ArrowSchemaInit(&text_type);
    if (ArrowSchemaSetType(&text_type, NANOARROW_TYPE_STRING) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    ArrowArray needle{};
    const bool converted = p.constant_as(0, text_type, needle, error);
    text_type.release(&text_type);
    if (!converted) {
        return false;
    }
    ArrowArrayView view;
    ArrowError aerr;
    ArrowArrayViewInitFromType(&view, NANOARROW_TYPE_STRING);
    if (ArrowArrayViewSetArray(&view, &needle, &aerr) != NANOARROW_OK) {
        ArrowArrayViewReset(&view);
        if (needle.release != nullptr) {
            needle.release(&needle);
        }
        error = std::string("ngram index: ") + aerr.message;
        return false;
    }
    const bool is_null = ArrowArrayViewIsNull(&view, 0);
    std::string text;
    if (!is_null) {
        const auto sv = ArrowArrayViewGetStringUnsafe(&view, 0);
        text.assign(sv.data, static_cast<std::size_t>(sv.size_bytes));
    }
    ArrowArrayViewReset(&view);
    if (needle.release != nullptr) {
        needle.release(&needle);
    }
    if (is_null) {
        return true;
    }
    std::size_t characters = 0;  // UTF-8 code points, as Lance counts the needle
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) {
            ++characters;
        }
    }
    if (characters < 3U) {
        return true;
    }
    const auto tokens = fts::ngram_tokens(fts::lower_and_fold(text));
    if (tokens.empty()) {
        return true;
    }
    std::shared_ptr<const NGramIndex> index;
    if (!load_ngram(dir, index, error)) {
        return false;
    }
    std::vector<std::uint64_t> rows;  // the file rows of the needle's tokens
    rows.reserve(tokens.size());
    for (const auto token : tokens) {
        const auto found = index->rows.find(token);
        if (found == index->rows.end()) {
            answered = true;  // no row holds this trigram, so none holds the needle
            return true;
        }
        rows.push_back(found->second);
    }
    FileTable taken;
    const std::vector<std::string> postings = {"posting_list"};
    if (!take_table(index->file, &postings, rows, taken, error)) {
        return false;
    }
    bool first = true;
    std::vector<std::uint64_t> common;  // the row ids holding every trigram
    for (const auto& b : taken.batches) {
        ArrowArrayView list;
        ArrowError lerr;
        if (ArrowArrayViewInitFromSchema(&list, &taken.type(0), &lerr) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&list, b.children[0], &lerr) != NANOARROW_OK) {
            ArrowArrayViewReset(&list);
            error = std::string("ngram index: ") + lerr.message;
            return false;
        }
        bool ok = true;
        for (std::int64_t r = 0; ok && r < b.length; ++r) {
            if (ArrowArrayViewIsNull(&list, r)) {
                continue;
            }
            const auto bytes = ArrowArrayViewGetBytesUnsafe(&list, r);
            std::vector<std::uint64_t> ids;
            ok = roaring::decode_treemap(bytes.data.as_uint8, static_cast<std::size_t>(bytes.size_bytes), ids, error);
            if (ok && first) {
                common = std::move(ids);
            } else if (ok) {
                std::vector<std::uint64_t> merged;
                std::set_intersection(common.begin(), common.end(), ids.begin(), ids.end(), std::back_inserter(merged));
                common = std::move(merged);
            }
            first = false;
        }
        ArrowArrayViewReset(&list);
        if (!ok) {
            return false;
        }
    }
    for (const auto id : common) {
        out.rows[static_cast<std::uint32_t>(id >> 32U)].push_back(static_cast<std::uint32_t>(id));
    }
    answered = true;
    return true;
}

bool search_bitmap(const std::filesystem::path& dir, bool label_list, const expr::Predicate& p,
                   const std::map<std::uint32_t, std::uint64_t>& physical, RowSet& out, bool& answered,
                   std::string& error) {
    answered = false;
    if (p.test() == Test::Contains) {
        return true;
    }
    const bool array_test = p.test() == Test::HasAny || p.test() == Test::HasAll || p.test() == Test::Has;
    if (array_test != label_list && !(label_list && p.test() == Test::IsNull)) {
        return true;
    }
    if (p.test() == Test::HasAll && p.constants() == 0U) {
        return true;  // every non-null list has all of no elements
    }
    std::shared_ptr<const BitmapIndex> index;
    bool cached = false;
    if (!load_bitmap(dir, index, cached, error)) {
        return false;
    }
    if (label_list && p.test() == Test::IsNull) {
        if (!index->has_list_nulls) {
            return true;
        }
        if (!read_treemap(index->list_nulls.data(), index->list_nulls.size(), physical, out, error)) {
            return false;
        }
        answered = true;
        return true;
    }
    const auto matching = [&](const expr::Predicate& q, std::vector<std::uint64_t>& rows) {
        std::vector<std::uint8_t> pass;
        if (!evaluate(q, index->keys, 0, pass, error)) {
            return false;
        }
        rows.clear();
        for (std::size_t r = 0; r < pass.size(); ++r) {
            if (pass[r] != 0) {
                rows.push_back(r);
            }
        }
        // One lookup per value found in the index (a value not in it reads nothing): a hit when the
        // index was loaded before this search, else the load answered each of them.
        for (std::size_t i = 0; i < rows.size(); ++i) {
            work_stats::count_index_cache(cached);
        }
        return true;
    };
    std::vector<std::uint64_t> rows;
    if (p.test() == Test::HasAll) {
        // Each named element's rows, intersected.
        std::optional<RowSet> all;
        for (std::size_t k = 0; k < p.constants(); ++k) {
            RowSet one;
            if (!matching(p.compare_with("=", k), rows) || !bitmap_rows(*index, rows, physical, one, error)) {
                return false;
            }
            if (!all) {
                all = std::move(one);
                continue;
            }
            RowSet next;
            for (const auto& [f, r] : all->rows) {
                const auto it = one.rows.find(f);
                if (it != one.rows.end()) {
                    auto both_rows = intersect(r, it->second);
                    if (!both_rows.empty()) {
                        next.rows[f] = std::move(both_rows);
                    }
                }
            }
            all = std::move(next);
        }
        out.rows = std::move(all->rows);
    } else {
        if (!matching(p, rows) || !bitmap_rows(*index, rows, physical, out, error)) {
            return false;
        }
    }
    answered = true;
    return true;
}

// ── the filter ──────────────────────────────────────────────────────────────────────────────────

/// The rows of `set` as the fragment-reuse index says they are now: deleted rows drop out, moved rows
/// land in their new fragments. A fragment no version rewrote keeps its rows as they are.
void remap_rows(const FragReuse& fri, RowSet& set) {
    std::map<std::uint32_t, std::vector<std::uint32_t>> moved;
    for (auto& [fragment, offsets] : set.rows) {
        if (!fri.touches(fragment)) {
            auto& to = moved[fragment];
            to.insert(to.end(), offsets.begin(), offsets.end());
            continue;
        }
        for (const auto offset : offsets) {
            std::uint64_t to = 0;
            if (fri.resolve((static_cast<std::uint64_t>(fragment) << 32U) | offset, to)) {
                moved[static_cast<std::uint32_t>(to >> 32U)].push_back(static_cast<std::uint32_t>(to));
            }
        }
    }
    for (auto& [fragment, offsets] : moved) {
        std::sort(offsets.begin(), offsets.end());
        offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
    }
    set.rows = std::move(moved);
}

struct Search {
    const std::filesystem::path& dataset_path;
    const pb::Manifest& manifest;
    std::map<std::uint32_t, std::uint64_t> physical;  // fragment id: its rows
    std::string& error;
    std::vector<std::string> used;
    const RowIdToAddress* row_ids = nullptr;  // with stable row ids: what an index holds are ids
    const FragReuse* frag_reuse = nullptr;    // a deferred compaction moved rows: the indexes hold old addresses

    /// An answer read from an index of a dataset with stable row ids is in ids; make it addresses
    /// (a row whose id is gone, deleted, is dropped).
    void to_addresses(RowSet& set) const {
        if (row_ids == nullptr || !row_ids->stable()) {
            return;
        }
        std::map<std::uint32_t, std::vector<std::uint32_t>> moved;
        for (const auto& [high, offsets] : set.rows) {
            for (const auto offset : offsets) {
                std::uint64_t address = 0;
                if (row_ids->find((static_cast<std::uint64_t>(high) << 32U) | offset, address)) {
                    moved[static_cast<std::uint32_t>(address >> 32U)].push_back(static_cast<std::uint32_t>(address));
                }
            }
        }
        for (auto& [fragment, offsets] : moved) {
            std::sort(offsets.begin(), offsets.end());
        }
        set.rows = std::move(moved);
    }

    /// The field a column path names: exact, else case-insensitively unique (as the filter binds).
    const pb::Field* field_of(const std::vector<std::string>& path) const {
        std::string joined;
        for (const auto& p : path) {
            joined += (joined.empty() ? "" : ".") + p;
        }
        const auto lower = [](std::string s) {
            for (auto& c : s) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        };
        const auto find = [&](std::int32_t parent, const std::string& name) -> const pb::Field* {
            const pb::Field* found = nullptr;
            for (const auto& f : manifest.fields) {
                if (f.parent_id == parent && f.name == name) {
                    return &f;
                }
            }
            for (const auto& f : manifest.fields) {
                if (f.parent_id == parent && lower(f.name) == lower(name)) {
                    if (found != nullptr) {
                        return nullptr;
                    }
                    found = &f;
                }
            }
            return found;
        };
        if (path.size() > 1) {
            if (const auto* whole = find(-1, joined)) {
                return whole;
            }
        }
        const pb::Field* at = nullptr;
        std::int32_t parent = -1;
        for (const auto& p : path) {
            at = find(parent, p);
            if (at == nullptr) {
                return nullptr;
            }
            parent = at->id;
        }
        return at;
    }

    /// The answer of the field's indices to `p`, or nothing.
    bool predicate(const expr::Predicate& p, std::optional<RowSet>& answer) {
        answer.reset();
        const auto* field = field_of(p.column());
        if (field == nullptr) {
            return true;
        }
        // Each index (by name) of the field, all of its segments; the first one that answers.
        // In the manifest's order, as Lance picks among a column's indices: the oldest first.
        std::vector<std::pair<std::string, std::vector<const pb::IndexMetadata*>>> by_name;
        for (const auto& index : manifest.indices) {
            if (index.fields.size() == 1U && index.fields.front() == field->id && index.has_fragment_bitmap) {
                auto it = std::find_if(by_name.begin(), by_name.end(),
                                       [&](const auto& entry) { return entry.first == index.name; });
                if (it == by_name.end()) {
                    by_name.emplace_back(index.name, std::vector<const pb::IndexMetadata*>{});
                    it = by_name.end() - 1;
                }
                it->second.push_back(&index);
            }
        }
        for (const auto& [name, segments] : by_name) {
            RowSet set;
            bool answered_all = true;
            bool exact_all = true;
            for (const auto* index : segments) {
                const auto& kind = index->details_type_url;
                std::filesystem::path dir;
                if (!index_dir_of(dataset_path, manifest, *index, dir, error)) {
                    return false;
                }
                RowSet one;
                bool answered = false;
                bool ok = true;
                bool exact_segment = false;
                // A JSON index answers the JSON tests and they only it: a plain test of the column would
                // compare the document, not the values at the path.
                const bool json_index = kind == "/lance.index.pb.JsonIndexDetails";
                if (json_index != p.json().has_value()) {
                    answered_all = false;
                    break;
                }
                const bool bloom = kind == "/lance.index.pb.BloomFilterIndexDetails";
                const bool zoned = kind == "/lance.table.ZoneMapIndexDetails" || bloom;
                if (json_index) {
                    ok = search_json(dir, *index, p, one, answered, error);
                } else if (kind == "/lance.table.BTreeIndexDetails" && index->files.size() == 2U &&
                    std::any_of(index->files.begin(), index->files.end(),
                                [](const pb::IndexMetadata::File& f) { return f.path == "page_lookup.lance"; })) {
                    ok = search_btree(dir, p, one, answered, error);
                    exact_segment = true;
                } else if (bloom && !index->files.empty()) {
                    ArrowSchema column{};
                    ok = arrow_column_type(*field, column, error) &&
                         search_bloomfilter(dir, column, p, one, answered, error);
                    if (column.release != nullptr) {
                        column.release(&column);
                    }
                } else if (zoned && !index->files.empty()) {
                    ok = search_zonemap(dir, p, one, answered, error);
                } else if ((kind == "/lance.table.BitmapIndexDetails" || kind == "/lance.table.LabelListIndexDetails") &&
                           !index->files.empty()) {
                    ok = search_bitmap(dir, kind == "/lance.table.LabelListIndexDetails", p, physical, one, answered,
                                       error);
                    exact_segment = kind == "/lance.table.BitmapIndexDetails";
                } else if (kind == "/lance.table.NGramIndexDetails" && !index->files.empty()) {
                    ok = search_ngram(dir, p, one, answered, error);
                }
                if (!ok) {
                    return false;
                }
                if (!answered) {
                    answered_all = false;
                    break;
                }
                exact_all = exact_all && exact_segment;
                // The rows an index holds are addresses, as they were when it was built: a compaction
                // that deferred the index remap moved them since (stable row ids: the ids do not move).
                if (frag_reuse != nullptr && (row_ids == nullptr || !row_ids->stable())) {
                    remap_rows(*frag_reuse, one);
                }
                if (!zoned) {
                    to_addresses(one);  // a zonemap holds addresses already
                }
                auto coverage = index->fragment_ids;
                if (frag_reuse != nullptr) {
                    frag_reuse->remap_coverage(coverage);
                }
                one.covered.insert(coverage.begin(), coverage.end());
                for (const auto stale : fragments_stale_for_index(manifest, index->fields, index->dataset_version)) {
                    one.covered.erase(stale);  // overlaid since the index was built: scanned instead
                }
                set.covered.insert(one.covered.begin(), one.covered.end());
                for (auto& [f, rows] : one.rows) {
                    if (one.covered.count(f) != 0U) {
                        auto& to = set.rows[f];
                        to = to.empty() ? std::move(rows) : unite(to, rows);
                    }
                }
            }
            // An index that covers none of the live fragments (a compaction rewrote them all and Lance does not
            // remap a zone map) is not used: the plan is a plain scan, as Lance's.
            const bool covers_live = std::any_of(manifest.fragments.begin(), manifest.fragments.end(),
                                                 [&](const pb::DataFragment& f) { return set.covered.count(f.id) != 0U; });
            if (answered_all && covers_live) {
                const auto& url = segments.front()->details_type_url;
                std::string type = url.substr(url.rfind('.') + 1);
                type = type.substr(0, type.size() - std::string("IndexDetails").size());
                if (type == "Json") {
                    type = "BTree";  // the plan names the BTree a JSON index is built over
                }
                used.push_back("ScalarIndexQuery: query=[" + p.to_string() + "]@" + name + "(" + type + ")");
                set.exact = exact_all;
                answer = std::move(set);
                return true;
            }
        }
        return true;
    }

    bool condition(const expr::Condition& c, std::optional<RowSet>& answer) {
        answer.reset();
        switch (c.kind) {
            case expr::Condition::Kind::Predicate:
                return predicate(*c.predicate, answer);
            case expr::Condition::Kind::And:
            case expr::Condition::Kind::Or: {
                std::optional<RowSet> a;
                std::optional<RowSet> b;
                if (!condition(c.children[0], a) || !condition(c.children[1], b)) {
                    return false;
                }
                if (c.kind == expr::Condition::Kind::And) {
                    if (a && b) {
                        answer = both(*a, *b);
                    } else if (a || b) {
                        // The side that is not answered may match any row: what is left only prunes.
                        answer = a ? std::move(a) : std::move(b);
                        answer->exact = false;
                    }
                } else if (a && b) {
                    answer = either(*a, *b);
                }
                return true;
            }
            default:
                return true;
        }
    }
};

}  // namespace

bool index_candidates(const std::filesystem::path& dataset_path, const pb::Manifest& manifest,
                      const expr::Expression& filter, IndexCandidates& out, std::string& error) {
    out.rows.clear();
    out.used.clear();
    out.exact = false;
    error.clear();
    if (manifest.indices.empty()) {
        return true;
    }
    std::shared_ptr<const FragReuse> frag_reuse;
    if (!FragReuse::load(dataset_path, manifest, frag_reuse, error)) {
        return false;
    }
    RowIdToAddress resolver;
    if (!RowIdToAddress::build(dataset_path, manifest, resolver, error)) {
        return false;
    }
    Search search{dataset_path, manifest, {}, error, {}, &resolver, frag_reuse.get()};
    for (const auto& f : manifest.fragments) {
        search.physical[static_cast<std::uint32_t>(f.id)] = f.physical_rows;
    }
    std::optional<RowSet> answer;
    if (!search.condition(filter.conditions(), answer)) {
        return false;
    }
    if (!answer) {
        return true;
    }
    out.used = std::move(search.used);
    out.exact = answer->exact;
    for (const auto f : answer->covered) {
        if (search.physical.count(f) == 0U) {
            continue;  // a fragment the version no longer has
        }
        const auto it = answer->rows.find(f);
        auto& rows = out.rows[f];
        if (it != answer->rows.end()) {
            rows = std::move(it->second);
        }
    }
    return true;
}

}  // namespace nano_lance
