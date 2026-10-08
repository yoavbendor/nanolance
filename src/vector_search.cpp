// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Nearest-neighbour search (vector_search.hpp), following Lance 12 step for step (docs/VECTOR_INDEX.md):
//
//   index     index.idx (the IVF centroids, global buffer `lance:ivf`) and auxiliary.idx (the rows,
//             grouped by partition: `_rowid` and the vector or its PQ code; the partitions' offsets
//             and lengths in global buffer 1, the PQ codebook in `codebook_position`).
//   probing   the partitions nearest the query, as many as Lance's adaptive or legacy rule says, then
//             more while a filter or deletions leave fewer than k rows.
//   scoring   exact distances on IVF_FLAT's vectors; PQ distance tables (8 and 4 bits) on IVF_PQ's codes.
//   merging   the index's candidates -- re-scored exactly when asked to or when some fragments are not
//             indexed -- and an exact scan of the fragments the index does not cover; best k by
//             (distance, row id).

#include "nanolance/vector_search.hpp"

#include "index_build.hpp"
#include "index_files.hpp"

#include "nanolance/data_file_reader.hpp"
#include "nanolance/expr.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/parallel.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <unordered_map>

namespace nano_lance {

bool parse_vector_metric(const std::string& name, VectorMetric& out) {
    std::string n;
    for (const char c : name) {
        n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (n == "l2" || n == "euclidean") {
        out = VectorMetric::L2;
    } else if (n == "cosine") {
        out = VectorMetric::Cosine;
    } else if (n == "dot") {
        out = VectorMetric::Dot;
    } else {
        return false;
    }
    return true;
}

const char* vector_metric_name(VectorMetric metric) {
    switch (metric) {
        case VectorMetric::Cosine: return "cosine";
        case VectorMetric::Dot: return "dot";
        default: return "l2";
    }
}

namespace {

using index_files::FileTable;
using index_files::IndexCache;
using index_files::file_key;
using index_files::read_table;
using index_files::schema_metadata;

// ── protobuf ────────────────────────────────────────────────────────────────────────────────────

struct Field {
    std::uint32_t number = 0;
    std::uint32_t wire = 0;
    std::uint64_t value = 0;  // varint
    const std::uint8_t* data = nullptr;  // length-delimited / fixed
    std::size_t size = 0;
};

bool varint(const std::uint8_t*& p, const std::uint8_t* end, std::uint64_t& v) {
    v = 0;
    for (unsigned shift = 0; shift < 64U && p < end; shift += 7U) {
        const std::uint8_t b = *p++;
        v |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
        if ((b & 0x80U) == 0U) {
            return true;
        }
    }
    return false;
}

bool next_field(const std::uint8_t*& p, const std::uint8_t* end, Field& f) {
    std::uint64_t key = 0;
    if (!varint(p, end, key)) {
        return false;
    }
    f.number = static_cast<std::uint32_t>(key >> 3U);
    f.wire = static_cast<std::uint32_t>(key & 7U);
    f.data = nullptr;
    f.size = 0;
    switch (f.wire) {
        case 0: return varint(p, end, f.value);
        case 1:
        case 5: {
            const std::size_t n = f.wire == 1 ? 8U : 4U;
            if (static_cast<std::size_t>(end - p) < n) {
                return false;
            }
            f.data = p;
            f.size = n;
            p += n;
            return true;
        }
        case 2: {
            std::uint64_t n = 0;
            if (!varint(p, end, n) || n > static_cast<std::uint64_t>(end - p)) {
                return false;
            }
            f.data = p;
            f.size = static_cast<std::size_t>(n);
            p += n;
            return true;
        }
        default: return false;
    }
}

/// A repeated varint field, packed or not.
bool add_varints(const Field& f, std::vector<std::uint64_t>& out) {
    if (f.wire == 0) {
        out.push_back(f.value);
        return true;
    }
    if (f.wire != 2) {
        return false;
    }
    const auto* p = f.data;
    const auto* end = f.data + f.size;
    while (p < end) {
        std::uint64_t v = 0;
        if (!varint(p, end, v)) {
            return false;
        }
        out.push_back(v);
    }
    return true;
}

/// Tensor { data_type = 1, shape = 2, data = 3 }, its values as floats.
bool decode_tensor(const std::uint8_t* data, std::size_t size, std::vector<std::uint64_t>& shape,
                   std::vector<float>& values, std::string& error) {
    const auto* p = data;
    const auto* end = data + size;
    std::uint64_t type = 0;
    const std::uint8_t* bytes = nullptr;
    std::size_t nbytes = 0;
    Field f;
    while (p < end) {
        if (!next_field(p, end, f)) {
            error = "malformed Tensor";
            return false;
        }
        if (f.number == 1 && f.wire == 0) {
            type = f.value;
        } else if (f.number == 2 && !add_varints(f, shape)) {
            error = "malformed Tensor shape";
            return false;
        } else if (f.number == 3 && f.wire == 2) {
            bytes = f.data;
            nbytes = f.size;
        }
    }
    switch (type) {
        case 1: {  // FLOAT16
            values.resize(nbytes / 2U);
            for (std::size_t i = 0; i < values.size(); ++i) {
                std::uint16_t h = 0;
                std::memcpy(&h, bytes + 2U * i, 2U);
                values[i] = ArrowHalfFloatToFloat(h);
            }
            return true;
        }
        case 2:  // FLOAT32
            values.resize(nbytes / 4U);
            if (nbytes != 0U) {
                std::memcpy(values.data(), bytes, values.size() * 4U);
            }
            return true;
        case 3: {  // FLOAT64
            values.resize(nbytes / 8U);
            for (std::size_t i = 0; i < values.size(); ++i) {
                double d = 0;
                std::memcpy(&d, bytes + 8U * i, 8U);
                values[i] = static_cast<float>(d);
            }
            return true;
        }
        default:
            error = "a Tensor of data type " + std::to_string(type) + " is not supported";
            return false;
    }
}

/// IVF { centroids = 1 (deprecated, floats), offsets = 2, lengths = 3, centroids_tensor = 4 }.
struct IvfModel {
    std::vector<float> centroids;
    std::size_t partitions = 0;
    std::size_t dim = 0;
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> lengths;
};

bool decode_ivf(const std::vector<std::uint8_t>& message, IvfModel& out, std::string& error) {
    const auto* p = message.data();
    const auto* end = p + message.size();
    std::vector<float> legacy;
    Field f;
    while (p < end) {
        if (!next_field(p, end, f)) {
            error = "malformed IVF message";
            return false;
        }
        if (f.number == 1) {
            if (f.wire == 5) {
                float v = 0;
                std::memcpy(&v, f.data, 4U);
                legacy.push_back(v);
            } else if (f.wire == 2) {
                const std::size_t n = f.size / 4U;
                const std::size_t at = legacy.size();
                legacy.resize(at + n);
                std::memcpy(legacy.data() + at, f.data, n * 4U);
            }
        } else if (f.number == 2) {
            if (!add_varints(f, out.offsets)) {
                error = "malformed IVF offsets";
                return false;
            }
        } else if (f.number == 3) {
            if (!add_varints(f, out.lengths)) {
                error = "malformed IVF lengths";
                return false;
            }
        } else if (f.number == 4 && f.wire == 2) {
            std::vector<std::uint64_t> shape;
            if (!decode_tensor(f.data, f.size, shape, out.centroids, error)) {
                return false;
            }
            if (shape.size() == 2U) {
                out.partitions = static_cast<std::size_t>(shape[0]);
                out.dim = static_cast<std::size_t>(shape[1]);
            }
        }
    }
    if (out.centroids.empty() && !legacy.empty()) {
        out.centroids = std::move(legacy);
    }
    return true;
}

// ── small JSON lookups ──────────────────────────────────────────────────────────────────────────

/// `"key":` in `text` (with any escaping of the quotes removed first), and what follows it.
std::string json_value(std::string text, const std::string& key) {
    text.erase(std::remove(text.begin(), text.end(), '\\'), text.end());
    const auto at = text.find("\"" + key + "\":");
    if (at == std::string::npos) {
        return {};
    }
    auto i = at + key.size() + 3U;
    while (i < text.size() && text[i] == ' ') {
        ++i;
    }
    if (i < text.size() && text[i] == '"') {
        const auto close = text.find('"', i + 1U);
        return close == std::string::npos ? std::string{} : text.substr(i + 1U, close - i - 1U);
    }
    auto j = i;
    while (j < text.size() && text[j] != ',' && text[j] != '}' && text[j] != ']') {
        ++j;
    }
    return text.substr(i, j - i);
}

// ── Arrow access ────────────────────────────────────────────────────────────────────────────────

/// A batch's view, released with it.
struct BatchView {
    ArrowArrayView view{};
    bool ok = false;
    BatchView(const ArrowSchema& schema, const ArrowArray& batch, std::string& error) {
        ArrowError e{};
        if (ArrowArrayViewInitFromSchema(&view, &schema, &e) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &e) != NANOARROW_OK) {
            error = std::string("cannot read a batch: ") + e.message;
            return;
        }
        ok = true;
    }
    BatchView(const BatchView&) = delete;
    BatchView& operator=(const BatchView&) = delete;
    ~BatchView() { ArrowArrayViewReset(&view); }
};

/// Vectors of a fixed-size-list column: `dim` floats per row, null rows flagged.
struct VectorRows {
    std::size_t dim = 0;
    std::vector<float> values;
    std::vector<std::uint8_t> valid;
    std::vector<std::uint64_t> ids;
};

int child_index(const ArrowSchema& schema, const std::string& name) {
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children[i]->name != nullptr && name == schema.children[i]->name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// Append the rows of `batches`' column `vector_column` (and uint64 `id_column`) to `out`.
bool collect_vectors(const ArrowSchema& schema, const std::vector<ArrowArray>& batches, int vector_column,
                     int id_column, VectorRows& out, std::string& error) {
    for (const auto& batch : batches) {
        BatchView b(schema, batch, error);
        if (!b.ok) {
            return false;
        }
        const ArrowArrayView* list = b.view.children[vector_column];
        const ArrowArrayView* items = list->children[0];
        const ArrowArrayView* ids = id_column < 0 ? nullptr : b.view.children[id_column];
        const auto dim = static_cast<std::int64_t>(out.dim);
        const bool f32 = items->storage_type == NANOARROW_TYPE_FLOAT;
        for (std::int64_t r = 0; r < batch.length; ++r) {
            const std::int64_t row = b.view.offset + r;
            const bool null = ArrowArrayViewIsNull(list, row);
            out.valid.push_back(null ? 0 : 1);
            const std::int64_t first = (list->offset + row) * dim;
            const std::size_t at = out.values.size();
            out.values.resize(at + out.dim);
            if (!null) {
                if (f32) {
                    std::memcpy(out.values.data() + at, items->buffer_views[1].data.as_float + items->offset + first,
                                out.dim * sizeof(float));
                } else {
                    for (std::int64_t j = 0; j < dim; ++j) {
                        out.values[at + static_cast<std::size_t>(j)] =
                            static_cast<float>(ArrowArrayViewGetDoubleUnsafe(items, first + j));
                    }
                }
            }
            if (ids != nullptr) {
                out.ids.push_back(ArrowArrayViewGetUIntUnsafe(ids, row));
            }
        }
    }
    return true;
}

// ── distances ───────────────────────────────────────────────────────────────────────────────────

float dot(const float* a, const float* b, std::size_t n) {
    float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::size_t i = 0;
    for (; i + 8U <= n; i += 8U) {
        for (std::size_t j = 0; j < 8U; ++j) {
            s[j] += a[i + j] * b[i + j];
        }
    }
    float t = ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
    for (; i < n; ++i) {
        t += a[i] * b[i];
    }
    return t;
}

float l2(const float* a, const float* b, std::size_t n) {
    float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::size_t i = 0;
    for (; i + 8U <= n; i += 8U) {
        for (std::size_t j = 0; j < 8U; ++j) {
            const float d = a[i + j] - b[i + j];
            s[j] += d * d;
        }
    }
    float t = ((s[0] + s[1]) + (s[2] + s[3])) + ((s[4] + s[5]) + (s[6] + s[7]));
    for (; i < n; ++i) {
        const float d = a[i] - b[i];
        t += d * d;
    }
    return t;
}

/// Lance's distance: squared L2, 1 - cos, or 1 - dot. `q_norm` is |q| (cosine only).
float distance(VectorMetric metric, const float* x, const float* q, std::size_t n, float q_norm) {
    switch (metric) {
        case VectorMetric::Cosine: {
            const float x_norm = std::sqrt(dot(x, x, n));
            return 1.0F - dot(x, q, n) / (x_norm * q_norm);
        }
        case VectorMetric::Dot: return 1.0F - dot(x, q, n);
        default: return l2(x, q, n);
    }
}

struct Candidate {
    float d = 0;
    std::uint64_t id = 0;
};

/// Lance's order: distance ascending (NaN last), then row id.
bool better(const Candidate& a, const Candidate& b) {
    const bool an = std::isnan(a.d);
    const bool bn = std::isnan(b.d);
    if (an != bn) {
        return bn;
    }
    if (!an && a.d != b.d) {
        return a.d < b.d;
    }
    return a.id < b.id;
}

void keep_best(std::vector<Candidate>& c, std::size_t n) {
    if (c.size() > n) {
        std::nth_element(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(n), c.end(), better);
        c.resize(n);
    }
    std::sort(c.begin(), c.end(), better);
}

bool in_range(float d, const NearestQuery& q) {
    return !(q.lower_bound && d < *q.lower_bound) && !(q.upper_bound && d >= *q.upper_bound);
}

// ── the index ───────────────────────────────────────────────────────────────────────────────────

struct Partition {
    std::vector<std::uint64_t> ids;
    std::vector<float> vectors;       // IVF_FLAT: n * dim
    std::vector<std::uint8_t> codes;  // IVF_PQ: [code bytes][n]
};

struct IvfIndex {
    std::filesystem::path aux;
    std::string type;  // "IVF_FLAT" / "IVF_PQ"
    VectorMetric metric = VectorMetric::L2;
    bool pq = false;
    std::size_t dim = 0;
    std::size_t partitions = 0;
    std::vector<float> centroids;  // partitions * dim
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> lengths;
    std::uint32_t nbits = 8;
    std::size_t m = 0;            // sub-vectors
    std::size_t code_bytes = 0;   // m, or m / 2 with 4 bits
    bool transposed = true;
    std::vector<float> codebook;    // [m][2^nbits][dim / m]
    std::vector<float> codebook_t;  // the same as [m][dim / m][2^nbits]: a table row is one sweep

    mutable std::mutex mutex;
    mutable std::unordered_map<std::size_t, std::shared_ptr<const Partition>> loaded;

    bool partition(std::size_t p, std::shared_ptr<const Partition>& out, std::string& error) const;
};

bool IvfIndex::partition(std::size_t p, std::shared_ptr<const Partition>& out, std::string& error) const {
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = loaded.find(p);
        if (it != loaded.end()) {
            out = it->second;
            return true;
        }
    }
    auto part = std::make_shared<Partition>();
    const std::uint64_t n = lengths[p];
    if (n != 0U) {
        const std::vector<std::string> columns = {"_rowid", pq ? "__pq_code" : "flat"};
        FileTable table;
        if (!read_table(aux, &columns, LanceRowRange{offsets[p], n}, table, error)) {
            return false;
        }
        const int id_col = child_index(table.schema, "_rowid");
        const int value_col = child_index(table.schema, columns[1]);
        if (id_col < 0 || value_col < 0) {
            error = "vector index: auxiliary.idx has no _rowid / " + columns[1] + " column";
            return false;
        }
        if (!pq) {
            VectorRows rows;
            rows.dim = dim;
            if (!collect_vectors(table.schema, table.batches, value_col, id_col, rows, error)) {
                return false;
            }
            part->ids = std::move(rows.ids);
            part->vectors = std::move(rows.values);
        } else {
            std::vector<std::uint8_t> flat;  // as stored: n * code_bytes
            for (const auto& batch : table.batches) {
                BatchView b(table.schema, batch, error);
                if (!b.ok) {
                    return false;
                }
                const ArrowArrayView* list = b.view.children[value_col];
                const ArrowArrayView* bytes = list->children[0];
                const ArrowArrayView* ids = b.view.children[id_col];
                for (std::int64_t r = 0; r < batch.length; ++r) {
                    const std::int64_t row = b.view.offset + r;
                    part->ids.push_back(ArrowArrayViewGetUIntUnsafe(ids, row));
                    const std::int64_t first = (list->offset + row) * static_cast<std::int64_t>(code_bytes);
                    const auto* src = bytes->buffer_views[1].data.as_uint8 + bytes->offset + first;
                    flat.insert(flat.end(), src, src + code_bytes);
                }
            }
            if (part->ids.size() != n || flat.size() != n * code_bytes) {
                error = "vector index: partition " + std::to_string(p) + " has " + std::to_string(part->ids.size()) +
                        " rows, not " + std::to_string(n);
                return false;
            }
            if (transposed) {
                part->codes = std::move(flat);
            } else {
                part->codes.resize(flat.size());
                for (std::size_t r = 0; r < n; ++r) {
                    for (std::size_t s = 0; s < code_bytes; ++s) {
                        part->codes[s * n + r] = flat[r * code_bytes + s];
                    }
                }
            }
        }
        if (part->ids.size() != n) {
            error = "vector index: partition " + std::to_string(p) + " is short";
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    out = loaded.emplace(p, std::move(part)).first->second;
    return true;
}

bool load_ivf(const std::filesystem::path& dir, std::shared_ptr<const IvfIndex>& out, std::string& error) {
    static IndexCache<IvfIndex> cache;
    const auto index_path = dir / "index.idx";
    const auto aux_path = dir / "auxiliary.idx";
    const auto key = file_key(index_path) + "|" + file_key(aux_path);
    if ((out = cache.find(key)) != nullptr) {
        return true;
    }
    auto index = std::make_shared<IvfIndex>();
    index->aux = aux_path;

    std::string value;
    bool found = false;
    LanceDataFileFooterLayout layout{};
    if (!schema_metadata(index_path, "lance:index", value, found, layout, error)) {
        return false;
    }
    index->type = found ? json_value(value, "type") : std::string{};
    if (index->type != "IVF_FLAT" && index->type != "IVF_PQ") {
        error = "vector index type '" + index->type + "' is not supported";
        return false;
    }
    index->pq = index->type == "IVF_PQ";
    if (!parse_vector_metric(json_value(value, "distance_type"), index->metric)) {
        error = "vector index: unknown distance type '" + json_value(value, "distance_type") + "'";
        return false;
    }
    if (!schema_metadata(index_path, "lance:ivf", value, found, layout, error)) {
        return false;
    }
    std::vector<std::uint8_t> buffer;
    IvfModel model;
    if (!found || !read_lance_file_global_buffer(index_path, layout,
                                                 static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10)),
                                                 buffer, error) ||
        !decode_ivf(buffer, model, error)) {
        if (error.empty()) {
            error = "vector index: index.idx has no IVF model";
        }
        return false;
    }
    index->centroids = std::move(model.centroids);
    index->partitions = model.partitions;
    index->dim = model.dim;

    LanceDataFileFooterLayout aux_layout{};
    std::string storage;
    if (!schema_metadata(aux_path, "storage_metadata", storage, found, aux_layout, error)) {
        return false;
    }
    if (!schema_metadata(aux_path, "lance:ivf", value, found, aux_layout, error)) {
        return false;
    }
    IvfModel parts;
    if (!found || !read_lance_file_global_buffer(aux_path, aux_layout,
                                                 static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10)),
                                                 buffer, error) ||
        !decode_ivf(buffer, parts, error)) {
        if (error.empty()) {
            error = "vector index: auxiliary.idx has no partitions";
        }
        return false;
    }
    index->offsets = std::move(parts.offsets);
    index->lengths = std::move(parts.lengths);
    if (index->partitions == 0U || index->dim == 0U || index->centroids.size() != index->partitions * index->dim ||
        index->offsets.size() != index->partitions || index->lengths.size() != index->partitions) {
        error = "vector index: " + std::to_string(index->partitions) + " partitions of dimension " +
                std::to_string(index->dim) + " do not match its centroids and partition table";
        return false;
    }
    if (index->pq) {
        index->nbits = static_cast<std::uint32_t>(std::strtoul(json_value(storage, "nbits").c_str(), nullptr, 10));
        index->m = static_cast<std::size_t>(std::strtoul(json_value(storage, "num_sub_vectors").c_str(), nullptr, 10));
        index->transposed = json_value(storage, "transposed") != "false";
        const auto position =
            static_cast<std::uint32_t>(std::strtoul(json_value(storage, "codebook_position").c_str(), nullptr, 10));
        if ((index->nbits != 8U && index->nbits != 4U) || index->m == 0U || index->dim % index->m != 0U ||
            (index->nbits == 4U && index->m % 2U != 0U)) {
            error = "vector index: PQ with " + std::to_string(index->nbits) + " bits and " + std::to_string(index->m) +
                    " sub-vectors of a " + std::to_string(index->dim) + "-dimensional vector is not supported";
            return false;
        }
        index->code_bytes = index->nbits == 4U ? index->m / 2U : index->m;
        std::vector<std::uint64_t> shape;
        if (position == 0U || !read_lance_file_global_buffer(aux_path, aux_layout, position, buffer, error) ||
            !decode_tensor(buffer.data(), buffer.size(), shape, index->codebook, error)) {
            if (error.empty()) {
                error = "vector index: no PQ codebook";
            }
            return false;
        }
        if (index->codebook.size() != (std::size_t{1} << index->nbits) * index->dim) {
            error = "vector index: the PQ codebook has " + std::to_string(index->codebook.size()) + " values";
            return false;
        }
        const std::size_t nc = std::size_t{1} << index->nbits;
        const std::size_t w = index->dim / index->m;
        index->codebook_t.resize(index->codebook.size());
        for (std::size_t sub = 0; sub < index->m; ++sub) {
            for (std::size_t c = 0; c < nc; ++c) {
                for (std::size_t j = 0; j < w; ++j) {
                    index->codebook_t[(sub * w + j) * nc + c] = index->codebook[(sub * nc + c) * w + j];
                }
            }
        }
    }
    out = index;
    cache.put(key, out);
    return true;
}

// ── which partitions ────────────────────────────────────────────────────────────────────────────

/// How many partitions the first pass searches (Lance's AutoProbePolicy), and at most.
void probe_counts(const IvfIndex& index, const NearestQuery& q, bool f32_column, const std::vector<float>& sorted,
                  std::size_t& first, std::size_t& most) {
    const std::size_t P = sorted.size();
    most = q.maximum_nprobes ? std::min<std::size_t>(*q.maximum_nprobes, P) : P;
    std::size_t minimum = q.minimum_nprobes;
    if (q.maximum_nprobes && *q.maximum_nprobes == q.minimum_nprobes) {
        first = std::min(minimum, P);
        return;
    }
    const float nearest = sorted.empty() ? 0.0F : sorted.front();
    const bool finite_key = std::all_of(q.key.begin(), q.key.end(), [](float x) { return std::isfinite(x); });
    const bool adaptive = !q.maximum_nprobes && f32_column && finite_key && !index.pq &&
                          (index.metric == VectorMetric::L2 || index.metric == VectorMetric::Cosine) && q.k <= 100U &&
                          !(q.refine_factor && *q.refine_factor > 1U);
    const int bucket = q.k <= 1U ? 0 : (q.k <= 10U ? 1 : 2);
    if (adaptive) {
        static const float margins[2][3] = {{0.2175F, 0.265F, 0.33F}, {0.235F, 0.2875F, 0.38F}};
        static const std::size_t floors[2][3] = {{5, 6, 11}, {3, 8, 7}};
        static const std::size_t caps[2][3] = {{19, 24, 38}, {50, 77, 106}};
        const int m = index.metric == VectorMetric::Cosine ? 1 : 0;
        std::size_t selected = P;
        if (std::isfinite(nearest)) {
            const double d0 = nearest;
            const double gap = static_cast<double>(margins[m][bucket]) * d0;
            selected = static_cast<std::size_t>(std::partition_point(sorted.begin(), sorted.end(), [&](float d) {
                                                    return std::isfinite(d) && static_cast<double>(d) - d0 <= gap;
                                                }) -
                                                sorted.begin());
        } else if (sorted.empty()) {
            selected = 0;
        }
        selected = std::min(std::max(selected, floors[m][bucket]), caps[m][bucket]);
        first = std::min({std::max(minimum, selected), most, P});
        return;
    }
    static const float factors[3] = {0.6F, 7.0F, 81.0F};
    std::size_t selected = 0;
    if (!sorted.empty()) {
        const float threshold = nearest * factors[bucket];
        selected = static_cast<std::size_t>(
            std::partition_point(sorted.begin(), sorted.end(), [&](float d) { return d <= threshold; }) -
            sorted.begin());
    }
    first = std::max(minimum, selected);
    if (q.maximum_nprobes) {
        first = std::min<std::size_t>(first, *q.maximum_nprobes);
    }
    first = std::min(first, P);
}

// ── rows that may be returned ───────────────────────────────────────────────────────────────────

/// The rows a search may return: the version's live rows -- of the fragments `limit` names, when
/// set -- passing the prefilter, when there is one.
struct RowMask {
    std::map<std::uint32_t, std::uint64_t> physical;            // the version's fragments
    std::map<std::uint32_t, std::vector<std::uint8_t>> rows;    // where not every row is allowed
    std::set<std::uint32_t> limit;                              // empty: any fragment
    bool has_count = false;  // an explicit prefilter: how many rows it allows
    std::uint64_t count = 0;

    /// Index the above by fragment id; call once they are set.
    void seal() {
        std::uint32_t top = 0;
        for (const auto& [f, n] : physical) {
            top = std::max(top, f + 1U);
        }
        frags_.assign(top, Frag{});
        for (const auto& [f, n] : physical) {
            if (limit.empty() || limit.count(f) != 0U) {
                const auto r = rows.find(f);
                frags_[f] = Frag{n, r == rows.end() ? nullptr : r->second.data(), true};
            }
        }
    }

    bool allows(std::uint64_t id) const {
        const auto frag = static_cast<std::uint32_t>(id >> 32U);
        const auto offset = id & 0xFFFFFFFFULL;
        if (frag >= frags_.size()) {
            return false;
        }
        const Frag& f = frags_[frag];
        return f.live && offset < f.rows && (f.allowed == nullptr || f.allowed[offset] != 0U);
    }

private:
    struct Frag {
        std::uint64_t rows = 0;
        const std::uint8_t* allowed = nullptr;  // null: every row
        bool live = false;
    };
    std::vector<Frag> frags_;
};

/// The addresses of the rows of `fragments` passing `filter` (live rows when null).
bool row_addresses(const std::filesystem::path& path, const NearestQuery& q, const std::vector<std::uint64_t>& fragments,
                   const std::string* filter, std::vector<std::uint64_t>& out, std::string& error) {
    const std::vector<std::string> none;
    LanceScanRequest request;
    request.columns = &none;
    request.has_version = q.has_version;
    request.version = q.version;
    request.fragment_ids = &fragments;
    request.with_row_address = true;
    request.filter = filter;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!lance_dataset_scan(path, request, schema, batches, error)) {
        return false;
    }
    bool ok = true;
    const int c = child_index(schema, "_rowaddr");
    for (auto& batch : batches) {
        if (ok && c >= 0) {
            BatchView b(schema, batch, error);
            ok = b.ok;
            for (std::int64_t r = 0; ok && r < batch.length; ++r) {
                out.push_back(ArrowArrayViewGetUIntUnsafe(b.view.children[c], b.view.offset + r));
            }
        }
        batch.release(&batch);
    }
    schema.release(&schema);
    return ok;
}

// ── searching a segment ─────────────────────────────────────────────────────────────────────────

/// PQ distance table for `query` (the residual for L2 / cosine): [m][2^nbits]. Each entry sums its
/// sub-vector's dimensions in order, as Lance's prepared (transposed) tables do.
void pq_table(const IvfIndex& index, const float* query, std::vector<float>& table) {
    const std::size_t nc = std::size_t{1} << index.nbits;
    const std::size_t w = index.dim / index.m;
    table.assign(index.m * nc, 0.0F);
    const bool dot_metric = index.metric == VectorMetric::Dot;
    for (std::size_t s = 0; s < index.m; ++s) {
        float* t = table.data() + s * nc;
        for (std::size_t j = 0; j < w; ++j) {
            const float x = query[s * w + j];
            const float* column = index.codebook_t.data() + (s * w + j) * nc;
            if (dot_metric) {
                for (std::size_t c = 0; c < nc; ++c) {
                    t[c] += x * column[c];
                }
            } else {
                for (std::size_t c = 0; c < nc; ++c) {
                    const float diff = x - column[c];
                    t[c] += diff * diff;
                }
            }
        }
        if (dot_metric) {
            for (std::size_t c = 0; c < nc; ++c) {
                t[c] = 1.0F - t[c];
            }
        }
    }
}

/// Every row's PQ distance in a partition of `n` rows, as Lance computes it (k_hint: rows kept).
void pq_distances(const IvfIndex& index, const std::vector<float>& table, const std::vector<std::uint8_t>& codes,
                  std::size_t n, std::size_t k_hint, std::vector<float>& d) {
    d.assign(n, 0.0F);
    if (n == 0U) {
        return;
    }
    if (index.nbits == 8U) {
        for (std::size_t s = 0; s < index.m; ++s) {
            const float* t = table.data() + s * 256U;
            const std::uint8_t* c = codes.data() + s * n;
            for (std::size_t r = 0; r < n; ++r) {
                d[r] += t[c[r]];
            }
        }
    } else {
        // 4 bits: exact sums for the first max(200, k) rows and the last n % 16; the rest from a
        // u8-quantized table, summed with saturation, then mapped back.
        const auto exact = [&](std::size_t from, std::size_t to) {
            for (std::size_t j = 0; j < index.code_bytes; ++j) {
                const float* lo = table.data() + 2U * j * 16U;
                const float* hi = table.data() + (2U * j + 1U) * 16U;
                const std::uint8_t* c = codes.data() + j * n;
                for (std::size_t r = from; r < to; ++r) {
                    d[r] += lo[c[r] & 0x0FU];
                    d[r] += hi[c[r] >> 4U];
                }
            }
        };
        const std::size_t flat_num = std::min(std::max<std::size_t>(200U, k_hint), n);
        exact(0, flat_num);
        const float qmax = *std::max_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(flat_num),
                                             [](float a, float b) { return a < b || (std::isnan(b) && !std::isnan(a)); });
        const float qmin = *std::min_element(table.begin(), table.end());
        const float factor = 255.0F / (qmax - qmin);
        std::vector<std::uint8_t> quantized(table.size());
        for (std::size_t i = 0; i < table.size(); ++i) {
            const float v = std::round((table[i] - qmin) * factor);
            quantized[i] = !(v > 0.0F) ? 0 : (v >= 255.0F ? 255 : static_cast<std::uint8_t>(v));
        }
        const std::size_t remainder = n % 16U;
        const std::size_t full = n - remainder;
        const float range = (qmax - qmin) / 255.0F;
        for (std::size_t r = flat_num; r < full; ++r) {
            unsigned sum = 0;
            for (std::size_t j = 0; j < index.code_bytes; ++j) {
                const std::uint8_t c = codes[j * n + r];
                sum = std::min(255U, sum + quantized[2U * j * 16U + (c & 0x0FU)]);
                sum = std::min(255U, sum + quantized[(2U * j + 1U) * 16U + (c >> 4U)]);
            }
            d[r] = static_cast<float>(sum) * range + qmin;
        }
        if (remainder != 0U) {
            exact(std::max(full, flat_num), n);
        }
    }
    if (index.metric == VectorMetric::Dot) {
        const float diff = static_cast<float>(index.m) - 1.0F;
        for (auto& x : d) {
            x -= diff;
        }
    }
}

/// The best `kk` candidates of one index segment.
bool search_segment(const IvfIndex& index, const NearestQuery& q, bool f32_column, const RowMask& mask,
                    std::size_t kk, std::vector<Candidate>& out, std::string& plan, std::string& error) {
    if (q.key.size() != index.dim) {
        error = "Query vector size " + std::to_string(q.key.size()) + " does not match index column size " +
                std::to_string(index.dim);
        return false;
    }
    std::vector<float> key = q.key;
    if (index.metric == VectorMetric::Cosine) {
        const float norm = std::sqrt(dot(key.data(), key.data(), key.size()));
        for (auto& x : key) {
            x /= norm;
        }
    }
    const std::size_t dim = index.dim;
    const std::size_t P = index.partitions;
    std::vector<float> to_centroid(P);
    for (std::size_t p = 0; p < P; ++p) {
        const float* c = index.centroids.data() + p * dim;
        to_centroid[p] = index.metric == VectorMetric::Dot ? 1.0F - dot(key.data(), c, dim) : l2(key.data(), c, dim);
    }
    std::vector<std::size_t> order(P);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return better(Candidate{to_centroid[a], 0}, Candidate{to_centroid[b], 0});
    });
    std::vector<float> sorted(P);
    for (std::size_t i = 0; i < P; ++i) {
        sorted[i] = to_centroid[order[i]];
    }
    std::size_t first = 0;
    std::size_t most = 0;
    probe_counts(index, q, f32_column, sorted, first, most);
    const std::uint64_t max_results =
        mask.has_count ? std::min<std::uint64_t>(q.k, mask.count) : static_cast<std::uint64_t>(q.k);

    std::vector<float> dot_table;  // dot: one table for every partition
    if (index.pq && index.metric == VectorMetric::Dot) {
        pq_table(index, key.data(), dot_table);
    }
    // One partition's best `kk` rows the mask and the distance range allow.
    const auto score = [&](std::size_t p, std::vector<Candidate>& local, std::string& why) {
        std::shared_ptr<const Partition> part;
        if (!index.partition(p, part, why)) {
            return false;
        }
        const std::size_t n = part->ids.size();
        std::vector<float> d;
        if (index.pq) {
            std::vector<float> own;
            if (index.metric != VectorMetric::Dot) {
                std::vector<float> residual(dim);
                const float* c = index.centroids.data() + p * dim;
                for (std::size_t j = 0; j < dim; ++j) {
                    residual[j] = key[j] - c[j];
                }
                pq_table(index, residual.data(), own);
            }
            pq_distances(index, index.metric == VectorMetric::Dot ? dot_table : own, part->codes, n, kk, d);
        } else {
            d.resize(n);
            for (std::size_t r = 0; r < n; ++r) {
                d[r] = distance(index.metric, part->vectors.data() + r * dim, key.data(), dim, 1.0F);
            }
        }
        for (std::size_t r = 0; r < n; ++r) {
            if (in_range(d[r], q) && mask.allows(part->ids[r])) {
                local.push_back(Candidate{d[r], part->ids[r]});
            }
        }
        keep_best(local, kk);
        return true;
    };
    // The first `first` partitions are searched whatever they hold, in parallel.
    std::vector<std::vector<Candidate>> early(std::min(first, most));
    std::vector<std::string> errors(early.size());
    std::vector<std::uint8_t> ok(early.size(), 1);
    parallel::for_each(early.size(), [&](std::size_t i) { ok[i] = score(order[i], early[i], errors[i]) ? 1 : 0; });
    std::uint64_t found = 0;
    for (std::size_t i = 0; i < early.size(); ++i) {
        if (ok[i] == 0U) {
            error = errors[i];
            return false;
        }
        found += early[i].size();
        out.insert(out.end(), early[i].begin(), early[i].end());
    }
    // Then one at a time while fewer than k rows were found.
    std::size_t searched = early.size();
    for (; searched < most && found < max_results; ++searched) {
        std::vector<Candidate> local;
        if (!score(order[searched], local, error)) {
            return false;
        }
        found += local.size();
        out.insert(out.end(), local.begin(), local.end());
    }
    // Fewer rows pass the prefilter than k, and the search did not reach them all: Lance returns
    // the rest too, at an infinite distance.
    if (mask.has_count && mask.count <= q.k && found < mask.count) {
        std::set<std::uint64_t> have;
        for (const auto& c : out) {
            have.insert(c.id);
        }
        for (const auto& [frag, rows] : mask.rows) {
            for (std::size_t r = 0; r < rows.size(); ++r) {
                const std::uint64_t id = (static_cast<std::uint64_t>(frag) << 32U) | r;
                if (rows[r] != 0U && have.count(id) == 0U && mask.allows(id)) {
                    out.push_back(Candidate{std::numeric_limits<float>::infinity(), id});
                }
            }
        }
    }
    keep_best(out, kk);
    plan = "ANNIvfPartition: nprobes=" + std::to_string(searched) + " of " + std::to_string(P) +
           " (minimum " + std::to_string(first) + ")";
    return true;
}

// ── exact distances ─────────────────────────────────────────────────────────────────────────────

/// Exact distances of `rows`' vectors (null vectors dropped).
bool rescore(const std::filesystem::path& path, const NearestQuery& q, VectorMetric metric, std::size_t dim,
             std::vector<Candidate>& rows, std::string& error) {
    if (rows.empty()) {
        return true;
    }
    std::vector<std::uint64_t> addrs;
    std::map<std::uint64_t, float> keep;
    for (const auto& c : rows) {
        if (std::isinf(c.d) && c.d > 0) {
            keep[c.id] = c.d;  // the prefilter's leftovers stay as they are
        } else {
            addrs.push_back(c.id);
        }
    }
    std::sort(addrs.begin(), addrs.end());
    addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
    const std::vector<std::string> columns = {q.column};
    LanceScanRequest request;
    request.columns = &columns;
    request.has_version = q.has_version;
    request.version = q.version;
    request.with_row_address = true;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!addrs.empty() && !lance_dataset_take_rows(path, request, addrs, schema, batches, error)) {
        return false;
    }
    VectorRows v;
    v.dim = dim;
    bool ok = addrs.empty() ||
              collect_vectors(schema, batches, child_index(schema, q.column), child_index(schema, "_rowaddr"), v, error);
    for (auto& b : batches) {
        b.release(&b);
    }
    if (schema.release != nullptr) {
        schema.release(&schema);
    }
    if (!ok) {
        return false;
    }
    const float q_norm = std::sqrt(dot(q.key.data(), q.key.data(), dim));
    std::vector<Candidate> out;
    for (std::size_t r = 0; r < v.ids.size(); ++r) {
        if (v.valid[r] != 0U) {
            out.push_back(Candidate{distance(metric, v.values.data() + r * dim, q.key.data(), dim, q_norm), v.ids[r]});
        }
    }
    for (const auto& [id, d] : keep) {
        out.push_back(Candidate{d, id});
    }
    rows = std::move(out);
    return true;
}

/// The best k of `fragments` by exact distance.
bool flat_search(const std::filesystem::path& path, const NearestQuery& q, VectorMetric metric, std::size_t dim,
                 const std::vector<std::uint64_t>& fragments, std::vector<Candidate>& out, std::string& error) {
    if (fragments.empty()) {
        return true;
    }
    const std::vector<std::string> columns = {q.column};
    LanceScanRequest request;
    request.columns = &columns;
    request.has_version = q.has_version;
    request.version = q.version;
    request.fragment_ids = &fragments;
    request.with_row_address = true;
    request.filter = q.filter && q.prefilter ? &*q.filter : nullptr;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!lance_dataset_scan(path, request, schema, batches, error)) {
        return false;
    }
    const float q_norm = std::sqrt(dot(q.key.data(), q.key.data(), dim));
    const int vc = child_index(schema, q.column);
    const int ic = child_index(schema, "_rowaddr");
    bool ok = vc >= 0 && ic >= 0;
    if (!ok) {
        error = "vector column '" + q.column + "' not read";
    }
    std::vector<Candidate> found;
    for (auto& batch : batches) {
        if (ok) {
            VectorRows v;
            v.dim = dim;
            std::vector<ArrowArray> one;  // collect_vectors takes a list; reuse without copying ownership
            one.push_back(batch);
            ok = collect_vectors(schema, one, vc, ic, v, error);
            for (std::size_t r = 0; ok && r < v.ids.size(); ++r) {
                if (v.valid[r] == 0U) {
                    continue;
                }
                const float d = distance(metric, v.values.data() + r * dim, q.key.data(), dim, q_norm);
                if (in_range(d, q)) {
                    found.push_back(Candidate{d, v.ids[r]});
                }
            }
            if (found.size() > 4U * q.k + 1024U) {
                keep_best(found, q.k);
            }
        }
        batch.release(&batch);
    }
    schema.release(&schema);
    if (!ok) {
        return false;
    }
    keep_best(found, q.k);
    out.insert(out.end(), found.begin(), found.end());
    return true;
}

/// Keep the hits passing `filter` (Lance's post-filter), in order.
bool post_filter(const std::filesystem::path& path, const NearestQuery& q, std::vector<Candidate>& hits,
                 std::string& error) {
    if (hits.empty()) {
        return true;
    }
    expr::Expression filter;
    if (!expr::Expression::parse(*q.filter, filter, error)) {
        return false;
    }
    std::vector<std::string> columns;
    for (const auto& c : filter.columns()) {
        const auto top = c.substr(0, c.find('.'));
        if (std::find(columns.begin(), columns.end(), top) == columns.end()) {
            columns.push_back(top);
        }
    }
    std::vector<std::uint64_t> addrs;
    for (const auto& h : hits) {
        addrs.push_back(h.id);
    }
    std::sort(addrs.begin(), addrs.end());
    addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
    LanceScanRequest request;
    request.columns = &columns;
    request.has_version = q.has_version;
    request.version = q.version;
    request.with_row_address = true;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!lance_dataset_take_rows(path, request, addrs, schema, batches, error)) {
        return false;
    }
    std::set<std::uint64_t> pass;
    bool ok = filter.bind(schema, error);
    const int ic = child_index(schema, "_rowaddr");
    for (auto& batch : batches) {
        if (ok) {
            std::vector<std::uint8_t> keep;
            ok = filter.filter(batch, keep, error);
            BatchView b(schema, batch, error);
            ok = ok && b.ok;
            for (std::int64_t r = 0; ok && r < batch.length; ++r) {
                if (keep[static_cast<std::size_t>(r)] != 0U) {
                    pass.insert(ArrowArrayViewGetUIntUnsafe(b.view.children[ic], b.view.offset + r));
                }
            }
        }
        batch.release(&batch);
    }
    schema.release(&schema);
    if (!ok) {
        return false;
    }
    hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const Candidate& h) { return pass.count(h.id) == 0U; }),
               hits.end());
    return true;
}

/// The id of the field at dotted `path`; -1 when there is none.
std::int32_t field_id(const pb::Manifest& manifest, const std::string& path) {
    std::int32_t parent = -1;
    std::size_t at = 0;
    while (true) {
        const auto dot_at = path.find('.', at);
        const auto name = path.substr(at, dot_at == std::string::npos ? std::string::npos : dot_at - at);
        std::int32_t id = -1;
        for (const auto& f : manifest.fields) {
            if (f.parent_id == parent && f.name == name) {
                id = f.id;
                break;
            }
        }
        if (id < 0 || dot_at == std::string::npos) {
            return id;
        }
        parent = id;
        at = dot_at + 1U;
    }
}

}  // namespace

bool index_build::load_vector_model(const std::filesystem::path& dir, VectorModel& out, std::string& error) {
    std::shared_ptr<const IvfIndex> index;
    if (!load_ivf(dir, index, error)) {
        return false;
    }
    out.type = index->type;
    out.metric = index->metric;
    out.dim = index->dim;
    out.partitions = index->partitions;
    out.centroids = index->centroids;
    out.nbits = index->nbits;
    out.m = index->pq ? index->m : 0U;
    out.codebook = index->pq ? index->codebook : std::vector<float>{};
    out.lengths = index->lengths;
    return true;
}

bool is_vector_index_url(const std::string& url) {
    const std::string suffix = "VectorIndexDetails";  // /lance.index.pb.VectorIndexDetails, /lance.table....
    return url.size() >= suffix.size() && url.compare(url.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string vector_index_type(const std::filesystem::path& dir, const std::vector<std::uint8_t>& details) {
    auto type = vector_index_type(details);
    if (type.empty()) {
        std::string value;
        bool found = false;
        LanceDataFileFooterLayout layout{};
        std::string error;
        if (schema_metadata(dir / "index.idx", "lance:index", value, found, layout, error) && found) {
            type = json_value(value, "type");
        }
    }
    return type;
}

std::string vector_index_type(const std::vector<std::uint8_t>& details) {
    const auto* p = details.data();
    const auto* end = p + details.size();
    bool hnsw = false;
    std::string compression;
    Field f;
    while (p < end) {
        if (!next_field(p, end, f)) {
            return {};
        }
        if (f.number == 3) {
            hnsw = true;
        } else if (f.number == 4) {
            compression = "PQ";
        } else if (f.number == 5) {
            compression = "SQ";
        } else if (f.number == 6) {
            compression = "RQ";
        } else if (f.number == 8) {
            compression = "FLAT";
        }
    }
    if (compression.empty()) {
        return {};
    }
    return std::string(hnsw ? "IVF_HNSW_" : "IVF_") + compression;
}

bool dataset_nearest(const std::filesystem::path& dataset_path, const NearestQuery& q, NearestResult& out,
                     std::string& error) {
    out = NearestResult{};
    error.clear();
    if (q.key.empty()) {
        error = "the query vector is empty";
        return false;
    }
    pb::Manifest manifest;
    std::uint64_t version = q.version;
    if (q.has_version ? !load_manifest_version(dataset_path, q.version, manifest, error)
                      : !load_latest_manifest(dataset_path, manifest, version, error)) {
        return false;
    }
    // The column: a fixed-size list of floats, of the query's dimension.
    ArrowSchema schema{};
    LanceScanRequest schema_request;
    schema_request.has_version = q.has_version;
    schema_request.version = q.version;
    if (!lance_dataset_schema(dataset_path, schema_request, schema, error)) {
        return false;
    }
    std::size_t dim = 0;
    bool f32_column = false;
    {
        const ArrowSchema* column = nullptr;
        const ArrowSchema* level = &schema;
        std::size_t at = 0;
        while (level != nullptr) {
            const auto dot_at = q.column.find('.', at);
            const auto name = q.column.substr(at, dot_at == std::string::npos ? std::string::npos : dot_at - at);
            const int i = child_index(*level, name);
            if (i < 0) {
                column = nullptr;
                break;
            }
            column = level->children[i];
            if (dot_at == std::string::npos) {
                break;
            }
            level = column;
            at = dot_at + 1U;
        }
        std::string format = column == nullptr || column->format == nullptr ? "" : column->format;
        std::string item = column == nullptr || column->n_children != 1 || column->children[0]->format == nullptr
                               ? ""
                               : column->children[0]->format;
        if (column != nullptr && format.rfind("+w:", 0) == 0) {
            dim = static_cast<std::size_t>(std::strtoull(format.c_str() + 3, nullptr, 10));
            f32_column = item == "f";
        }
        schema.release(&schema);
        if (column == nullptr) {
            error = "Embedding column " + q.column + " is not in the dataset";
            return false;
        }
        if (dim == 0U || (item != "f" && item != "e" && item != "g")) {
            error = "Query column " + q.column + " must be a vector of floats";
            return false;
        }
    }
    if (q.key.size() != dim) {
        error = "Query vector size " + std::to_string(q.key.size()) + " does not match index column size " +
                std::to_string(dim);
        return false;
    }
    if (q.k == 0U) {
        return true;
    }

    // The column's vector index segments.
    const std::int32_t fid = field_id(manifest, q.column);
    std::vector<const pb::IndexMetadata*> segments;
    std::string index_name;
    for (const auto& index : manifest.indices) {
        if (index.fields.size() == 1U && index.fields[0] == fid && is_vector_index_url(index.details_type_url)) {
            if (index_name.empty()) {
                index_name = index.name;
            }
            if (index.name == index_name) {
                segments.push_back(&index);
            }
        }
    }
    std::vector<std::shared_ptr<const IvfIndex>> loaded;
    std::string not_used;  // why the index was not used
    if (!q.use_index) {
        not_used = "use_index=False";
    } else if (!segments.empty()) {
        if ((manifest.reader_feature_flags & pb::kFlagStableRowIds) != 0U) {
            not_used = "stable row ids";
        }
        for (const auto* s : segments) {
            if (!not_used.empty()) {
                break;
            }
            std::shared_ptr<const IvfIndex> index;
            std::string why;
            if (!load_ivf(dataset_path / "_indices" / pb::uuid_string(s->uuid), index, why)) {
                not_used = why;
            } else if (q.metric && *q.metric != index->metric) {
                not_used = std::string("the index's metric is ") + vector_metric_name(index->metric);
            } else {
                loaded.push_back(index);
            }
        }
        if (!not_used.empty()) {
            loaded.clear();
        }
    }
    const VectorMetric metric = !loaded.empty() ? loaded.front()->metric : q.metric.value_or(VectorMetric::L2);

    std::vector<std::uint64_t> all_fragments;
    for (const auto& f : manifest.fragments) {
        all_fragments.push_back(f.id);
    }
    std::vector<Candidate> hits;
    if (loaded.empty()) {
        if (!segments.empty() && !not_used.empty()) {
            out.plan.push_back("index " + index_name + " not used: " + not_used);
        }
        out.plan.push_back(std::string("KNNVectorDistance: metric=") + vector_metric_name(metric) +
                           " (exact, " + std::to_string(all_fragments.size()) + " fragments)");
        if (!flat_search(dataset_path, q, metric, dim, all_fragments, hits, error)) {
            return false;
        }
    } else {
        const std::size_t refine = q.refine_factor.value_or(1U);
        const std::size_t kk = static_cast<std::size_t>(q.k) * std::max<std::size_t>(refine, 1U);
        std::set<std::uint32_t> covered;
        std::vector<Candidate> ann;
        out.plan.push_back("ANNSubIndex: name=" + index_name + ", k=" + std::to_string(q.k) + ", deltas=" +
                           std::to_string(loaded.size()) + ", metric=" + vector_metric_name(metric) + " (" +
                           loaded.front()->type + ")");
        for (std::size_t s = 0; s < loaded.size(); ++s) {
            RowMask mask;
            for (const auto& f : manifest.fragments) {
                mask.physical[static_cast<std::uint32_t>(f.id)] = f.physical_rows;
            }
            mask.limit.insert(segments[s]->fragment_ids.begin(), segments[s]->fragment_ids.end());
            covered.insert(segments[s]->fragment_ids.begin(), segments[s]->fragment_ids.end());
            std::vector<std::uint64_t> frags;
            bool deletions = false;
            for (const auto& f : manifest.fragments) {
                if (mask.limit.count(static_cast<std::uint32_t>(f.id)) != 0U) {
                    frags.push_back(f.id);
                    deletions = deletions || f.deletion_file.present;
                }
            }
            const bool explicit_filter = q.filter && q.prefilter;
            if (explicit_filter || deletions) {
                std::vector<std::uint64_t> addrs;
                if (!row_addresses(dataset_path, q, frags, explicit_filter ? &*q.filter : nullptr, addrs, error)) {
                    return false;
                }
                for (const auto f : frags) {
                    mask.rows[static_cast<std::uint32_t>(f)].assign(mask.physical[static_cast<std::uint32_t>(f)], 0);
                }
                for (const auto a : addrs) {
                    auto& rows = mask.rows[static_cast<std::uint32_t>(a >> 32U)];
                    const auto offset = static_cast<std::size_t>(a & 0xFFFFFFFFULL);
                    if (offset < rows.size()) {
                        rows[offset] = 1;
                    }
                }
                if (explicit_filter) {
                    mask.has_count = true;
                    mask.count = addrs.size();
                }
            }
            mask.seal();
            std::string line;
            if (!search_segment(*loaded[s], q, f32_column, mask, kk, ann, line, error)) {
                return false;
            }
            out.plan.push_back("  " + line);
        }
        keep_best(ann, kk);
        std::vector<std::uint64_t> unindexed;
        if (!q.fast_search) {
            for (const auto& f : manifest.fragments) {
                if (covered.count(static_cast<std::uint32_t>(f.id)) == 0U) {
                    unindexed.push_back(f.id);
                }
            }
        }
        if (q.refine_factor || !unindexed.empty()) {
            if (!rescore(dataset_path, q, metric, dim, ann, error)) {
                return false;
            }
            keep_best(ann, q.k);
            out.plan.push_back(std::string("KNNVectorDistance: metric=") + vector_metric_name(metric) +
                               " (refine " + std::to_string(kk) + " candidates)");
        }
        hits = std::move(ann);
        if (!unindexed.empty()) {
            out.plan.push_back(std::string("KNNVectorDistance: metric=") + vector_metric_name(metric) + " (exact, " +
                               std::to_string(unindexed.size()) + " unindexed fragments)");
            if (!flat_search(dataset_path, q, metric, dim, unindexed, hits, error)) {
                return false;
            }
        }
    }
    keep_best(hits, q.k);
    if (q.filter && !q.prefilter) {
        out.plan.push_back("FilterExec: " + *q.filter + " (after the search)");
        if (!post_filter(dataset_path, q, hits, error)) {
            return false;
        }
    }
    for (const auto& h : hits) {
        out.row_ids.push_back(h.id);
        out.distances.push_back(h.d);
    }
    (void)version;
    return true;
}

}  // namespace nano_lance
