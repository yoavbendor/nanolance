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

#include "nanolance/base_paths.hpp"
#include "nanolance/vector_search.hpp"
#include "rabitq_build.hpp"
#include "nanolance/work_stats.hpp"
#include "nanolance/overlays.hpp"
#include "nanolance/row_ids.hpp"

#include "index_build.hpp"
#include "fts_json.hpp"
#include "index_files.hpp"

#include "nanolance/data_file_reader.hpp"
#include "nanolance/expr.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/parallel.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include "float16.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <cstdlib>
#include <functional>
#include <optional>
#include <thread>
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
        case VectorMetric::Hamming: return "hamming";
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
                   std::vector<float>& values, std::string& error, std::vector<std::uint8_t>* u8 = nullptr) {
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
        case 4:  // UINT8: a Hamming index's centroids, kept as bytes
            if (u8 == nullptr) {
                error = "a Tensor of data type 4 (UINT8) is not supported here";
                return false;
            }
            u8->assign(bytes, bytes + nbytes);
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
    std::vector<std::uint8_t> bytes;  // a UINT8 tensor (Hamming centroids), instead of centroids
    std::size_t partitions = 0;
    std::size_t dim = 0;
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> lengths;
    bool has_loss = false;
    double loss = 0;  // field 5: the k-means loss
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
        } else if (f.number == 5 && f.wire == 1) {
            std::memcpy(&out.loss, f.data, 8U);
            out.has_loss = true;
        } else if (f.number == 4 && f.wire == 2) {
            std::vector<std::uint64_t> shape;
            if (!decode_tensor(f.data, f.size, shape, out.centroids, error, &out.bytes)) {
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
    std::vector<std::uint8_t> bytes;  // a uint8 (binary) column: dim bytes per row, instead of values
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
                     int id_column, VectorRows& out, std::string& error, const std::vector<int>& sub_path = {}) {
    for (const auto& batch : batches) {
        BatchView b(schema, batch, error);
        if (!b.ok) {
            return false;
        }
        const ArrowArrayView* list = b.view.children[vector_column];
        for (const int child : sub_path) {  // a vector inside structs: struct children share the row index
            list = list->children[child];
        }
        const ArrowArrayView* items = list->children[0];
        const ArrowArrayView* ids = id_column < 0 ? nullptr : b.view.children[id_column];
        const auto dim = static_cast<std::int64_t>(out.dim);
        const bool f32 = items->storage_type == NANOARROW_TYPE_FLOAT;
        const bool u8 = items->storage_type == NANOARROW_TYPE_UINT8;
        for (std::int64_t r = 0; r < batch.length; ++r) {
            const std::int64_t row = b.view.offset + r;
            const bool null = ArrowArrayViewIsNull(list, row);
            out.valid.push_back(null ? 0 : 1);
            const std::int64_t first = (list->offset + row) * dim;
            const std::size_t at = out.values.size();
            out.values.resize(at + out.dim);
            if (u8) {
                const std::size_t at_bytes = out.bytes.size();
                out.bytes.resize(at_bytes + out.dim);
                if (!null) {
                    std::memcpy(out.bytes.data() + at_bytes, items->buffer_views[1].data.as_uint8 + items->offset + first,
                                out.dim);
                }
            } else if (!null) {
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

/// The vectors of a multivector column (a list of fixed-size lists): row r holds count[r] vectors of
/// `dim` values, the first being vector first[r] of `values` (of `bytes` for a uint8 column). A null or
/// empty row holds none.
struct MultiRows {
    std::size_t dim = 0;
    std::vector<float> values;
    std::vector<std::uint8_t> bytes;
    std::vector<std::size_t> first;
    std::vector<std::size_t> count;
    std::vector<std::uint64_t> ids;
};

/// Append the rows of one batch: the multivector column at `vector_column` (inside structs along
/// `sub_path`) and the row addresses at `id_column`. Values are read as collect_vectors reads them.
bool collect_multivectors(const ArrowSchema& schema, const ArrowArray& batch, int vector_column, int id_column,
                          MultiRows& out, std::string& error, const std::vector<int>& sub_path = {}) {
    BatchView b(schema, batch, error);
    if (!b.ok) {
        return false;
    }
    const ArrowArrayView* list = b.view.children[vector_column];
    for (const int child : sub_path) {
        list = list->children[child];
    }
    const ArrowArrayView* vectors = list->children[0];  // the fixed-size lists
    const ArrowArrayView* items = vectors->children[0];  // their values
    const ArrowArrayView* ids = b.view.children[id_column];
    const auto dim = static_cast<std::int64_t>(out.dim);
    const bool f32 = items->storage_type == NANOARROW_TYPE_FLOAT;
    const bool u8 = items->storage_type == NANOARROW_TYPE_UINT8;
    const std::int32_t* offsets = list->buffer_views[1].data.as_int32;
    for (std::int64_t r = 0; r < batch.length; ++r) {
        const std::int64_t row = b.view.offset + r;
        const bool null = ArrowArrayViewIsNull(list, row);
        const std::int64_t begin = null ? 0 : offsets[list->offset + row];
        const std::int64_t n = null ? 0 : offsets[list->offset + row + 1] - begin;
        out.first.push_back(out.count.empty() ? 0 : out.first.back() + out.count.back());
        out.count.push_back(static_cast<std::size_t>(n));
        for (std::int64_t j = 0; j < n; ++j) {
            // element begin + j of the fixed-size lists sits at (list offset + begin + j) * dim of the values
            const std::int64_t at = (vectors->offset + begin + j) * dim;
            if (u8) {
                const std::size_t old = out.bytes.size();
                out.bytes.resize(old + out.dim);
                std::memcpy(out.bytes.data() + old, items->buffer_views[1].data.as_uint8 + items->offset + at,
                            out.dim);
            } else {
                const std::size_t old = out.values.size();
                out.values.resize(old + out.dim);
                if (f32) {
                    std::memcpy(out.values.data() + old, items->buffer_views[1].data.as_float + items->offset + at,
                                out.dim * sizeof(float));
                } else {
                    for (std::int64_t k = 0; k < dim; ++k) {
                        out.values[old + static_cast<std::size_t>(k)] =
                            static_cast<float>(ArrowArrayViewGetDoubleUnsafe(items, at + k));
                    }
                }
            }
        }
        if (ids != nullptr) {
            out.ids.push_back(ArrowArrayViewGetUIntUnsafe(ids, row));
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
/// Bits in which the `n` bytes of `a` and `b` differ.
float hamming(const std::uint8_t* a, const std::uint8_t* b, std::size_t n) {
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < n; ++i) {
        bits += static_cast<std::uint32_t>(std::popcount(static_cast<unsigned>(a[i] ^ b[i])));
    }
    return static_cast<float>(bits);
}

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
    std::vector<std::uint8_t> codes;  // IVF_PQ: [code bytes][n]; IVF_HNSW_SQ: [n][dim]; IVF_RQ: packed bits
    // IVF_RQ: the add / scale factors of the 1-bit code and of the extended code, one per row, and
    // the extended codes in Lance's blocked layout ([n][ex row bytes]).
    std::vector<float> rq_add;
    std::vector<float> rq_scale;
    std::vector<float> rq_ex_add;
    std::vector<float> rq_ex_scale;
    std::vector<float> rq_err;  // __error_factors: the lower bound's error term, per row
    std::vector<std::uint8_t> rq_ex;
    // IVF_HNSW_SQ: the partition's graph. Level 0 holds every node (row == node id); an upper level
    // only the nodes on it.
    std::vector<std::uint32_t> level0_offsets;  // n + 1
    std::vector<std::uint32_t> level0;
    std::vector<std::unordered_map<std::uint32_t, std::vector<std::uint32_t>>> upper;  // levels 1..
    std::uint32_t entry_point = 0;
    std::size_t max_level = 0;  // levels holding nodes (Lance's max_level())

    const std::uint32_t* neighbors(std::size_t level, std::uint32_t node, std::size_t& count) const {
        count = 0;
        if (level == 0) {
            if (node + 1U >= level0_offsets.size()) {
                return nullptr;
            }
            count = level0_offsets[node + 1U] - level0_offsets[node];
            return level0.data() + level0_offsets[node];
        }
        if (level - 1U >= upper.size()) {
            return nullptr;
        }
        const auto it = upper[level - 1U].find(node);
        if (it == upper[level - 1U].end()) {
            return nullptr;
        }
        count = it->second.size();
        return it->second.data();
    }
};

/// One partition's HNSW graph metadata (index.idx's `lance:hnsw` entry).
struct HnswMeta {
    std::uint32_t entry_point = 0;
    std::vector<std::uint64_t> level_offsets;
};

/// Partition lookups in the index cache during one search: a hit found the partition loaded.
struct CacheCount {
    std::atomic<std::uint64_t> hits{0};
    std::atomic<std::uint64_t> misses{0};
};

/// An IVF index's rows (auxiliary.idx): FLAT floats, PQ codes or 8-bit SQ codes.
enum class Storage { Flat, Pq, Sq, Rq };
/// The sub-index over each partition (index.idx): FLAT (exhaustive) or an HNSW graph.
enum class SubIndex { Flat, Hnsw };

/// IVF_<storage> or IVF_HNSW_<storage>: the storage and the sub-index of an index type.
bool split_index_type(const std::string& type, Storage& storage, SubIndex& sub) {
    if (type.rfind("IVF_", 0) != 0) {
        return false;
    }
    const bool hnsw = type.rfind("IVF_HNSW_", 0) == 0;
    const std::string name = type.substr(hnsw ? 9U : 4U);
    if (name == "FLAT") {
        storage = Storage::Flat;
    } else if (name == "PQ") {
        storage = Storage::Pq;
    } else if (name == "SQ") {
        storage = Storage::Sq;
    } else if (name == "RQ") {
        storage = Storage::Rq;
    } else {
        return false;
    }
    sub = hnsw ? SubIndex::Hnsw : SubIndex::Flat;
    return true;
}

struct IvfIndex {
    std::filesystem::path aux;
    std::string type;  // "IVF_FLAT", "IVF_HNSW_PQ", ...: split into storage and sub-index
    VectorMetric metric = VectorMetric::L2;
    Storage storage = Storage::Flat;
    SubIndex sub = SubIndex::Flat;
    std::size_t dim = 0;
    std::size_t partitions = 0;
    std::vector<float> centroids;  // partitions * dim
    std::vector<std::uint8_t> binary_centroids;  // Hamming: partitions * dim bytes, instead of centroids
    std::vector<std::uint64_t> offsets;
    std::vector<std::uint64_t> lengths;
    std::uint32_t nbits = 8;
    std::size_t m = 0;            // sub-vectors
    std::size_t code_bytes = 0;   // m, or m / 2 with 4 bits
    bool transposed = true;
    std::vector<float> codebook;    // [m][2^nbits][dim / m]
    // SQ storage: 8-bit scalar quantization over [sq_start, sq_end]; HNSW: the graphs in index.idx.
    double sq_start = 0;
    double sq_end = 0;
    std::filesystem::path graph;
    std::vector<std::uint64_t> graph_offsets;
    std::vector<std::uint64_t> graph_lengths;
    std::vector<HnswMeta> hnsw_meta;
    std::uint32_t hnsw_m = 20;  // the graphs' build parameters
    std::uint32_t hnsw_ef_construction = 150;
    std::uint32_t hnsw_max_level = 7;
    std::vector<float> codebook_t;  // the same as [m][dim / m][2^nbits]: a table row is one sweep
    // IVF_RQ: the fast rotation's sign bits (4 rounds of ceil(dim / 8) bytes) and the extended code's
    // bytes per row (4 extended bits, blocked: padded dim * 4 / 8).
    std::vector<std::uint8_t> rq_signs;
    std::size_t rq_ex_len = 0;
    std::uint32_t rq_bits = 5;     // num_bits: 1 binary bit and rq_bits - 1 extended bits
    bool rq_packed = true;         // the binary codes are in Lance's packed layout (else row-major)

    mutable std::mutex mutex;
    mutable std::unordered_map<std::size_t, std::shared_ptr<const Partition>> loaded;

    bool partition(std::size_t p, std::shared_ptr<const Partition>& out, std::string& error,
                   CacheCount& count) const;
    bool load_sq_rows(const FileTable& table, int id_col, int code_col, Partition& part, std::string& error) const;
    bool load_graph(std::size_t p, Partition& part, std::string& error) const;
};

bool IvfIndex::load_sq_rows(const FileTable& table, int id_col, int code_col, Partition& part,
                            std::string& error) const {
    for (const auto& batch : table.batches) {
        BatchView b(table.schema, batch, error);
        if (!b.ok) {
            return false;
        }
        const ArrowArrayView* list = b.view.children[code_col];
        const ArrowArrayView* bytes = list->children[0];
        const ArrowArrayView* ids = b.view.children[id_col];
        for (std::int64_t r = 0; r < batch.length; ++r) {
            const std::int64_t row = b.view.offset + r;
            part.ids.push_back(ArrowArrayViewGetUIntUnsafe(ids, row));
            const std::int64_t first = (list->offset + row) * static_cast<std::int64_t>(dim);
            const auto* src = bytes->buffer_views[1].data.as_uint8 + bytes->offset + first;
            part.codes.insert(part.codes.end(), src, src + dim);
        }
    }
    return true;
}

bool IvfIndex::load_graph(std::size_t p, Partition& part, std::string& error) const {
    const std::size_t n = part.ids.size();
    // The graph: index.idx rows of this partition, level by level.
    const HnswMeta& meta = hnsw_meta[p];
    const std::uint64_t rows = graph_lengths[p];
    if (meta.level_offsets.back() != rows) {
        error = "vector index: HNSW partition " + std::to_string(p) + " has " + std::to_string(rows) +
                " graph rows, its levels " + std::to_string(meta.level_offsets.back());
        return false;
    }
    std::vector<std::uint32_t> node_ids;
    std::vector<std::uint32_t> offsets{0};
    std::vector<std::uint32_t> values;
    if (rows != 0U) {
        const std::vector<std::string> columns = {"__vector_id", "__neighbors"};
        FileTable g;
        if (!read_table(graph, &columns, LanceRowRange{graph_offsets[p], rows}, g, error)) {
            return false;
        }
        const int vid = child_index(g.schema, "__vector_id");
        const int nb = child_index(g.schema, "__neighbors");
        if (vid < 0 || nb < 0) {
            error = "vector index: index.idx has no __vector_id / __neighbors column";
            return false;
        }
        for (const auto& batch : g.batches) {
            BatchView b(g.schema, batch, error);
            if (!b.ok) {
                return false;
            }
            const ArrowArrayView* lists = b.view.children[nb];
            for (std::int64_t r = 0; r < batch.length; ++r) {
                const std::int64_t row = b.view.offset + r;
                node_ids.push_back(static_cast<std::uint32_t>(ArrowArrayViewGetUIntUnsafe(b.view.children[vid], row)));
                const auto begin = ArrowArrayViewListChildOffset(lists, row);
                const auto end = ArrowArrayViewListChildOffset(lists, row + 1);
                for (auto e = begin; e < end; ++e) {
                    const auto v = static_cast<std::uint32_t>(ArrowArrayViewGetUIntUnsafe(lists->children[0], e));
                    if (v < n) {  // an edge out of the graph is dropped, as Lance drops it
                        values.push_back(v);
                    }
                }
                offsets.push_back(static_cast<std::uint32_t>(values.size()));
            }
        }
    }
    const std::size_t levels = meta.level_offsets.size() - 1U;
    std::size_t max_level = 0;
    for (std::size_t l = 0; l < levels; ++l) {
        const auto from = meta.level_offsets[l];
        const auto to = meta.level_offsets[l + 1U];
        if (to > from) {
            max_level = l + 1U;
        }
        if (l == 0) {
            if (to - from != n) {
                error = "vector index: HNSW level 0 of partition " + std::to_string(p) + " has " +
                        std::to_string(to - from) + " nodes, the partition " + std::to_string(n) + " rows";
                return false;
            }
            for (std::uint64_t r = from; r < to; ++r) {
                if (node_ids[r] != r - from) {
                    error = "vector index: HNSW level-0 __vector_id must equal the row index";
                    return false;
                }
            }
            part.level0_offsets.assign(offsets.begin() + static_cast<std::ptrdiff_t>(from),
                                       offsets.begin() + static_cast<std::ptrdiff_t>(to + 1U));
            const std::uint32_t base = part.level0_offsets.front();
            for (auto& o : part.level0_offsets) {
                o -= base;
            }
            part.level0.assign(values.begin() + offsets[from], values.begin() + offsets[to]);
        } else {
            std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> level;
            for (std::uint64_t r = from; r < to; ++r) {
                level[node_ids[r]] = std::vector<std::uint32_t>(values.begin() + offsets[r], values.begin() + offsets[r + 1U]);
            }
            part.upper.push_back(std::move(level));
        }
    }
    part.max_level = max_level;
    part.entry_point = meta.entry_point;
    if (n != 0U && part.entry_point >= n) {
        error = "vector index: HNSW entry point out of range";
        return false;
    }
    return true;
}

/// IVF_RQ's storage_metadata (a JSON list of one JSON string): the rotation, the code and the query
/// estimator. R1a reads the default layout only (5 bits, fast rotation, raw-query estimator, packed
/// codes, L2); anything else is refused by name.
bool load_rq_metadata(const std::string& text, IvfIndex& index, std::string& error) {
    fts::json::Value list;
    fts::json::Value meta;
    if (!fts::json::parse(text, list) || list.kind != fts::json::Value::Array || list.items.empty() ||
        list.items[0].kind != fts::json::Value::String || !fts::json::parse(list.items[0].s, meta)) {
        error = "vector index: IVF_RQ storage metadata is malformed";
        return false;
    }
    const auto* bits = meta.get("num_bits");
    const auto* estimator = meta.get("query_estimator");
    const auto* rotation = meta.get("rotation_type");
    const auto* position = meta.get("rotate_mat_position");
    const auto* packed = meta.get("packed");
    const auto* code_dim = meta.get("code_dim");
    const auto* signs = meta.get("fast_rotation_signs");
    // num_bits 1 to 9 (Lance's range): one binary bit and num_bits - 1 extended bits.
    if (bits == nullptr || bits->n < 1.0 || bits->n > 9.0 || bits->n != std::floor(bits->n)) {
        error = "vector index: IVF_RQ with " + std::to_string(bits == nullptr ? 0 : static_cast<int>(bits->n)) +
                " bits is not supported";
        return false;
    }
    if (estimator == nullptr || estimator->s != "raw_query") {
        error = "vector index: IVF_RQ with the " + (estimator == nullptr ? std::string("missing") : estimator->s) +
                " query estimator is not supported yet";
        return false;
    }
    if (rotation == nullptr || rotation->s != "fast") {
        error = "vector index: IVF_RQ with the " + (rotation == nullptr ? std::string("missing") : rotation->s) +
                " rotation is not supported yet";
        return false;
    }
    if (position != nullptr && position->kind != fts::json::Value::Null) {
        error = "vector index: IVF_RQ with a dense rotation matrix is not supported yet";
        return false;
    }
    // Lance packs unpacked codes (skip_transpose) when it loads them, so both layouts search alike.
    if (packed == nullptr || packed->kind != fts::json::Value::Bool) {
        error = "vector index: IVF_RQ storage metadata has no packed flag";
        return false;
    }
    index.rq_packed = packed->b;
    index.rq_bits = static_cast<std::uint32_t>(bits->n);
    const std::size_t dim = index.dim;
    if (dim == 0U || dim % 8U != 0U || code_dim == nullptr || static_cast<std::size_t>(code_dim->n) != dim) {
        error = "vector index: IVF_RQ code_dim does not match the centroids' dimension " + std::to_string(dim);
        return false;
    }
    const std::size_t bytes_per_round = (dim + 7U) / 8U;
    if (signs == nullptr || signs->kind != fts::json::Value::Array || signs->items.size() != 4U * bytes_per_round) {
        error = "vector index: IVF_RQ has no fast rotation signs for dimension " + std::to_string(dim);
        return false;
    }
    index.rq_signs.clear();
    for (const auto& item : signs->items) {
        index.rq_signs.push_back(static_cast<std::uint8_t>(item.n));
    }
    index.code_bytes = dim / 8U;
    // The extended codes: num_bits - 1 bits per dimension, blocked by 64 dims (Lance's blocked_ex_code_bytes).
    index.rq_ex_len = index.rq_bits > 1U ? (dim + 63U) / 64U * 64U * (index.rq_bits - 1U) / 8U : 0U;
    return true;
}

/// One IVF_RQ partition: the 1-bit codes (packed), the factors and, for num_bits > 1, the blocked extended codes.
bool load_rq_partition(const IvfIndex& index, std::uint64_t first, std::uint64_t n, Partition& part,
                       std::string& error) {
    const bool ex = index.rq_bits > 1U;
    std::vector<std::string> columns = {"_rowid", "_rabit_codes", "__add_factors", "__scale_factors",
                                        "__error_factors"};
    if (ex) {
        columns.insert(columns.end(), {"__add_factors_ex", "__scale_factors_ex", "__blocked_ex_codes"});
    }
    FileTable table;
    if (!read_table(index.aux, &columns, LanceRowRange{first, n}, table, error)) {
        return false;
    }
    std::vector<int> col(columns.size());
    for (std::size_t c = 0; c < columns.size(); ++c) {
        col[c] = child_index(table.schema, columns[c]);
        if (col[c] < 0) {
            error = "vector index: auxiliary.idx has no " + columns[c] + " column";
            return false;
        }
    }
    const auto code_bytes = static_cast<std::int64_t>(index.code_bytes);
    const auto ex_bytes = static_cast<std::int64_t>(index.rq_ex_len);
    for (const auto& batch : table.batches) {
        BatchView b(table.schema, batch, error);
        if (!b.ok) {
            return false;
        }
        const ArrowArrayView* ids = b.view.children[col[0]];
        const ArrowArrayView* codes = b.view.children[col[1]];
        const ArrowArrayView* code_items = codes->children[0];
        const ArrowArrayView* ex_codes = ex ? b.view.children[col[7]] : nullptr;
        const ArrowArrayView* ex_items = ex ? ex_codes->children[0] : nullptr;
        for (std::int64_t r = 0; r < batch.length; ++r) {
            const std::int64_t row = b.view.offset + r;
            part.ids.push_back(ArrowArrayViewGetUIntUnsafe(ids, row));
            const auto* src = code_items->buffer_views[1].data.as_uint8 + code_items->offset +
                              (codes->offset + row) * code_bytes;
            part.codes.insert(part.codes.end(), src, src + code_bytes);
            part.rq_add.push_back(static_cast<float>(ArrowArrayViewGetDoubleUnsafe(b.view.children[col[2]], row)));
            part.rq_scale.push_back(static_cast<float>(ArrowArrayViewGetDoubleUnsafe(b.view.children[col[3]], row)));
            part.rq_err.push_back(static_cast<float>(ArrowArrayViewGetDoubleUnsafe(b.view.children[col[4]], row)));
            if (ex) {
                const auto* ex_src = ex_items->buffer_views[1].data.as_uint8 + ex_items->offset +
                                     (ex_codes->offset + row) * ex_bytes;
                part.rq_ex.insert(part.rq_ex.end(), ex_src, ex_src + ex_bytes);
                part.rq_ex_add.push_back(
                    static_cast<float>(ArrowArrayViewGetDoubleUnsafe(b.view.children[col[5]], row)));
                part.rq_ex_scale.push_back(
                    static_cast<float>(ArrowArrayViewGetDoubleUnsafe(b.view.children[col[6]], row)));
            }
        }
    }
    const auto n_ex = ex ? n : 0U;
    if (part.ids.size() != n || part.codes.size() != n * index.code_bytes || part.rq_ex.size() != n_ex * index.rq_ex_len ||
        part.rq_add.size() != n || part.rq_scale.size() != n || part.rq_ex_add.size() != n_ex ||
        part.rq_ex_scale.size() != n_ex || part.rq_err.size() != n) {
        error = "vector index: IVF_RQ partition has " + std::to_string(part.ids.size()) + " rows, not " +
                std::to_string(n);
        return false;
    }
    return true;
}

bool IvfIndex::partition(std::size_t p, std::shared_ptr<const Partition>& out, std::string& error,
                         CacheCount& count) const {
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = loaded.find(p);
        if (it != loaded.end()) {
            count.hits++;
            work_stats::count_index_cache(true);
            out = it->second;
            return true;
        }
    }
    count.misses++;
    work_stats::count_index_cache(false);
    auto part = std::make_shared<Partition>();
    const std::uint64_t n = lengths[p];
    if (n != 0U && storage == Storage::Rq) {
        if (!load_rq_partition(*this, offsets[p], n, *part, error)) {
            return false;
        }
    } else if (n != 0U) {
        const std::vector<std::string> columns = {
            "_rowid", storage == Storage::Pq ? "__pq_code" : (storage == Storage::Sq ? "__sq_code" : "flat")};
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
        if (storage == Storage::Sq) {
            if (!load_sq_rows(table, id_col, value_col, *part, error)) {
                return false;
            }
        } else if (storage == Storage::Flat && metric == VectorMetric::Hamming) {
            VectorRows rows;  // uint8 rows: dim bytes each, kept as codes
            rows.dim = dim;
            if (!collect_vectors(table.schema, table.batches, value_col, id_col, rows, error)) {
                return false;
            }
            part->ids = std::move(rows.ids);
            part->codes = std::move(rows.bytes);
        } else if (storage == Storage::Flat) {
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
        if (sub == SubIndex::Hnsw && !load_graph(p, *part, error)) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    out = loaded.emplace(p, std::move(part)).first->second;
    return true;
}

/// An index's stored distance type: pylance's names, and "hamming" (IVF_FLAT over uint8 vectors).
bool parse_index_metric(const std::string& name, VectorMetric& out) {
    if (name == "hamming") {
        out = VectorMetric::Hamming;
        return true;
    }
    return parse_vector_metric(name, out);
}

bool load_ivf(const std::filesystem::path& dir, std::shared_ptr<const IvfIndex>& out, std::string& error,
              bool use_cache = true) {
    static IndexCache<IvfIndex> cache;
    const auto index_path = dir / "index.idx";
    const auto aux_path = dir / "auxiliary.idx";
    const auto key = file_key(index_path) + "|" + file_key(aux_path);
    if (use_cache && (out = cache.find(key)) != nullptr) {
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
    if (!split_index_type(index->type, index->storage, index->sub)) {
        error = "vector index type '" + index->type + "' is not supported";
        return false;
    }
    if (!parse_index_metric(json_value(value, "distance_type"), index->metric)) {
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
    const bool binary_index = index->metric == VectorMetric::Hamming;
    if (binary_index) {
        if (index->storage != Storage::Flat || index->sub != SubIndex::Flat) {
            error = "vector index: a Hamming index must be IVF_FLAT";
            return false;
        }
        index->binary_centroids = std::move(model.bytes);
    } else {
        index->centroids = std::move(model.centroids);
    }
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
    const std::size_t centroid_values = binary_index ? index->binary_centroids.size() : index->centroids.size();
    if (index->partitions == 0U || index->dim == 0U || centroid_values != index->partitions * index->dim ||
        index->offsets.size() != index->partitions || index->lengths.size() != index->partitions) {
        error = "vector index: " + std::to_string(index->partitions) + " partitions of dimension " +
                std::to_string(index->dim) + " do not match its centroids and partition table";
        return false;
    }
    if (index->storage == Storage::Sq) {
        // {"dim", "num_bits": 8, "bounds": {"start", "end"}}, and per partition {"entry_point",
        // "params", "level_offsets"}; the graph rows of each partition from index.idx's IVF table.
        fts::json::Value sq_list;
        fts::json::Value sq;
        if (!fts::json::parse(storage, sq_list) || sq_list.kind != fts::json::Value::Array || sq_list.items.empty() ||
            sq_list.items[0].kind != fts::json::Value::String || !fts::json::parse(sq_list.items[0].s, sq) ||
            sq.get("bounds") == nullptr || sq.get("bounds")->get("start") == nullptr ||
            sq.get("bounds")->get("end") == nullptr) {
            error = "vector index: IVF_HNSW_SQ storage metadata has no bounds";
            return false;
        }
        const auto* bits = sq.get("num_bits");
        if (bits != nullptr && bits->n != 8.0) {
            error = "vector index: SQ with " + std::to_string(static_cast<int>(bits->n)) + " bits is not supported";
            return false;
        }
        index->sq_start = sq.get("bounds")->get("start")->n;
        index->sq_end = sq.get("bounds")->get("end")->n;
        index->code_bytes = index->dim;
    }
    if (index->sub == SubIndex::Hnsw) {
        index->graph = index_path;
        index->graph_offsets = model.offsets;
        index->graph_lengths = model.lengths;
        std::string hnsw_text;
        if (!schema_metadata(index_path, "lance:hnsw", hnsw_text, found, layout, error)) {
            return false;
        }
        fts::json::Value list;
        if (!found || !fts::json::parse(hnsw_text, list) || list.kind != fts::json::Value::Array ||
            list.items.size() != index->partitions || index->graph_offsets.size() != index->partitions ||
            index->graph_lengths.size() != index->partitions) {
            error = "vector index: IVF_HNSW_SQ index.idx has no graph for every partition";
            return false;
        }
        for (const auto& item : list.items) {
            fts::json::Value meta;
            if (item.kind != fts::json::Value::String || !fts::json::parse(item.s, meta) ||
                meta.get("level_offsets") == nullptr || meta.get("entry_point") == nullptr) {
                error = "vector index: malformed HNSW metadata";
                return false;
            }
            HnswMeta m;
            m.entry_point = static_cast<std::uint32_t>(meta.get("entry_point")->n);
            for (const auto& o : meta.get("level_offsets")->items) {
                m.level_offsets.push_back(static_cast<std::uint64_t>(o.n));
            }
            if (m.level_offsets.empty()) {
                m.level_offsets.push_back(0);
            }
            if (const auto* params = meta.get("params"); params != nullptr && index->hnsw_meta.empty()) {
                const auto param = [&](const char* key, std::uint32_t& out) {
                    if (const auto* x = params->get(key); x != nullptr && x->n >= 1.0) {
                        out = static_cast<std::uint32_t>(x->n);
                    }
                };
                param("m", index->hnsw_m);
                param("ef_construction", index->hnsw_ef_construction);
                param("max_level", index->hnsw_max_level);
            }
            index->hnsw_meta.push_back(std::move(m));
        }
    }
    if (index->storage == Storage::Rq) {
        if (!load_rq_metadata(storage, *index, error)) {
            return false;
        }
    }
    if (index->storage == Storage::Pq) {
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
    if (use_cache) {
        cache.put(key, out);
    }
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
    const bool adaptive = !q.maximum_nprobes && f32_column && finite_key && index.type == "IVF_FLAT" &&
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
    /// With stable row ids the index holds ids, not addresses: this finds the address of one.
    const RowIdToAddress* resolver = nullptr;
    /// The caller's row-address masks, which every candidate must pass as well (null: none).
    const RowAddrMask* caller = nullptr;

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
        if ((resolver != nullptr || caller != nullptr) && !translate(id)) {
            return false;
        }
        const auto frag = static_cast<std::uint32_t>(id >> 32U);
        const auto offset = id & 0xFFFFFFFFULL;
        if (frag >= frags_.size()) {
            return false;
        }
        const Frag& f = frags_[frag];
        return f.live && offset < f.rows && (f.allowed == nullptr || f.allowed[offset] != 0U);
    }

private:
    /// Stable row ids to addresses, then the caller's masks. Out of line, so the usual case -- neither
    /// -- stays an inlined bounds and bitmap check in the scoring loops.
    [[gnu::noinline]] bool translate(std::uint64_t& id) const {
        if (resolver != nullptr && resolver->stable() && !resolver->find(id, id)) {
            return false;  // an id whose row is gone
        }
        return caller == nullptr || caller->allows(id);  // the caller's mask (`id` is an address by now)
    }

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
    request.row_mask = q.row_mask;
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
/// sub-vector's dimensions in order, as Lance's prepared (transposed) tables do. Dot: 1 - dot per
/// sub-vector, so the exhaustive scan's distance is m - dot; `dot_once` gives the HNSW graphs' 1 - dot
/// (negated entries; the caller adds the 1 once).
void pq_table(const IvfIndex& index, const float* query, std::vector<float>& table, bool dot_once = false) {
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
                t[c] = dot_once ? -t[c] : 1.0F - t[c];
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

// ── IVF_RQ (RaBitQ) ───────────────────────────────────────────────────────────────────────────────
// Lance's raw-query estimator (storage.rs, rotation.rs, ex_dot.rs): the query is rotated by the fast
// rotation, the 1-bit codes give its inner product with each row's sign pattern through a table of
// 4-dimension segments, and the 4 extended bits add the rest of the code. Codes are Lance's packed
// layout (32 rows per block, PERM0 order); the extended codes are the blocked 64-dim layout.

/// Lance's fast rotation: 4 rounds of random sign flips and a Walsh-Hadamard transform, with the
/// Kac step for dimensions that are not powers of two.
void rq_fwht(float* v, std::size_t n) {
    for (std::size_t half = 1; half < n; half *= 2) {
        for (std::size_t base = 0; base < n; base += 2 * half) {
            for (std::size_t i = 0; i < half; ++i) {
                const float x = v[base + i];
                const float y = v[base + i + half];
                v[base + i] = x + y;
                v[base + i + half] = x - y;
            }
        }
    }
}

void rq_flip(float* v, std::size_t n, const std::uint8_t* signs) {
    for (std::size_t i = 0; i < n; ++i) {
        if (((signs[i / 8U] >> (i % 8U)) & 1U) != 0U) {
            v[i] = -v[i];
        }
    }
}

void rq_scale(float* v, std::size_t n, float factor) {
    for (std::size_t i = 0; i < n; ++i) {
        v[i] *= factor;
    }
}

void rq_rotate(const float* in, std::size_t dim, const std::uint8_t* signs, float* out) {
    constexpr std::size_t kRounds = 4;
    std::copy(in, in + dim, out);
    const std::size_t bytes_per_round = (dim + 7U) / 8U;
    std::size_t trunc = 1;
    while (trunc * 2U <= dim) {
        trunc *= 2U;
    }
    const float scale = 1.0F / std::sqrt(static_cast<float>(trunc));
    if (trunc == dim) {
        for (std::size_t round = 0; round < kRounds; ++round) {
            rq_flip(out, dim, signs + round * bytes_per_round);
            rq_fwht(out, dim);
            rq_scale(out, dim, scale);
        }
        return;
    }
    const std::size_t start = dim - trunc;
    for (std::size_t round = 0; round < kRounds; ++round) {
        rq_flip(out, dim, signs + round * bytes_per_round);
        if (round % 2U == 0U) {
            rq_fwht(out, trunc);
            rq_scale(out, trunc, scale);
        } else {
            rq_fwht(out + start, trunc);
            rq_scale(out + start, trunc, scale);
        }
        const std::size_t half = dim / 2U;  // the Kac mixing step (x, y) -> (x + y, x - y)
        for (std::size_t i = 0; i < half; ++i) {
            const float x = out[i];
            const float y = out[half + i];
            out[i] = x + y;
            out[half + i] = x - y;
        }
    }
    rq_scale(out, dim, 0.25F);
}

/// The query's side of the estimator, once per search: the rotated query (zero-padded to 64-dim
/// blocks for the extended dot), its 4-dimension distance tables and its sum.
struct RqQuery {
    std::vector<float> dist_table;  // dim / 4 segments of 16 sums: entry [s * 16 + code]
    std::vector<float> ex_query;    // the rotated query, padded to a multiple of 64
    std::vector<float> rotated;     // the rotated query (dot: its inner product with the rotated centroids)
    float sum_q = 0.0F;
    bool packed = true;  // the index's binary codes are packed (else row-major: skip_transpose)
    bool gate = false;   // Lance's top-k lower-bound gate: raw_query with num_bits > 1, not approx Fast
    ApproxMode approx = ApproxMode::Normal;  // pylance's approx_mode (Fast: 1-bit scores only)
    // Lance's default (Normal) binary inner products of the full 32-row blocks come from the table
    // quantized to u8 (affine over [qmin, qmax]) and the integer sums, dequantized as
    // sum * range + segments * qmin. The remaining rows are exact. `exact_only` is Lance's fallback
    // when that affine map would overflow f32.
    std::vector<std::uint8_t> qtable;
    float range = 0.0F;
    float sum_min = 0.0F;
    bool exact_only = false;
    // Approx Accurate (Lance's hacc path): the same sums from the table quantized to u16 over
    // [qmin, qmax], dequantized as sum * (qmax - qmin) / 65535 + segments * qmin.
    std::vector<std::uint16_t> qtable16;
    float range16 = 0.0F;
};

RqQuery rq_prepare(const IvfIndex& index, const float* key, ApproxMode approx) {
    const std::size_t dim = index.dim;
    std::vector<float> rotated(dim);
    rq_rotate(key, dim, index.rq_signs.data(), rotated.data());
    RqQuery q;
    q.packed = index.rq_packed;
    q.approx = approx;
    q.gate = index.rq_bits > 1U && approx != ApproxMode::Fast;
    q.rotated = rotated;
    q.dist_table.assign(dim * 4U, 0.0F);
    for (std::size_t seg = 0; seg < dim / 4U; ++seg) {
        float* table = q.dist_table.data() + seg * 16U;
        table[0] = 0.0F;
        for (std::size_t j = 1; j < 16U; ++j) {
            const std::size_t low = j & (~j + 1U);  // lowbit(j)
            std::size_t bit = 0;
            while (((low >> bit) & 1U) == 0U) {
                ++bit;
            }
            table[j] = table[j - low] + rotated[4U * seg + bit];
        }
    }
    q.ex_query.assign((dim + 63U) / 64U * 64U, 0.0F);
    std::copy(rotated.begin(), rotated.end(), q.ex_query.begin());
    for (const float x : rotated) {
        q.sum_q += x;
    }
    // quantize_dist_table_into (dist_table_quant.rs): min and max over the whole table.
    const auto segments = static_cast<float>(dim / 4U);
    const float qmin = *std::min_element(q.dist_table.begin(), q.dist_table.end());
    const float qmax = *std::max_element(q.dist_table.begin(), q.dist_table.end());
    if (!std::isfinite(segments * qmin) || !std::isfinite(segments * qmax) ||
        !std::isfinite(segments * (qmax - qmin))) {
        q.exact_only = true;
        return q;
    }
    if (approx == ApproxMode::Accurate) {
        // quantize_dist_table_u16_into: the same affine map over 16 bits.
        q.qtable16.assign(q.dist_table.size(), 0U);
        const float factor16 = 65535.0F / (qmax - qmin);
        if (std::isfinite(factor16)) {
            for (std::size_t i = 0; i < q.dist_table.size(); ++i) {
                q.qtable16[i] = static_cast<std::uint16_t>(std::nearbyint((q.dist_table[i] - qmin) * factor16));
            }
        }
        q.range16 = (qmax - qmin) / 65535.0F;
        q.sum_min = segments * qmin;
        return q;
    }
    q.qtable.assign(q.dist_table.size(), 0U);
    const float factor = 255.0F / (qmax - qmin);
    if (std::isfinite(factor)) {
        for (std::size_t i = 0; i < q.dist_table.size(); ++i) {
            q.qtable[i] = static_cast<std::uint8_t>(std::nearbyint((q.dist_table[i] - qmin) * factor));
        }
    }
    q.range = (qmax - qmin) / 255.0F;
    q.sum_min = segments * qmin;
    return q;
}

/// One row's 1-bit code, as sequential bytes (bit d of the code is byte d / 8, bit d % 8), read from
/// Lance's packed layout: 32 rows per block of code_len 32-byte columns in PERM0 order, the remaining
/// rows transposed after them.
void rq_row_codes(const std::uint8_t* packed, std::size_t n, std::size_t code_len, std::size_t id,
                  std::uint8_t* codes) {
    static constexpr std::size_t kPerm0Inverse[16] = {0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15};
    const std::size_t rem = n % 32U;
    const std::size_t full = n - rem;
    if (id < full) {
        // The nibble pair of one column byte: lower nibble = segment 2c, upper = segment 2c + 1.
        const std::size_t base = (id / 32U) * 32U * code_len;
        const std::size_t in_batch = id % 32U;
        const std::size_t idx = kPerm0Inverse[in_batch % 16U];
        const bool lower = in_batch < 16U;
        for (std::size_t col = 0; col < code_len; ++col) {
            const std::uint8_t* block = packed + base + col * 32U;
            codes[col] = lower ? static_cast<std::uint8_t>((block[idx] & 0x0FU) |
                                                           static_cast<std::uint8_t>(block[idx + 16U] << 4U))
                               : static_cast<std::uint8_t>((block[idx] >> 4U) | (block[idx + 16U] & 0xF0U));
        }
        return;
    }
    const std::size_t off = id - full;
    const std::uint8_t* rest = packed + full * code_len;
    for (std::size_t col = 0; col < code_len; ++col) {
        codes[col] = rest[col * rem + off];
    }
}

/// The row's binary inner product with the query: the exact sum of the dist table (Lance's
/// compute_single_rq_distance) for the remainder rows and the exact fallback, else the dequantized
/// sum of the u8 table (binary_distances_with_scratch; the u16 table for approx Accurate, hacc).
float rq_binary_ip(const RqQuery& q, const std::uint8_t* packed, std::size_t n, std::size_t code_len,
                   std::size_t id) {
    std::uint8_t codes_buf[256];
    const std::uint8_t* codes = codes_buf;
    if (q.packed) {
        rq_row_codes(packed, n, code_len, id, codes_buf);
    } else {
        codes = packed + id * code_len;  // row-major (skip_transpose): Lance packs these at load
    }
    const bool simd_row = !q.exact_only && id < n - n % 32U;
    if (simd_row && q.approx == ApproxMode::Accurate) {
        std::uint32_t sum = 0;
        for (std::size_t col = 0; col < code_len; ++col) {
            sum += q.qtable16[(2U * col) * 16U + (codes[col] & 0x0FU)] +
                   q.qtable16[(2U * col + 1U) * 16U + (codes[col] >> 4U)];
        }
        return static_cast<float>(sum) * q.range16 + q.sum_min;
    }
    if (simd_row) {
        std::uint32_t sum = 0;
        for (std::size_t col = 0; col < code_len; ++col) {
            sum += q.qtable[(2U * col) * 16U + (codes[col] & 0x0FU)] +
                   q.qtable[(2U * col + 1U) * 16U + (codes[col] >> 4U)];
        }
        return static_cast<float>(sum) * q.range + q.sum_min;
    }
    float dist = 0.0F;
    for (std::size_t col = 0; col < code_len; ++col) {
        dist += q.dist_table[(2U * col) * 16U + (codes[col] & 0x0FU)] +
                q.dist_table[(2U * col + 1U) * 16U + (codes[col] >> 4U)];
    }
    return dist;
}

/// Lance's blocked extended codes, unpacked (ex_dot.rs unpack_group): one group of dims per call, the
/// group's codes at `c`. Groups of 16 dims for 1, 4 and 8 bits, 64 dims otherwise.
void rq_unpack_group(unsigned bits, const std::uint8_t* c, std::uint8_t* out) {
    const auto plane_at = [](const std::uint8_t* p) {
        std::uint64_t v = 0;
        for (int i = 7; i >= 0; --i) {
            v = (v << 8U) | p[i];
        }
        return v;
    };
    // The top bit of dimension 16k + b, in the top-bit plane at `plane` (bit 8 * (b % 8) + 2k + b / 8).
    const auto top_bit = [](std::uint64_t plane, std::size_t k, std::size_t b) {
        return static_cast<std::uint8_t>((plane >> (8U * (b % 8U) + 2U * k + b / 8U)) & 1U);
    };
    switch (bits) {
        case 1:
            for (std::size_t i = 0; i < 16; ++i) {
                out[i] = static_cast<std::uint8_t>((c[i / 8U] >> (i % 8U)) & 1U);
            }
            break;
        case 2:
            for (std::size_t k = 0; k < 4; ++k) {
                for (std::size_t b = 0; b < 16; ++b) {
                    out[16U * k + b] = static_cast<std::uint8_t>((c[b] >> (2U * k)) & 3U);
                }
            }
            break;
        case 3: {
            const std::uint64_t plane = plane_at(c + 16);
            for (std::size_t k = 0; k < 4; ++k) {
                for (std::size_t b = 0; b < 16; ++b) {
                    out[16U * k + b] = static_cast<std::uint8_t>(((c[b] >> (2U * k)) & 3U) | (top_bit(plane, k, b) << 2U));
                }
            }
            break;
        }
        case 4:
            for (std::size_t b = 0; b < 8; ++b) {
                out[b] = c[b] & 0x0FU;
                out[8U + b] = static_cast<std::uint8_t>(c[b] >> 4U);
            }
            break;
        case 5: {
            const std::uint64_t plane = plane_at(c + 32);
            for (std::size_t k = 0; k < 4; ++k) {
                for (std::size_t b = 0; b < 16; ++b) {
                    const auto nibble = static_cast<std::uint8_t>((c[16U * (k / 2U) + b] >> (4U * (k % 2U))) & 0x0FU);
                    out[16U * k + b] = static_cast<std::uint8_t>(nibble | (top_bit(plane, k, b) << 4U));
                }
            }
            break;
        }
        case 6:
        case 7: {
            for (std::size_t k = 0; k < 3; ++k) {
                for (std::size_t b = 0; b < 16; ++b) {
                    out[16U * k + b] = c[16U * k + b] & 0x3FU;
                }
            }
            for (std::size_t b = 0; b < 16; ++b) {
                out[48U + b] = static_cast<std::uint8_t>((c[b] >> 6U) | ((c[16U + b] >> 6U) << 2U) |
                                                         ((c[32U + b] >> 6U) << 4U));
            }
            if (bits == 7) {
                const std::uint64_t plane = plane_at(c + 48);
                for (std::size_t k = 0; k < 4; ++k) {
                    for (std::size_t b = 0; b < 16; ++b) {
                        out[16U * k + b] |= static_cast<std::uint8_t>(top_bit(plane, k, b) << 6U);
                    }
                }
            }
            break;
        }
        default:  // 8
            for (std::size_t i = 0; i < 16; ++i) {
                out[i] = c[i];
            }
            break;
    }
}

/// One row's extended codes, as one value per dimension (`padded` of them, a multiple of 64).
void rq_unpack_ex(unsigned bits, const std::uint8_t* row, std::size_t padded, std::uint8_t* out) {
    const std::size_t block_bytes = 64U * bits / 8U;
    for (std::size_t base = 0; base < padded; base += 64U) {
        const std::uint8_t* c = row + base / 64U * block_bytes;
        if (bits == 1 || bits == 4 || bits == 8) {
            for (std::size_t g = 0; g < 4U; ++g) {
                rq_unpack_group(bits, c + g * 2U * bits, out + base + 16U * g);
            }
        } else {
            rq_unpack_group(bits, c, out + base);
        }
    }
}

/// Lance's raw-query distance of one row (RabitDistCalculator::raw_query_multi_bit_exact_distance for
/// num_bits > 1, raw_query_binary_distance for num_bits 1), L2 terms: the binary inner product, the
/// extended dot and the factors of the row. `ex_values` is scratch of `q.ex_query.size()` bytes.
float rq_distance(const IvfIndex& index, const RqQuery& q, const Partition& part, std::size_t id,
                  float query_factor, float binary_ip, std::uint8_t* ex_values) {
    if (index.rq_bits <= 1U || q.approx == ApproxMode::Fast) {
        return (binary_ip - 0.5F * q.sum_q) * part.rq_scale[id] + part.rq_add[id] + query_factor;
    }
    const unsigned ex_bits = index.rq_bits - 1U;
    rq_unpack_ex(ex_bits, part.rq_ex.data() + id * index.rq_ex_len, q.ex_query.size(), ex_values);
    // sum_d query[d] * ex[d]
    float ex_dot = 0.0F;
    for (std::size_t d = 0; d < q.ex_query.size(); ++d) {
        ex_dot += q.ex_query[d] * static_cast<float>(ex_values[d]);
    }
    const auto levels = static_cast<float>(1U << ex_bits);
    const float code_bias = -(levels - 0.5F);
    const float full_dot = levels * binary_ip + ex_dot + code_bias * q.sum_q;
    return full_dot * part.rq_ex_scale[id] + part.rq_ex_add[id] + query_factor;
}

/// Lance's raw-query terms of one partition: the query factor and the query error (raw_query_factor,
/// raw_query_error). L2: the squared distance to the centroid. Cosine: that distance, with no -1 (measured:
/// pylance's cosine distances are one more than Lance's raw_query_factor Cosine branch gives). Dot: the
/// rotated query against the rotated centroid (`rotated_centroid`), which Lance's search always has.
void rq_query_terms(const IvfIndex& index, const RqQuery& q, float dist_q_c, const float* rotated_centroid,
                    float& factor, float& error) {
    const std::size_t dim = index.dim;
    if (index.metric == VectorMetric::Dot) {
        factor = -dot(q.rotated.data(), rotated_centroid, dim);
        error = std::sqrt(l2(q.rotated.data(), rotated_centroid, dim));
        return;
    }
    factor = dist_q_c;
    error = dist_q_c > 0.0F ? std::sqrt(dist_q_c) : 0.0F;
}

/// One IVF_RQ partition's distances to the query. `query_factor` as rq_query_terms gives it.
void rq_distances(const IvfIndex& index, const RqQuery& q, const Partition& part, float query_factor,
                  std::vector<float>& d) {
    const std::size_t n = part.ids.size();
    d.resize(n);
    const std::size_t code_len = index.dim / 8U;
    std::vector<std::uint8_t> ex_values(q.ex_query.size());
    for (std::size_t r = 0; r < n; ++r) {
        d[r] = rq_distance(index, q, part, r, query_factor, rq_binary_ip(q, part.codes.data(), n, code_len, r),
                           ex_values.data());
    }
}

// ── IVF_HNSW_SQ ───────────────────────────────────────────────────────────────────────────────

/// Rust's std::collections::BinaryHeap (a max-heap) over `T`, compared by `less` alone: pushes,
/// pops and the final sort move elements exactly as Rust's do, so equal distances come out in
/// Lance's order.
template <typename T, typename Less>
class RustHeap {
public:
    explicit RustHeap(Less less) : less_(less) {}
    std::size_t size() const { return d_.size(); }
    bool empty() const { return d_.empty(); }
    const T& peek() const { return d_.front(); }
    void push(T x) {
        d_.push_back(std::move(x));
        sift_up(0, d_.size() - 1U);
    }
    T pop() {
        T item = std::move(d_.back());
        d_.pop_back();
        if (!d_.empty()) {
            std::swap(item, d_.front());
            sift_down_to_bottom(0);
        }
        return item;
    }
    /// Ascending, as into_sorted_vec.
    std::vector<T> into_sorted() && {
        std::size_t end = d_.size();
        while (end > 1U) {
            --end;
            std::swap(d_[0], d_[end]);
            sift_down_range(0, end);
        }
        return std::move(d_);
    }

private:
    bool le(const T& a, const T& b) const { return !less_(b, a); }
    std::size_t sift_up(std::size_t start, std::size_t pos) {
        T el = std::move(d_[pos]);
        while (pos > start) {
            const std::size_t parent = (pos - 1U) / 2U;
            if (le(el, d_[parent])) {
                break;
            }
            d_[pos] = std::move(d_[parent]);
            pos = parent;
        }
        d_[pos] = std::move(el);
        return pos;
    }
    void sift_down_range(std::size_t pos, std::size_t end) {
        T el = std::move(d_[pos]);
        std::size_t child = 2U * pos + 1U;
        while (end >= 2U && child <= end - 2U) {
            if (le(d_[child], d_[child + 1U])) {
                ++child;
            }
            if (!less_(el, d_[child])) {  // el >= child
                d_[pos] = std::move(el);
                return;
            }
            d_[pos] = std::move(d_[child]);
            pos = child;
            child = 2U * pos + 1U;
        }
        if (child + 1U == end && less_(el, d_[child])) {
            d_[pos] = std::move(d_[child]);
            pos = child;
        }
        d_[pos] = std::move(el);
    }
    void sift_down_to_bottom(std::size_t pos) {
        const std::size_t end = d_.size();
        const std::size_t start = pos;
        T el = std::move(d_[pos]);
        std::size_t child = 2U * pos + 1U;
        while (end >= 2U && child <= end - 2U) {
            if (le(d_[child], d_[child + 1U])) {
                ++child;
            }
            d_[pos] = std::move(d_[child]);
            pos = child;
            child = 2U * pos + 1U;
        }
        if (child + 1U == end) {
            d_[pos] = std::move(d_[child]);
            pos = child;
        }
        d_[pos] = std::move(el);
        sift_up(start, pos);
    }

    std::vector<T> d_;
    Less less_;
};

struct HnswNode {
    float dist;
    std::uint32_t id;
};

/// OrderedFloat's total order (NaN greatest).
bool dist_less(float a, float b) {
    const bool an = std::isnan(a);
    const bool bn = std::isnan(b);
    if (an || bn) {
        return !an && bn;
    }
    return a < b;
}

/// The SQ codes of `v` over [start, end] (lance-index scale_to_u8): (v - start) * 255 / range in f64,
/// cast to u8 with Rust's saturation (NaN to 0).
void sq_codes(const float* v, std::size_t dim, double start, double end, std::uint8_t* out) {
    if (start == end) {
        std::fill(out, out + dim, std::uint8_t{0});
        return;
    }
    const double range = end - start;
    for (std::size_t j = 0; j < dim; ++j) {
        const double x = (static_cast<double>(v[j]) - start) * 255.0 / range;
        out[j] = std::isnan(x) || x <= 0.0 ? 0 : (x >= 255.0 ? 255 : static_cast<std::uint8_t>(x));
    }
}

/// Distances to one partition's SQ codes, as Lance's SQDistCalculator computes them.
struct SqDistance {
    const IvfIndex* index = nullptr;
    const Partition* part = nullptr;
    std::vector<std::uint8_t> query_code;  // L2 / cosine
    std::vector<float> query;              // dot
    float query_sum = 0;
    float scale = 0;
    float lower = 0;
    float value_scale = 0;

    SqDistance(const IvfIndex& idx, const Partition& p, const std::vector<float>& key) : index(&idx), part(&p) {
        value_scale = static_cast<float>(idx.sq_end - idx.sq_start) / 255.0F;
        scale = value_scale * value_scale;
        lower = static_cast<float>(idx.sq_start);
        if (idx.metric == VectorMetric::Dot) {
            query = key;
            for (const float x : key) {
                query_sum += x;
            }
        } else {
            query_code.resize(idx.dim);
            sq_codes(key.data(), idx.dim, idx.sq_start, idx.sq_end, query_code.data());
        }
    }
    float operator()(std::uint32_t id) const {
        const std::size_t dim = index->dim;
        const std::uint8_t* c = part->codes.data() + static_cast<std::size_t>(id) * dim;
        if (index->metric == VectorMetric::Dot) {
            float acc = 0;
            for (std::size_t j = 0; j < dim; ++j) {
                acc += static_cast<float>(c[j]) * query[j];
            }
            return 1.0F - (lower * query_sum + value_scale * acc);
        }
        // Summed in 32-bit chunks (255^2 * 65536 < 2^32), which compilers vectorize.
        std::uint64_t sum = 0;
        for (std::size_t at = 0; at < dim; at += 65536U) {
            std::uint32_t part = 0;
            for (std::size_t j = at; j < std::min<std::size_t>(dim, at + 65536U); ++j) {
                const int d = static_cast<int>(c[j]) - static_cast<int>(query_code[j]);
                part += static_cast<std::uint32_t>(d * d);
            }
            sum += part;
        }
        return static_cast<float>(sum) * scale;
    }
};

/// Distances to one partition's float rows (IVF_HNSW_FLAT), as the exhaustive scan computes them.
struct FloatDistance {
    const IvfIndex* index = nullptr;
    const Partition* part = nullptr;
    const std::vector<float>* key = nullptr;
    float key_norm = 1.0F;  // |key|: 1 unless a float16 column's normalized query was rounded again
    float operator()(std::uint32_t id) const {
        return distance(index->metric, part->vectors.data() + static_cast<std::size_t>(id) * index->dim, key->data(),
                        index->dim, key_norm);
    }
};

/// Distances to one partition's PQ codes (IVF_HNSW_PQ): the sum of the query's table entries, in
/// sub-vector order, as the exhaustive scan sums them. `table` is pq_table's for this partition.
struct PqDistance {
    const IvfIndex* index = nullptr;
    const Partition* part = nullptr;
    std::vector<float> table;
    float bias = 0.0F;  // dot: 1
    float operator()(std::uint32_t id) const {
        const std::size_t n = part->ids.size();
        const std::size_t nc = std::size_t{1} << index->nbits;
        float acc = bias;
        for (std::size_t s = 0; s < index->m; ++s) {
            std::size_t c = 0;
            if (index->nbits == 8U) {
                c = part->codes[s * n + id];
            } else {  // two 4-bit codes per byte: sub-vectors 2j (low) and 2j + 1 (high)
                const std::uint8_t byte = part->codes[(s / 2U) * n + id];
                c = s % 2U == 0U ? (byte & 0x0FU) : (byte >> 4U);
            }
            acc += table[s * nc + c];
        }
        return acc;
    }
};

/// One partition's HNSW search (lance-index HNSW::search): greedy descent to level 1, a beam of
/// `ef` at level 0, the best `k`. `accept` filters results (a prefilter, a distance range); null
/// accepts all.
template <typename Dist>
std::vector<HnswNode> hnsw_search(const Partition& part, const Dist& dist, std::size_t k, std::size_t ef,
                                  const std::function<bool(std::uint32_t, float)>* accept) {
    const std::size_t n = part.ids.size();
    if (n == 0U) {
        return {};
    }
    HnswNode ep{dist(part.entry_point), part.entry_point};
    for (std::size_t level = part.max_level; level-- > 1U;) {
        std::uint32_t current = ep.id;
        float closest = ep.dist;
        for (;;) {
            std::optional<std::uint32_t> next;
            std::size_t count = 0;
            const std::uint32_t* nb = part.neighbors(level, current, count);
            for (std::size_t i = 0; i < count; ++i) {
                const float d = dist(nb[i]);
                if (dist_less(d, closest)) {
                    closest = d;
                    next = nb[i];
                }
            }
            if (!next) {
                break;
            }
            current = *next;
        }
        ep = HnswNode{closest, current};
    }
    const auto by_dist = [](const HnswNode& a, const HnswNode& b) { return dist_less(a.dist, b.dist); };
    const auto reverse = [](const HnswNode& a, const HnswNode& b) { return dist_less(b.dist, a.dist); };
    RustHeap<HnswNode, decltype(reverse)> candidates(reverse);
    RustHeap<HnswNode, decltype(by_dist)> results(by_dist);
    std::vector<std::uint8_t> visited(n, 0U);
    visited[ep.id] = 1U;
    candidates.push(ep);
    if (accept == nullptr || (*accept)(ep.id, ep.dist)) {
        results.push(ep);
    }
    while (!candidates.empty()) {
        const HnswNode current = candidates.pop();
        const float furthest = results.empty() ? std::numeric_limits<float>::infinity() : results.peek().dist;
        if (dist_less(furthest, current.dist) && results.size() == ef) {
            break;
        }
        std::size_t count = 0;
        const std::uint32_t* nb = part.neighbors(0, current.id, count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint32_t id = nb[i];
            if (visited[id] != 0U) {
                continue;
            }
            visited[id] = 1U;
            const float d = dist(id);
            const float far = results.empty() ? std::numeric_limits<float>::infinity() : results.peek().dist;
            if (!dist_less(far, d) || results.size() < ef) {
                if (accept == nullptr || (*accept)(id, d)) {
                    if (results.size() < ef) {
                        results.push(HnswNode{d, id});
                    } else if (dist_less(d, results.peek().dist)) {
                        results.pop();
                        results.push(HnswNode{d, id});
                    }
                }
                candidates.push(HnswNode{d, id});
            }
        }
    }
    auto sorted = std::move(results).into_sorted();
    if (sorted.size() > k) {
        sorted.resize(k);
    }
    return sorted;
}

/// A partition's prefiltered search when few rows pass (under 10%): every passing row, exactly.
template <typename Dist>
std::vector<HnswNode> hnsw_flat(const Partition& part, const Dist& dist, std::size_t k,
                                const std::vector<std::uint32_t>& passing, const NearestQuery& q) {
    const auto by_dist = [](const HnswNode& a, const HnswNode& b) { return dist_less(a.dist, b.dist); };
    RustHeap<HnswNode, decltype(by_dist)> heap(by_dist);
    for (const auto id : passing) {
        const float d = dist(id);
        if (!in_range(d, q)) {
            continue;
        }
        if (heap.size() < k) {
            heap.push(HnswNode{d, id});
        } else if (dist_less(d, heap.peek().dist)) {
            heap.pop();
            heap.push(HnswNode{d, id});
        }
    }
    (void)part;
    return std::move(heap).into_sorted();
}

/// Lance's query parallelism for an index without a global top-k heap (HNSW): its compute pool --
/// the cores less LANCE_IO_CORE_RESERVATION (2), or LANCE_CPU_THREADS -- bounded by DataFusion's
/// target partitions (the cores), and by the query's own setting.
std::size_t late_parallelism(const NearestQuery& q) {
    const auto cores = static_cast<std::size_t>(std::max(1U, std::thread::hardware_concurrency()));
    const auto env = [](const char* name, std::size_t fallback) {
        const char* raw = std::getenv(name);
        if (raw == nullptr || *raw == '\0') {
            return fallback;
        }
        char* end = nullptr;
        const unsigned long long v = std::strtoull(raw, &end, 10);
        return end != nullptr && *end == '\0' ? static_cast<std::size_t>(v) : fallback;
    };
    std::size_t compute = 0;
    if (std::getenv("LANCE_CPU_THREADS") != nullptr) {
        compute = std::max<std::size_t>(env("LANCE_CPU_THREADS", 1), 1);
    } else {
        const std::size_t reserved = env("LANCE_IO_CORE_RESERVATION", 2);
        compute = cores <= reserved ? 1U : cores - reserved;
    }
    const std::size_t pool = std::max<std::size_t>(std::min(compute, cores), 1);
    if (q.query_parallelism == -1 || q.query_parallelism == 0) {
        return pool;
    }
    return q.query_parallelism > 0 ? std::clamp<std::size_t>(static_cast<std::size_t>(q.query_parallelism), 1, pool) : 1U;
}

/// One HNSW partition's best `kk` rows (Lance's HNSW::search), with any distance: the rows that may
/// come back (the prefilter: deletions, a filter); all of them -- a plain search; under 10% -- every
/// one of them, exactly; otherwise the graph search, keeping only those.
template <typename Dist>
bool hnsw_search_partition(const Partition& part, const Dist& dist, const NearestQuery& q, const RowMask& mask,
                           std::size_t kk, std::vector<Candidate>& local, std::string& why) {
    const std::size_t n = part.ids.size();
    std::vector<std::uint32_t> passing;
    for (std::size_t r = 0; r < n; ++r) {
        if (mask.allows(part.ids[r])) {
            passing.push_back(static_cast<std::uint32_t>(r));
        }
    }
    const std::size_t ef = q.ef.value_or(kk + kk / 2U);
    if (ef < kk) {
        why = "ef must be greater than or equal to k";
        return false;
    }
    std::vector<HnswNode> found;
    const bool ranged = q.lower_bound || q.upper_bound;
    if (passing.size() == n) {
        if (ranged) {
            const std::function<bool(std::uint32_t, float)> range = [&](std::uint32_t, float x) {
                return in_range(x, q);
            };
            found = hnsw_search(part, dist, kk, ef, &range);
        } else {
            found = hnsw_search(part, dist, kk, ef, nullptr);
        }
    } else if (passing.size() < n * 10U / 100U) {
        found = hnsw_flat(part, dist, kk, passing, q);
    } else {
        std::vector<std::uint8_t> ok(n, 0U);
        for (const auto r : passing) {
            ok[r] = 1U;
        }
        const std::function<bool(std::uint32_t, float)> allowed = [&](std::uint32_t id, float x) {
            return ok[id] != 0U && in_range(x, q);
        };
        found = hnsw_search(part, dist, kk, ef, &allowed);
    }
    std::set<std::uint64_t> seen;
    for (const auto& f : found) {
        const std::uint64_t id = part.ids[f.id];
        if (seen.insert(id).second) {
            local.push_back(Candidate{f.dist, id});
        }
    }
    return true;
}

/// One HNSW partition's best `kk` rows, with the distances of its storage: PQ tables on the query
/// (the residual for L2 / cosine, as the IVF_PQ path), SQ codes, or floats.
[[gnu::noinline]] bool hnsw_partition(const IvfIndex& index, const Partition& part, std::size_t p,
                                      const NearestQuery& q, const RowMask& mask, const std::vector<float>& key,
                                      std::size_t kk, std::vector<Candidate>& local, std::string& why) {
    if (index.storage == Storage::Pq) {
        std::vector<float> table;
        const bool dot = index.metric == VectorMetric::Dot;
        if (dot) {
            pq_table(index, key.data(), table, true);
        } else {
            std::vector<float> residual(index.dim);
            const float* c = index.centroids.data() + p * index.dim;
            for (std::size_t j = 0; j < index.dim; ++j) {
                residual[j] = q.f16_column ? ArrowHalfFloatToFloat(float16_bits(key[j] - c[j])) : key[j] - c[j];
            }
            pq_table(index, residual.data(), table);
        }
        const PqDistance dist{&index, &part, std::move(table), dot ? 1.0F : 0.0F};
        return hnsw_search_partition(part, dist, q, mask, kk, local, why);
    }
    if (index.storage == Storage::Sq) {
        return hnsw_search_partition(part, SqDistance(index, part, key), q, mask, kk, local, why);
    }
    const float key_norm = index.metric == VectorMetric::Cosine && q.f16_column
                               ? std::sqrt(dot(key.data(), key.data(), index.dim))
                               : 1.0F;
    return hnsw_search_partition(part, FloatDistance{&index, &part, &key, key_norm}, q, mask, kk, local, why);
}

/// Lance's top-k scan of one IVF_RQ partition into the heap all the partitions share
/// (accumulate_raw_query_multi_bit_topk). A row whose lower bound -- its 1-bit estimate less
/// error_factor * query_error -- is at least the heap's worst distance, once the heap holds `k`
/// rows, is skipped without its extended distance. Otherwise that distance is computed and the row
/// replaces the worst one when it is smaller. `query_error` is sqrt of the squared distance to the
/// centroid (raw_query_error, L2). Upper and lower bounds are f32 max and min: nothing is cut by them.
template <typename Heap>
void rq_gate_partition(const IvfIndex& index, const RqQuery& q, const Partition& part, float query_factor,
                       float query_error, const RowMask& mask, std::size_t k, Heap& heap) {
    if (k == 0U) {
        return;
    }
    const std::size_t n = part.ids.size();
    const std::size_t code_len = index.dim / 8U;
    const float top = std::numeric_limits<float>::max();
    std::vector<std::uint8_t> ex_values(q.ex_query.size());
    for (std::size_t r = 0; r < n; ++r) {
        const std::uint64_t id = part.ids[r];
        if (!mask.allows(id)) {
            continue;
        }
        const float binary_ip = rq_binary_ip(q, part.codes.data(), n, code_len, r);
        if (q.gate) {
            const float binary = (binary_ip - 0.5F * q.sum_q) * part.rq_scale[r] + part.rq_add[r] + query_factor;
            const float lower = binary - part.rq_err[r] * query_error;
            if (lower >= top || (heap.size() >= k && lower >= heap.peek().d)) {
                continue;
            }
        }
        const float d = rq_distance(index, q, part, r, query_factor, binary_ip, ex_values.data());
        if (d < -top || d >= top) {
            continue;
        }
        if (heap.size() < k) {
            heap.push(Candidate{d, id});
        } else if (heap.peek().d > d) {
            heap.pop();
            heap.push(Candidate{d, id});
        }
    }
}

// The Hamming (binary IVF_FLAT) steps of search_segment, kept out of line so the float search loop
// compiles as it did before they existed.
[[gnu::noinline]] void hamming_to_centroids(const IvfIndex& index, const std::vector<std::uint8_t>& key,
                                            std::vector<float>& out) {
    for (std::size_t p = 0; p < index.partitions; ++p) {
        out[p] = hamming(key.data(), index.binary_centroids.data() + p * index.dim, index.dim);
    }
}

/// Each row of `part` into Lance's global top-k heap: a row replaces the heap's worst only when strictly
/// closer, so among equal distances the one met first stays.
template <typename Heap>
[[gnu::noinline]] void hamming_into_heap(const Partition& part, const std::vector<std::uint8_t>& key, std::size_t dim,
                                         const NearestQuery& q, const RowMask& mask, std::size_t kk, Heap& heap) {
    const std::size_t n = part.ids.size();
    for (std::size_t r = 0; r < n; ++r) {
        const float d = hamming(part.codes.data() + r * dim, key.data(), dim);
        if (!in_range(d, q) || !mask.allows(part.ids[r])) {
            continue;
        }
        const Candidate c{d, part.ids[r]};
        if (heap.size() < kk) {
            heap.push(c);
        } else if (heap.peek().d > d) {
            heap.pop();
            heap.push(c);
        }
    }
}

/// The best `kk` candidates of one index segment.
bool search_segment(const IvfIndex& index, const NearestQuery& q, bool f32_column, const RowMask& mask,
                    std::size_t kk, std::vector<Candidate>& out, std::string& plan, std::string& error,
                    CacheCount& count) {
    if (q.key.size() != index.dim) {
        error = "Query vector size " + std::to_string(q.key.size()) + " does not match index column size " +
                std::to_string(index.dim);
        return false;
    }
    std::vector<std::uint8_t> bkey;  // Hamming: the query's bytes (dataset_nearest checked them)
    if (index.metric == VectorMetric::Hamming) {
        for (const float x : q.key) {
            bkey.push_back(static_cast<std::uint8_t>(x));
        }
    }
    std::vector<float> key = q.key;
    if (index.metric == VectorMetric::Cosine) {
        // Lance's normalize_arrow: a sequential f32 sum of squares (not a lane-wise dot), then x / norm.
        // The order matters: the norm's last bit moves the RQ distance table across a u8 rounding step.
        float sum = 0.0F;
        for (const float x : key) {
            sum += x * x;
        }
        const float norm = std::sqrt(sum);
        for (auto& x : key) {
            x /= norm;
            if (q.f16_column) {  // pylance's normalized query is a float16 vector again (measured to 1e-7)
                x = ArrowHalfFloatToFloat(float16_bits(x));
            }
        }
    }
    const std::size_t dim = index.dim;
    // |key| for the cosine distances: 1 for a normalized float32 query; a float16 one, rounded again after
    // normalizing, is not quite of length 1 (pylance divides by its true norm).
    const float key_norm = index.metric == VectorMetric::Cosine && q.f16_column ? std::sqrt(dot(key.data(), key.data(), dim)) : 1.0F;
    const std::size_t P = index.partitions;
    std::vector<float> to_centroid(P);
    if (index.metric == VectorMetric::Hamming) {
        hamming_to_centroids(index, bkey, to_centroid);
    } else {
        for (std::size_t p = 0; p < P; ++p) {
            const float* c = index.centroids.data() + p * dim;
            to_centroid[p] = index.metric == VectorMetric::Dot ? 1.0F - dot(key.data(), c, dim) : l2(key.data(), c, dim);
        }
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

    RqQuery rq;  // IVF_RQ: the rotated query and its tables, once for every partition
    std::vector<float> rq_rotated_centroids;  // IVF_RQ dot: the centroids rotated (Lance's search cache)
    if (index.storage == Storage::Rq) {
        rq = rq_prepare(index, key.data(), q.approx_mode);
        if (index.metric == VectorMetric::Dot) {
            rq_rotated_centroids.resize(P * dim);
            for (std::size_t p = 0; p < P; ++p) {
                rq_rotate(index.centroids.data() + p * dim, dim, index.rq_signs.data(),
                          rq_rotated_centroids.data() + p * dim);
            }
        }
    }
    // IVF_RQ: partition p's query factor and error (rq_query_terms).
    const auto rq_terms = [&](std::size_t p, float& factor, float& error) {
        rq_query_terms(index, rq, to_centroid[p],
                       rq_rotated_centroids.empty() ? nullptr : rq_rotated_centroids.data() + p * dim, factor, error);
    };
    std::vector<float> dot_table;  // dot: one table for every partition
    if (index.storage == Storage::Pq && index.metric == VectorMetric::Dot) {
        pq_table(index, key.data(), dot_table);
    }
    // One partition's best `kk` rows the mask and the distance range allow.
    const auto score = [&](std::size_t p, std::vector<Candidate>& local, std::string& why) {
        std::shared_ptr<const Partition> part;
        if (!index.partition(p, part, why, count)) {
            return false;
        }
        const std::size_t n = part->ids.size();
        std::vector<float> d;
        if (index.sub == SubIndex::Hnsw) {
            return hnsw_partition(index, *part, p, q, mask, key, kk, local, why);
        }
        if (index.storage == Storage::Pq) {
            std::vector<float> own;
            if (index.metric != VectorMetric::Dot) {
                std::vector<float> residual(dim);
                const float* c = index.centroids.data() + p * dim;
                for (std::size_t j = 0; j < dim; ++j) {
                    residual[j] = q.f16_column ? ArrowHalfFloatToFloat(float16_bits(key[j] - c[j])) : key[j] - c[j];
                }
                pq_table(index, residual.data(), own);
            }
            pq_distances(index, index.metric == VectorMetric::Dot ? dot_table : own, part->codes, n, kk, d);
        } else if (index.storage == Storage::Sq) {
            const SqDistance dist(index, *part, key);
            d.resize(n);
            for (std::size_t r = 0; r < n; ++r) {
                d[r] = dist(static_cast<std::uint32_t>(r));
            }
        } else if (index.storage == Storage::Rq) {
            float factor = 0.0F;
            float error = 0.0F;
            rq_terms(p, factor, error);
            rq_distances(index, rq, *part, factor, d);
        } else {
            d.resize(n);
            for (std::size_t r = 0; r < n; ++r) {
                d[r] = distance(index.metric, part->vectors.data() + r * dim, key.data(), dim, key_norm);
            }
        }
        // Gathered on the stack, not in `local`: the parallel callers' vectors share cache lines.
        std::vector<Candidate> found;
        for (std::size_t r = 0; r < n; ++r) {
            if (in_range(d[r], q) && mask.allows(part->ids[r])) {
                found.push_back(Candidate{d[r], part->ids[r]});
            }
        }
        keep_best(found, kk);
        local.insert(local.end(), found.begin(), found.end());
        return true;
    };
    // The end of every search: the prefilter's rows the search did not reach, the best `kk`, the plan.
    const auto finish = [&](std::size_t searched, std::uint64_t found) {
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
               " (minimum " + std::to_string(first) + ") minimum_nprobes=" + std::to_string(q.minimum_nprobes) +
               " maximum_nprobes=" +
               (q.maximum_nprobes ? "Some(" + std::to_string(*q.maximum_nprobes) + ")" : std::string("None"));
        return true;
    };

    if (index.metric == VectorMetric::Hamming) {
        // Lance's global top-k heap over the probed partitions (IVF_FLAT supports it): a row replaces the
        // heap's worst only when strictly closer, so among equal distances the one met first stays.
        const auto by_dist = [](const Candidate& a, const Candidate& b) { return a.d < b.d; };
        RustHeap<Candidate, decltype(by_dist)> heap(by_dist);
        std::size_t searched = 0;
        for (std::size_t i = 0; i < most && (i < first || heap.size() < max_results); ++i, ++searched) {
            std::shared_ptr<const Partition> part;
            if (!index.partition(order[i], part, error, count)) {
                return false;
            }
            hamming_into_heap(*part, bkey, dim, q, mask, kk, heap);
        }
        std::uint64_t added = 0;
        for (const Candidate& c : std::move(heap).into_sorted()) {
            out.push_back(c);
            ++added;
        }
        return finish(searched, added);
    }
    if (index.storage == Storage::Rq) {
        // IVF_RQ searches as Lance's FLAT sub-index does under IVF's global top-k heap: every probed
        // partition in probe order into one heap of kk entries, rows gated against it. The first
        // `first` partitions always; then more, one at a time, only while fewer than k rows were found.
        const auto by_dist = [](const Candidate& a, const Candidate& b) { return a.d < b.d; };
        RustHeap<Candidate, decltype(by_dist)> heap(by_dist);
        std::size_t searched = 0;
        for (std::size_t i = 0; i < most && (i < first || heap.size() < max_results); ++i, ++searched) {
            std::shared_ptr<const Partition> part;
            if (!index.partition(order[i], part, error, count)) {
                return false;
            }
            float factor = 0.0F;
            float error = 0.0F;
            rq_terms(order[i], factor, error);
            rq_gate_partition(index, rq, *part, factor, error, mask, kk, heap);
        }
        std::uint64_t added = 0;
        for (const Candidate& c : std::move(heap).into_sorted()) {
            out.push_back(c);
            ++added;
        }
        return finish(searched, added);
    }
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
    // Then, while fewer than k rows were found, more partitions in order: one at a time (Lance's
    // global top-k path, IVF_FLAT / IVF_PQ), or for HNSW `query_parallelism` in flight -- Lance pulls
    // that many before the first finishes, and one more each time a finished one leaves it short.
    std::size_t searched = early.size();
    // Fewer rows pass the prefilter than k: Lance searches no further (the rest come back below).
    const bool few = mask.has_count && mask.count <= q.k && found < mask.count;
    if (!few && found < max_results && searched < most) {
        const std::size_t remaining = most - searched;
        std::size_t pulled = index.sub == SubIndex::Hnsw ?std::min(remaining, late_parallelism(q)) : 1U;
        for (std::size_t done = 0; done < pulled; ++done) {
            std::vector<Candidate> local;
            if (!score(order[searched + done], local, error)) {
                return false;
            }
            found += local.size();
            out.insert(out.end(), local.begin(), local.end());
            if (found < max_results && pulled < remaining) {
                ++pulled;
            }
        }
        searched += pulled;
    }
    return finish(searched, found);
}

// ── exact distances ─────────────────────────────────────────────────────────────────────────────

/// The top-level column a scan must read for vector column `column` -- `column` itself, or the struct
/// holding it for a nested path `a.b.c` -- and, once `schema` is read, the child path down to it.
std::string vector_root(const std::string& column) {
    return column.substr(0, column.find('.'));
}

bool vector_sub_path(const ArrowSchema& schema, const std::string& column, int& root, std::vector<int>& path,
                     std::string& error) {
    path.clear();
    root = child_index(schema, vector_root(column));
    const ArrowSchema* at = root >= 0 ? schema.children[root] : nullptr;
    std::size_t start = column.find('.');
    while (at != nullptr && start != std::string::npos) {
        const std::size_t end = column.find('.', start + 1);
        const std::string name = column.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
        const int i = child_index(*at, name);
        at = i >= 0 ? at->children[i] : nullptr;
        path.push_back(i);
        start = end;
    }
    if (at == nullptr) {
        error = "vector column '" + column + "' not read";
        return false;
    }
    return true;
}

/// Exact distances of `rows`' vectors (null vectors dropped).
float multivec_distance(const MultiRows& v, std::size_t r, const NearestQuery& q, VectorMetric metric,
                        const std::vector<std::uint8_t>* binary_key, std::size_t dim);

bool rescore(const std::filesystem::path& path, const NearestQuery& q, VectorMetric metric, std::size_t dim,
             std::vector<Candidate>& rows, std::string& error, bool multi = false,
             const std::vector<std::uint8_t>* binary_key = nullptr) {
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
    const std::vector<std::string> columns = {vector_root(q.column)};
    LanceScanRequest request;
    request.row_mask = q.row_mask;
    request.columns = &columns;
    request.has_version = q.has_version;
    request.version = q.version;
    request.with_row_address = true;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!addrs.empty() && !lance_dataset_take_rows(path, request, addrs, schema, batches, error)) {
        return false;
    }
    if (multi) {
        // A multivector column: each candidate's exact multivec_distance over all the query's vectors.
        MultiRows v;
        v.dim = dim;
        int vc = -1;
        std::vector<int> sub;
        const int ic = child_index(schema, "_rowaddr");
        bool ok = addrs.empty() || (vector_sub_path(schema, q.column, vc, sub, error) && ic >= 0);
        for (std::size_t b = 0; ok && b < batches.size(); ++b) {
            ok = collect_multivectors(schema, batches[b], vc, ic, v, error, sub);
        }
        for (auto& b : batches) {
            b.release(&b);
        }
        if (schema.release != nullptr) {
            schema.release(&schema);
        }
        if (!ok) {
            return false;
        }
        std::vector<Candidate> out;
        for (std::size_t r = 0; r < v.ids.size(); ++r) {
            const float d = multivec_distance(v, r, q, metric, nullptr, dim);
            if (!std::isnan(d)) {
                out.push_back(Candidate{d, v.ids[r]});
            }
        }
        for (const auto& [id, d] : keep) {
            out.push_back(Candidate{d, id});
        }
        rows = std::move(out);
        return true;
    }
    VectorRows v;
    v.dim = dim;
    int vc = -1;
    std::vector<int> sub;
    bool ok = addrs.empty() || (vector_sub_path(schema, q.column, vc, sub, error) &&
                                collect_vectors(schema, batches, vc, child_index(schema, "_rowaddr"), v, error, sub));
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
            const float d = binary_key != nullptr
                                ? hamming(v.bytes.data() + r * dim, binary_key->data(), dim)
                                : distance(metric, v.values.data() + r * dim, q.key.data(), dim, q_norm);
            out.push_back(Candidate{d, v.ids[r]});
        }
    }
    for (const auto& [id, d] : keep) {
        out.push_back(Candidate{d, id});
    }
    rows = std::move(out);
    return true;
}

/// Row `r` of a multivector column against the query's vectors (Lance's multivec_distance): for each
/// query vector, the least distance to a vector of the row, summed. A null or empty row is NaN. The
/// query's uint8 values are `binary_key` (Hamming distance) when given.
float multivec_distance(const MultiRows& v, std::size_t r, const NearestQuery& q, VectorMetric metric,
                        const std::vector<std::uint8_t>* binary_key, std::size_t dim) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (v.count[r] == 0U) {
        return nan;
    }
    float total = 0;
    for (std::size_t qi = 0; qi < q.key_vectors; ++qi) {
        // the least distance, NaN only when every distance is NaN (Lance's min_by total_cmp order)
        float best = nan;
        const float* qv = binary_key != nullptr ? nullptr : q.key.data() + qi * dim;
        const float q_norm = qv != nullptr && metric == VectorMetric::Cosine ? std::sqrt(dot(qv, qv, dim)) : 0.0F;
        for (std::size_t j = 0; j < v.count[r]; ++j) {
            const std::size_t at = (v.first[r] + j) * dim;
            float d = 0;
            if (binary_key != nullptr) {
                d = hamming(v.bytes.data() + at, binary_key->data() + qi * dim, dim);
            } else {
                d = distance(metric, v.values.data() + at, qv, dim, q_norm);
            }
            if (std::isnan(best) || (!std::isnan(d) && d < best)) {
                best = d;
            }
        }
        total += best;
    }
    return total;
}

/// The best k of `fragments` by exact distance; `binary_key` (a uint8 column's query): by Hamming distance.
/// `multi`: a multivector column, searched by multivec_distance.
bool flat_search(const std::filesystem::path& path, const NearestQuery& q, VectorMetric metric, std::size_t dim,
                 const std::vector<std::uint64_t>& fragments, std::vector<Candidate>& out, std::string& error,
                 const std::vector<std::uint8_t>* binary_key = nullptr, bool multi = false) {
    if (fragments.empty()) {
        return true;
    }
    const std::vector<std::string> columns = {vector_root(q.column)};
    LanceScanRequest request;
    request.row_mask = q.row_mask;
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
    int vc = -1;
    std::vector<int> sub;
    const int ic = child_index(schema, "_rowaddr");
    bool ok = vector_sub_path(schema, q.column, vc, sub, error) && ic >= 0;
    if (!ok && error.empty()) {
        error = "vector column '" + q.column + "' not read";
    }
    std::vector<Candidate> found;
    for (auto& batch : batches) {
        if (ok && multi) {
            MultiRows v;
            v.dim = dim;
            ok = collect_multivectors(schema, batch, vc, ic, v, error, sub);
            for (std::size_t r = 0; ok && r < v.ids.size(); ++r) {
                // a null or empty row (a NaN distance) is left out, as Lance's KNN leaves it out
                const float d = multivec_distance(v, r, q, metric, binary_key, dim);
                if (!std::isnan(d) && in_range(d, q)) {
                    found.push_back(Candidate{d, v.ids[r]});
                }
            }
            if (found.size() > 4U * q.k + 1024U) {
                keep_best(found, q.k);
            }
        } else if (ok) {
            VectorRows v;
            v.dim = dim;
            std::vector<ArrowArray> one;  // collect_vectors takes a list; reuse without copying ownership
            one.push_back(batch);
            ok = collect_vectors(schema, one, vc, ic, v, error, sub);
            for (std::size_t r = 0; ok && r < v.ids.size(); ++r) {
                if (v.valid[r] == 0U) {
                    continue;
                }
                const float d = binary_key != nullptr
                                    ? hamming(v.bytes.data() + r * dim, binary_key->data(), dim)
                                    : distance(metric, v.values.data() + r * dim, q.key.data(), dim, q_norm);
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
    request.row_mask = q.row_mask;
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

namespace rq_build {

void rotate(const float* in, std::size_t dim, const std::uint8_t* signs, float* out) {
    rq_rotate(in, dim, signs, out);
}

}  // namespace rq_build

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
    out.binary_centroids = index->binary_centroids;
    out.nbits = index->storage == Storage::Rq ? index->rq_bits : index->nbits;
    out.rq_signs = index->storage == Storage::Rq ? index->rq_signs : std::vector<std::uint8_t>{};
    out.m = index->storage == Storage::Pq ? index->m : 0U;
    out.codebook = index->storage == Storage::Pq ? index->codebook : std::vector<float>{};
    out.lengths = index->lengths;
    out.sq_start = index->sq_start;
    out.sq_end = index->sq_end;
    out.hnsw_m = index->hnsw_m;
    out.hnsw_ef_construction = index->hnsw_ef_construction;
    out.hnsw_max_level = index->hnsw_max_level;
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

bool vector_index_runtime_hints(const std::vector<std::uint8_t>& details,
                                std::vector<std::pair<std::string, std::string>>& out) {
    out.clear();
    const auto* p = details.data();
    const auto* end = p + details.size();
    Field f;
    while (p < end) {
        if (!next_field(p, end, f)) {
            return false;
        }
        if (f.number != 9 || f.wire != 2) {  // runtime_hints: a map entry {key = 1, value = 2}
            continue;
        }
        std::string key;
        std::string value;
        const auto* q = f.data;
        const auto* entry_end = f.data + f.size;
        Field g;
        while (q < entry_end) {
            if (!next_field(q, entry_end, g)) {
                return false;
            }
            if (g.wire == 2 && g.number == 1) {
                key.assign(reinterpret_cast<const char*>(g.data), g.size);
            } else if (g.wire == 2 && g.number == 2) {
                value.assign(reinterpret_cast<const char*>(g.data), g.size);
            }
        }
        out.emplace_back(std::move(key), std::move(value));
    }
    return true;
}

bool vector_index_details(const std::filesystem::path& index_dir, VectorIndexDetails& out, std::string& error) {
    std::shared_ptr<const IvfIndex> index;
    if (!load_ivf(index_dir, index, error)) {
        return false;
    }
    out.type = index->type;
    out.metric = vector_metric_name(index->metric);
    out.pq = index->storage == Storage::Pq;
    out.num_bits = index->nbits;
    out.num_sub_vectors = index->m;
    return true;
}

/// Lance's XTR search of a multivector query over an IVF_FLAT index (`multivec_ann` and
/// `MultivectorScoringExec`). For each query vector: the index's best `kk` entries, sorted by (distance,
/// row id), each row's first (best) entry kept; its similarity is 1 - distance. A row's similarity is
/// summed over the query vectors in turn: a row missing from a list adds that list's `min_sim` (the
/// similarity of its last kept entry), a row new to the sum starts at its similarity plus the earlier
/// lists' `min_sim`s. The distance is the query vector count minus the sum; `keep` rows are kept.
bool xtr_search(const NearestQuery& q, const std::vector<std::shared_ptr<const IvfIndex>>& loaded,
                const std::vector<RowMask>& masks, RowIdToAddress& resolver, std::size_t dim, bool f32_column,
                std::uint32_t overfetch, std::size_t keep, std::vector<Candidate>& out,
                std::vector<std::string>& plan, std::string& error, CacheCount& counts) {
    const std::size_t queries = q.key_vectors;
    const std::size_t kk = static_cast<std::size_t>(q.k) * overfetch;  // the candidates per query vector
    std::unordered_map<std::uint64_t, float> sums;
    float missed = 0.0F;  // the sum of the earlier lists' min_sim
    for (std::size_t qi = 0; qi < queries; ++qi) {
        NearestQuery one = q;
        one.key.assign(q.key.begin() + static_cast<std::ptrdiff_t>(qi * dim),
                       q.key.begin() + static_cast<std::ptrdiff_t>((qi + 1U) * dim));
        one.key_vectors = 1;
        // Lance's per-vector query carries refine_factor = overfetch (its probing is then the legacy rule,
        // not the adaptive one) and keeps k * overfetch candidates.
        one.refine_factor = overfetch;
        std::vector<Candidate> list;
        for (std::size_t s = 0; s < loaded.size(); ++s) {
            std::string line;
            if (!search_segment(*loaded[s], one, f32_column, masks[s], kk, list, line, error, counts)) {
                return false;
            }
            if (qi == 0) {
                plan.push_back("  " + line);
            }
        }
        if (resolver.stable()) {  // as the exact search: the candidates are addresses from here on
            for (auto& hit : list) {
                resolver.find(hit.id, hit.id);
            }
        }
        keep_best(list, kk);
        const float min_sim = list.empty() ? 0.0F : 1.0F - list.back().d;
        std::unordered_map<std::uint64_t, float> first;  // the best entry of each row in this list
        for (const auto& c : list) {
            first.emplace(c.id, 1.0F - c.d);
        }
        for (auto& [id, sum] : sums) {
            const auto it = first.find(id);
            sum += it != first.end() ? it->second : min_sim;
        }
        for (const auto& [id, sim] : first) {
            sums.emplace(id, sim + missed);  // emplace: a row already summed keeps its sum
        }
        missed += min_sim;
    }
    out.clear();
    for (const auto& [id, sum] : sums) {
        out.push_back(Candidate{static_cast<float>(queries) - sum, id});
    }
    keep_best(out, keep);
    return true;
}

bool dataset_nearest(const std::filesystem::path& dataset_path, const NearestQuery& query, NearestResult& out,
                     std::string& error) {
    out = NearestResult{};
    error.clear();
    NearestQuery q = query;  // f16_column is set below, from the column's type
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
    bool binary = false;  // a uint8 column: binary vectors, searched by Hamming distance
    bool multi = false;   // a list of fixed-size lists: a multivector column (multivec_distance)
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
        // A multivector column is a list of fixed-size lists: the vectors are the inner lists'.
        const ArrowSchema* vectors = column;
        if (column != nullptr && column->format != nullptr && std::string(column->format) == "+l" &&
            column->n_children == 1 && column->children[0]->format != nullptr &&
            std::string(column->children[0]->format).rfind("+w:", 0) == 0) {
            vectors = column->children[0];
            multi = true;
        }
        std::string format = vectors == nullptr || vectors->format == nullptr ? "" : vectors->format;
        std::string item = vectors == nullptr || vectors->n_children != 1 || vectors->children[0]->format == nullptr
                               ? ""
                               : vectors->children[0]->format;
        if (vectors != nullptr && format.rfind("+w:", 0) == 0) {
            dim = static_cast<std::size_t>(std::strtoull(format.c_str() + 3, nullptr, 10));
            f32_column = item == "f";
            q.f16_column = item == "e";
            binary = item == "C";
        }
        schema.release(&schema);
        if (column == nullptr) {
            error = "Embedding column " + q.column + " is not in the dataset";
            return false;
        }
        if (dim == 0U || (item != "f" && item != "e" && item != "g" && !binary)) {
            error = "Query column " + q.column + " must be a vector of floats";
            return false;
        }
    }
    if (!multi && q.key_vectors != 1U) {
        error = "Query column " + q.column + " must be a multivector to take several query vectors";
        return false;
    }
    if (q.key.size() != dim * q.key_vectors) {
        error = "Query vector size " + std::to_string(q.key.size() / std::max<std::size_t>(q.key_vectors, 1U)) +
                " does not match index column size " + std::to_string(dim);
        return false;
    }
    // Binary vectors: Hamming distance only, exact (no index), the query's values are bytes.
    std::vector<std::uint8_t> binary_key;
    if (binary) {
        if (q.metric) {
            error = std::string("Distance type ") + vector_metric_name(*q.metric) +
                    " does not support UInt8 vectors, use hamming";
            return false;
        }
        for (const float v : q.key) {
            if (!(v >= 0.0F && v <= 255.0F && v == std::floor(v))) {
                error = "the query for a UInt8 vector must hold whole numbers from 0 to 255";
                return false;
            }
            binary_key.push_back(static_cast<std::uint8_t>(v));
        }
    } else if (q.hamming) {
        error = "Distance type hamming does not support Float32 vectors, use l2, cosine or dot";
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
    const bool chosen = q.use_index && !q.segments.empty();
    if (chosen) {
        // The segments asked for, in manifest order (Lance's with_index_segments).
        const std::set<std::array<std::uint8_t, 16>> wanted(q.segments.begin(), q.segments.end());
        segments.clear();
        for (const auto& index : manifest.indices) {
            if (wanted.count(index.uuid) != 0U) {
                segments.push_back(&index);
            }
        }
        if (segments.size() != wanted.size()) {
            std::string missing;
            for (const auto& uuid : wanted) {
                if (std::none_of(segments.begin(), segments.end(),
                                 [&](const pb::IndexMetadata* s) { return s->uuid == uuid; })) {
                    missing += (missing.empty() ? "\"" : ", \"") + pb::uuid_string(uuid) + "\"";
                }
            }
            error = "with_index_segments referenced unknown index segments: [" + missing + "]";
            return false;
        }
        if (std::any_of(segments.begin(), segments.end(),
                        [&](const pb::IndexMetadata* s) { return s->fields.empty() || s->fields.front() != fid; })) {
            error = "with_index_segments contained a segment that does not belong to vector column '" + q.column + "'";
            return false;
        }
        index_name = segments.front()->name;
        if (std::any_of(segments.begin(), segments.end(),
                        [&](const pb::IndexMetadata* s) { return s->name != index_name; })) {
            error = "with_index_segments must reference segments from a single logical index";
            return false;
        }
    }
    std::vector<std::shared_ptr<const IvfIndex>> loaded;
    std::string not_used;  // why the index was not used
    if (!q.use_index) {
        not_used = "use_index=False";
    } else if (!segments.empty()) {
        for (const auto* s : segments) {
            if (!not_used.empty()) {
                break;
            }
            std::shared_ptr<const IvfIndex> index;
            std::string why;
            std::filesystem::path seg_dir;
            if (!index_dir_of(dataset_path, manifest, *s, seg_dir, why) ||
                !load_ivf(seg_dir, index, why, q.index_cache)) {
                not_used = why;
            } else if (binary != (index->metric == VectorMetric::Hamming)) {
                if (chosen) {
                    error = "with_index_segments: the selected index segments cannot be searched: metric " +
                            std::string(vector_metric_name(index->metric)) + " for this column";
                    return false;
                }
                not_used = binary ? std::string("the index's metric is ") + vector_metric_name(index->metric) +
                                        ", not hamming"
                                  : std::string("the index's metric is hamming");
            } else if (q.metric && *q.metric != index->metric) {
                if (chosen) {
                    error = std::string("with_index_segments requested metric ") + vector_metric_name(*q.metric) +
                            " but the selected index segments use " + vector_metric_name(index->metric);
                    return false;
                }
                not_used = std::string("the index's metric is ") + vector_metric_name(index->metric);
            } else {
                loaded.push_back(index);
            }
        }
        if (!not_used.empty()) {
            if (chosen) {
                error = "with_index_segments: the selected index segments cannot be searched: " + not_used;
                return false;
            }
            loaded.clear();
        }
    }
    const VectorMetric metric = !loaded.empty() ? loaded.front()->metric : q.metric.value_or(VectorMetric::L2);
    const char* metric_name = binary ? "hamming" : vector_metric_name(metric);

    std::vector<std::uint64_t> all_fragments;
    for (const auto& f : manifest.fragments) {
        all_fragments.push_back(f.id);
    }
    RowIdToAddress resolver;  // stable row ids: what the index holds are ids
    if (!loaded.empty() && !RowIdToAddress::build(dataset_path, manifest, resolver, error)) {
        return false;
    }
    std::vector<Candidate> hits;
    if (loaded.empty()) {
        if (!segments.empty() && !not_used.empty()) {
            out.plan.push_back("index " + index_name + " not used: " + not_used);
        }
        if (q.fast_search) {  // as pylance: fast_search skips the unindexed rows, so without an index, none
            out.plan.push_back(std::string("KNNVectorDistance: metric=") + metric_name +
                               " (fast_search, no index: no rows)");
        } else {
            out.plan.push_back(std::string("KNNVectorDistance: metric=") + metric_name +
                               " (exact, " + std::to_string(all_fragments.size()) + " fragments)");
            if (!flat_search(dataset_path, q, metric, dim, all_fragments, hits, error, binary ? &binary_key : nullptr, multi)) {
                return false;
            }
        }
    } else {
        const std::size_t refine = q.refine_factor.value_or(1U);
        const std::size_t kk = static_cast<std::size_t>(q.k) * std::max<std::size_t>(refine, 1U);
        std::set<std::uint32_t> covered;
        std::vector<Candidate> ann;
        CacheCount counts;
        out.plan.push_back("ANNSubIndex: name=" + index_name + ", k=" + std::to_string(q.k) + ", deltas=" +
                           std::to_string(loaded.size()) + ", metric=" + (binary ? "Hamming" : metric_name) + " (" +
                           loaded.front()->type + ")");
        // Each segment's row mask (the fragments it covers, deletions, the prefilter), built once.
        std::vector<RowMask> masks;
        masks.reserve(loaded.size());
        for (std::size_t s = 0; s < loaded.size(); ++s) {
            masks.emplace_back();
            RowMask& mask = masks.back();
            mask.resolver = &resolver;
            mask.caller = q.row_mask;
            for (const auto& f : manifest.fragments) {
                mask.physical[static_cast<std::uint32_t>(f.id)] = f.physical_rows;
            }
            mask.limit.insert(segments[s]->fragment_ids.begin(), segments[s]->fragment_ids.end());
            for (const auto stale : fragments_stale_for_index(manifest, segments[s]->fields,
                                                              segments[s]->dataset_version)) {
                mask.limit.erase(stale);  // overlaid since the index was built: searched exactly instead
            }
            covered.insert(mask.limit.begin(), mask.limit.end());
            if (mask.limit.empty()) {
                mask.limit.insert(UINT32_MAX);  // an empty limit means any fragment; this one has none left
            }
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
        }
        std::vector<std::uint64_t> unindexed;
        if (!q.fast_search && !chosen) {
            for (const auto& f : manifest.fragments) {
                if (covered.count(static_cast<std::uint32_t>(f.id)) == 0U) {
                    unindexed.push_back(f.id);
                }
            }
        }
        if (multi) {
            // XTR over the index (xtr_search). Distance bounds are not written for several vectors per row
            // yet: refused rather than answered from the wrong distances.
            if (q.lower_bound || q.upper_bound) {
                error = "distance bounds on a multivector index are not supported yet";
                return false;
            }
            constexpr std::uint32_t kXtrOverfetch = 10;  // Lance's DEFAULT_XTR_OVERFETCH
            // Lance's multivec_ann keeps k * refine_factor rows (1 without a refine_factor).
            if (!xtr_search(q, loaded, masks, resolver, dim, f32_column, kXtrOverfetch, kk, ann, out.plan, error,
                            counts)) {
                return false;
            }
            out.plan.push_back(std::string("KNNVectorDistance: metric=") + metric_name + " (multivector XTR, " +
                               std::to_string(q.key_vectors) + " query vectors)");
            if (q.refine_factor || !unindexed.empty()) {
                // Lance's scanner: with a refine_factor, the candidates are re-scored exactly and the top k
                // kept; knn_combined does the same once unindexed fragments are merged in. The XTR
                // distances are only for choosing the candidates.
                if (!rescore(dataset_path, q, metric, dim, ann, error, true)) {
                    return false;
                }
                keep_best(ann, q.k);
                out.plan.push_back(std::string("KNNVectorDistance: metric=") + metric_name +
                                   " (exact re-score of the XTR candidates)");
            }
        } else {
            for (std::size_t s = 0; s < loaded.size(); ++s) {
                std::string line;
                if (!search_segment(*loaded[s], q, f32_column, masks[s], kk, ann, line, error, counts)) {
                    return false;
                }
                out.plan.push_back("  " + line);
            }
            if (resolver.stable()) {  // from here on the candidates are addresses, as the exact search's are
                for (auto& hit : ann) {
                    resolver.find(hit.id, hit.id);
                }
            }
            keep_best(ann, kk);
            if (q.refine_factor || !unindexed.empty()) {
                if (!rescore(dataset_path, q, metric, dim, ann, error, false, binary ? &binary_key : nullptr)) {
                    return false;
                }
                keep_best(ann, q.k);
                out.plan.push_back(std::string("KNNVectorDistance: metric=") + metric_name +
                                   " (refine " + std::to_string(kk) + " candidates)");
            }
        }
        out.index_cache_hits = counts.hits.load();
        out.index_cache_misses = counts.misses.load();
        hits = std::move(ann);
        if (!unindexed.empty()) {
            out.plan.push_back(std::string("KNNVectorDistance: metric=") + metric_name + " (exact, " +
                               std::to_string(unindexed.size()) + " unindexed fragments)");
            if (!flat_search(dataset_path, q, metric, dim, unindexed, hits, error, binary ? &binary_key : nullptr, multi)) {
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

bool read_vector_index_model(const std::filesystem::path& dir, VectorIndexModel& out, std::string& error) {
    out = VectorIndexModel{};
    const auto index_path = dir / "index.idx";
    const auto aux_path = dir / "auxiliary.idx";
    std::string value;
    bool found = false;
    LanceDataFileFooterLayout layout{};
    if (!schema_metadata(index_path, "lance:index", out.index_metadata, found, layout, error) ||
        !schema_metadata(index_path, "lance:hnsw", out.sub_index_metadata, found, layout, error) ||
        !schema_metadata(index_path, "lance:ivf", value, found, layout, error)) {
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
    LanceDataFileFooterLayout aux_layout{};
    if (!schema_metadata(aux_path, "storage_metadata", out.storage_metadata, found, aux_layout, error) ||
        !schema_metadata(aux_path, "lance:ivf", value, found, aux_layout, error)) {
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
    out.centroids = std::move(model.centroids);
    for (const std::uint8_t b : model.bytes) {  // a Hamming index: its centroids' byte values
        out.centroids.push_back(static_cast<float>(b));
    }
    out.partitions = model.partitions;
    out.dim = model.dim;
    out.has_loss = model.has_loss;
    out.loss = model.loss;
    out.partition_sizes = std::move(parts.lengths);
    return true;
}


bool read_vector_index_partition(const std::filesystem::path& dir, std::size_t p, bool with_values,
                                 VectorIndexPartition& out, std::string& error) {
    out = VectorIndexPartition{};
    std::shared_ptr<const IvfIndex> index;
    if (!load_ivf(dir, index, error)) {
        return false;
    }
    out.partitions = index->partitions;
    if (p >= index->partitions) {
        error = "vector index: partition " + std::to_string(p) + " is out of range (" +
                std::to_string(index->partitions) + " partitions)";
        return false;
    }
    std::shared_ptr<const Partition> part;
    CacheCount count;
    if (!index->partition(p, part, error, count)) {
        return false;
    }
    out.ids = part->ids;
    if (!with_values) {
        return true;
    }
    const std::size_t n = part->ids.size();
    if (index->storage == Storage::Flat && index->metric == VectorMetric::Hamming) {
        out.column = "flat";
        out.width = index->dim;
        out.uint8 = true;
        out.codes = part->codes;
    } else if (index->storage == Storage::Flat) {
        out.column = "flat";
        out.width = index->dim;
        out.floats = part->vectors;
    } else if (index->storage == Storage::Pq) {
        out.column = "__pq_code";
        out.width = index->code_bytes;
        if (index->transposed) {
            out.codes = part->codes;
        } else {  // the partition holds [code byte][row]: back to row by row
            out.codes.resize(n * out.width);
            for (std::size_t r = 0; r < n; ++r) {
                for (std::size_t s = 0; s < out.width; ++s) {
                    out.codes[r * out.width + s] = part->codes[s * n + r];
                }
            }
        }
    } else {
        out.column = "__sq_code";
        out.width = index->dim;
        out.codes = part->codes;
    }
    return true;
}

}  // namespace nano_lance
