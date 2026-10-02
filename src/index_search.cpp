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
//
// Each index covers the fragments of its fragment bitmap: an answer says nothing about the others.
// AND intersects answers where both apply; OR unites them where both apply; NOT and anything
// else answer nothing.

#include "nanolance/index_search.hpp"

#include "nanolance/data_file_reader.hpp"
#include "nanolance/index_maintenance.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/roaring_bitmap.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <unordered_map>

namespace nano_lance {
namespace {

// ── loaded index files ──────────────────────────────────────────────────────────────────────────

/// A read of an index file: its schema and batches, owned.
struct FileTable {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    FileTable() = default;
    FileTable(const FileTable&) = delete;
    FileTable& operator=(const FileTable&) = delete;
    ~FileTable() {
        for (auto& b : batches) {
            if (b.release != nullptr) {
                b.release(&b);
            }
        }
        if (schema.release != nullptr) {
            schema.release(&schema);
        }
    }
    const ArrowSchema& type(std::size_t column) const { return *schema.children[column]; }
};

bool read_table(const std::filesystem::path& path, const std::vector<std::string>* columns, const LanceRowRange& range,
                FileTable& out, std::string& error) {
    LanceScanRequest request;
    request.columns = columns;
    request.range = range;
    if (!lance_file_read(path, request, out.schema, out.batches, error)) {
        out.schema = ArrowSchema{};  // released by the reader on failure
        error = path.filename().string() + ": " + error;
        return false;
    }
    return true;
}

bool take_table(const std::filesystem::path& path, const std::vector<std::string>* columns,
                const std::vector<std::uint64_t>& rows, FileTable& out, std::string& error) {
    LanceScanRequest request;
    request.columns = columns;
    if (!lance_file_take(path, request, rows, out.schema, out.batches, error)) {
        out.schema = ArrowSchema{};
        error = path.filename().string() + ": " + error;
        return false;
    }
    return true;
}

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

/// Loaded indices, by file identity: an index's files never change, but a path could be reused by
/// a dataset written anew, so size and modification time are part of the key.
template <typename T>
class IndexCache {
public:
    std::shared_ptr<const T> find(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : it->second;
    }
    void put(const std::string& key, std::shared_ptr<const T> value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.size() >= 64U) {
            entries_.erase(entries_.begin());
        }
        entries_[key] = std::move(value);
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const T>> entries_;
};

std::string file_key(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    const auto mtime = std::filesystem::last_write_time(path, ec).time_since_epoch().count();
    return path.string() + "|" + std::to_string(size) + "|" + std::to_string(mtime);
}

bool schema_metadata(const std::filesystem::path& path, const std::string& key, std::string& value, bool& found,
                     LanceDataFileFooterLayout& layout, std::string& error) {
    pb::FileDescriptor descriptor;
    if (!read_lance_data_file_footer_and_descriptor(path, descriptor, layout, error)) {
        return false;
    }
    const auto it = descriptor.schema_metadata.find(key);
    found = it != descriptor.schema_metadata.end();
    if (found) {
        value.assign(it->second.begin(), it->second.end());
    }
    return true;
}

bool load_btree(const std::filesystem::path& dir, std::shared_ptr<const BTreeIndex>& out, std::string& error) {
    static IndexCache<BTreeIndex> cache;
    const auto lookup_path = dir / "page_lookup.lance";
    const auto key = file_key(lookup_path);
    if ((out = cache.find(key)) != nullptr) {
        return true;
    }
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

bool load_bitmap(const std::filesystem::path& dir, std::shared_ptr<const BitmapIndex>& out, std::string& error) {
    static IndexCache<BitmapIndex> cache;
    const auto path = dir / "bitmap_page_lookup.lance";
    const auto key = file_key(path);
    if ((out = cache.find(key)) != nullptr) {
        return true;
    }
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

// ── row sets ────────────────────────────────────────────────────────────────────────────────────

/// An answer: for each fragment in `covered`, the rows (ascending) that may pass; none for a covered
/// fragment `rows` does not list.
struct RowSet {
    std::set<std::uint32_t> covered;
    std::map<std::uint32_t, std::vector<std::uint32_t>> rows;
};

void add_addresses(RowSet& set, std::vector<std::uint64_t>& addrs) {
    std::sort(addrs.begin(), addrs.end());
    addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
    for (const auto a : addrs) {
        set.rows[static_cast<std::uint32_t>(a >> 32U)].push_back(static_cast<std::uint32_t>(a));
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

bool search_btree(const std::filesystem::path& dir, const expr::Predicate& p, RowSet& out, bool& answered,
                  std::string& error) {
    answered = false;
    if (p.test() == Test::HasAny || p.test() == Test::HasAll || p.test() == Test::Has) {
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
    std::vector<std::uint8_t> min_null;
    std::vector<std::uint8_t> min_nan;
    std::vector<std::uint8_t> max_null;
    std::vector<std::uint8_t> max_nan;
    nulls_of(lookup, 0, min_null, min_nan);
    nulls_of(lookup, 1, max_null, max_nan);

    // A page may hold a match where its bounds allow one. Its min may be a null (the page starts
    // with nulls) or a NaN (sorted first when negative): no lower bound then.
    std::vector<std::uint8_t> keep(pages, 0);
    std::vector<std::uint8_t> pass;
    const auto bound = [&](const expr::Predicate& q, std::size_t column, std::vector<std::uint8_t>& ok) {
        if (!evaluate(q, lookup, column, ok, error)) {
            return false;
        }
        for (std::size_t i = 0; i < pages; ++i) {
            if (column == 0 && (min_null[i] != 0 || min_nan[i] != 0)) {
                ok[i] = 1;
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
            if (p.constants() != 2U || !value_range(0, ">=", 1, "<=", keep)) {
                return p.constants() == 2U ? false : true;
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
            return true;
    }

    // The kept pages' values, the predicate on each: their row ids are the answer. Adjacent pages
    // are read as one range.
    std::vector<std::uint64_t> addrs;
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

bool search_bitmap(const std::filesystem::path& dir, bool label_list, const expr::Predicate& p,
                   const std::map<std::uint32_t, std::uint64_t>& physical, RowSet& out, bool& answered,
                   std::string& error) {
    answered = false;
    const bool array_test = p.test() == Test::HasAny || p.test() == Test::HasAll || p.test() == Test::Has;
    if (array_test != label_list && !(label_list && p.test() == Test::IsNull)) {
        return true;
    }
    if (p.test() == Test::HasAll && p.constants() == 0U) {
        return true;  // every non-null list has all of no elements
    }
    std::shared_ptr<const BitmapIndex> index;
    if (!load_bitmap(dir, index, error)) {
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

struct Search {
    const std::filesystem::path& dataset_path;
    const pb::Manifest& manifest;
    std::map<std::uint32_t, std::uint64_t> physical;  // fragment id: its rows
    std::string& error;
    std::vector<std::string> used;

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
            for (const auto* index : segments) {
                const auto& kind = index->details_type_url;
                const auto dir = dataset_path / "_indices" / pb::uuid_string(index->uuid);
                RowSet one;
                bool answered = false;
                bool ok = true;
                if (kind == "/lance.table.BTreeIndexDetails" && index->files.size() == 2U &&
                    std::any_of(index->files.begin(), index->files.end(),
                                [](const pb::IndexMetadata::File& f) { return f.path == "page_lookup.lance"; })) {
                    ok = search_btree(dir, p, one, answered, error);
                } else if ((kind == "/lance.table.BitmapIndexDetails" || kind == "/lance.table.LabelListIndexDetails") &&
                           !index->files.empty()) {
                    ok = search_bitmap(dir, kind == "/lance.table.LabelListIndexDetails", p, physical, one, answered,
                                       error);
                }
                if (!ok) {
                    return false;
                }
                if (!answered) {
                    answered_all = false;
                    break;
                }
                one.covered.insert(index->fragment_ids.begin(), index->fragment_ids.end());
                set.covered.insert(one.covered.begin(), one.covered.end());
                for (auto& [f, rows] : one.rows) {
                    if (one.covered.count(f) != 0U) {
                        auto& to = set.rows[f];
                        to = to.empty() ? std::move(rows) : unite(to, rows);
                    }
                }
            }
            if (answered_all) {
                const auto& url = segments.front()->details_type_url;
                std::string type = url.substr(url.rfind('.') + 1);
                type = type.substr(0, type.size() - std::string("IndexDetails").size());
                used.push_back("ScalarIndexQuery: query=[" + p.to_string() + "]@" + name + "(" + type + ")");
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
                    answer = a && b ? both(*a, *b) : (a ? std::move(a) : std::move(b));
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
    error.clear();
    // Index row ids are row addresses only without stable row ids; and an index whose rows a
    // deferred compaction moved needs the fragment-reuse index to be found again.
    if (manifest.indices.empty() || (manifest.reader_feature_flags & pb::kFlagStableRowIds) != 0U ||
        std::any_of(manifest.indices.begin(), manifest.indices.end(),
                    [](const pb::IndexMetadata& i) { return i.name == "__lance_frag_reuse"; })) {
        return true;
    }
    Search search{dataset_path, manifest, {}, error, {}};
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
