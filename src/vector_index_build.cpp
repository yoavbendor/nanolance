// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Building Lance's IVF_FLAT / IVF_PQ indexes (vector_search.hpp), in Lance 12's format
// (docs/VECTOR_INDEX.md), with Lance's defaults:
//
//   IVF   k-means (random initial centroids, at most 50 Lloyd iterations, stopping when the loss
//         changes by less than 1e-4 of itself) on at most 256 vectors a partition; hierarchical
//         (16-way splits) past 256 partitions. Every vector then goes to its nearest centroid --
//         squared L2 (cosine: of the normalized vectors) or the largest dot product.
//   PQ    per sub-vector, k-means of 2^nbits centroids on at most 256 * 2^nbits residuals
//         (vector - its centroid; the vector itself for dot); codes are the nearest centroids. Both
//         by L2 whatever the metric, as Lance's v3 builder does: dot only scores the codes.
//
// The vectors are scanned with their row addresses (null vectors and vectors with NaN / infinite
// values left out, as Lance's filter_nan leaves them), and the index is committed in the
// manifest's index section as Lance commits one.

#include "nanolance/vector_search.hpp"

#include "index_files.hpp"

#include "nanolance/dataset_commit.hpp"
#include "nanolance/lance_file_writer.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/parallel.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <random>
#include <system_error>
#include <unordered_set>

namespace nano_lance {
namespace {

using index_files::OwnedArray;
using index_files::OwnedBatches;
using index_files::OwnedSchema;
using index_files::WrittenFile;
using index_files::bytes_of;
using index_files::find_field;
using index_files::new_uuid;
using index_files::struct_batch;
using index_files::write_file;

constexpr std::size_t kMaxPartitions = 4096;  // Lance's recommended_num_partitions cap
constexpr std::size_t kHierarchyFanOut = 16;  // Lance's hierarchical_k
constexpr double kTolerance = 1e-4;           // Lance's KMeansParams::tolerance

// ── protobuf writing ────────────────────────────────────────────────────────────────────────────

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_key(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint32_t wire) {
    put_varint(out, (static_cast<std::uint64_t>(field) << 3U) | wire);
}

void put_bytes(std::vector<std::uint8_t>& out, std::uint32_t field, const void* data, std::size_t size) {
    put_key(out, field, 2);
    put_varint(out, size);
    const auto* p = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}

void put_packed(std::vector<std::uint8_t>& out, std::uint32_t field, const std::vector<std::uint64_t>& values) {
    std::vector<std::uint8_t> body;
    for (const auto v : values) {
        put_varint(body, v);
    }
    put_bytes(out, field, body.data(), body.size());
}

/// Tensor { data_type = FLOAT32, shape, data }.
std::vector<std::uint8_t> tensor_message(const std::vector<std::uint64_t>& shape, const std::vector<float>& data) {
    std::vector<std::uint8_t> out;
    put_key(out, 1, 0);
    put_varint(out, 2);  // FLOAT32
    put_packed(out, 2, shape);
    put_bytes(out, 3, data.data(), data.size() * sizeof(float));
    return out;
}

/// IVF { offsets = 2, lengths = 3, centroids_tensor = 4 (when given), loss = 5 (when given) }.
std::vector<std::uint8_t> ivf_message(const std::vector<std::uint64_t>& offsets, const std::vector<std::uint64_t>& lengths,
                                      const std::vector<std::uint8_t>* centroids, const double* loss) {
    std::vector<std::uint8_t> out;
    put_packed(out, 2, offsets);
    put_packed(out, 3, lengths);
    if (centroids != nullptr) {
        put_bytes(out, 4, centroids->data(), centroids->size());
    }
    if (loss != nullptr) {
        put_key(out, 5, 1);
        const auto* p = reinterpret_cast<const std::uint8_t*>(loss);
        out.insert(out.end(), p, p + 8);
    }
    return out;
}

// ── k-means ─────────────────────────────────────────────────────────────────────────────────────

/// `n` vectors of `d` floats, row-major.
struct Rows {
    std::vector<float> v;
    std::size_t n = 0;
    std::size_t d = 0;
    const float* row(std::size_t i) const { return v.data() + i * d; }
};

/// Each of `x`'s rows' nearest centroid: smallest squared L2, or (dot) largest dot product.
/// `dist` (optional) gets the squared L2 distance, or 1 - dot.
void assign(const Rows& x, const std::vector<float>& centroids, std::size_t k, bool dot, std::vector<std::uint32_t>& label,
            std::vector<float>* dist) {
    const std::size_t d = x.d;
    std::vector<float> ct(d * k);  // transposed: [d][k]
    std::vector<float> norm(k, 0.0F);
    for (std::size_t c = 0; c < k; ++c) {
        for (std::size_t j = 0; j < d; ++j) {
            const float v = centroids[c * d + j];
            ct[j * k + c] = v;
            norm[c] += v * v;
        }
    }
    label.resize(x.n);
    if (dist != nullptr) {
        dist->resize(x.n);
    }
    constexpr std::size_t kChunk = 256;
    const std::size_t chunks = (x.n + kChunk - 1) / kChunk;
    parallel::for_each(chunks, [&](std::size_t chunk) {
        std::vector<float> acc(k);
        const std::size_t end = std::min(x.n, (chunk + 1) * kChunk);
        for (std::size_t r = chunk * kChunk; r < end; ++r) {
            const float* xr = x.row(r);
            std::fill(acc.begin(), acc.end(), 0.0F);
            float xnorm = 0.0F;
            for (std::size_t j = 0; j < d; ++j) {
                const float xj = xr[j];
                xnorm += xj * xj;
                const float* col = ct.data() + j * k;
                for (std::size_t c = 0; c < k; ++c) {
                    acc[c] += xj * col[c];
                }
            }
            std::size_t best = 0;
            float best_score = std::numeric_limits<float>::infinity();
            for (std::size_t c = 0; c < k; ++c) {
                const float score = dot ? -acc[c] : norm[c] - 2.0F * acc[c];
                if (score < best_score) {
                    best_score = score;
                    best = c;
                }
            }
            label[r] = static_cast<std::uint32_t>(best);
            if (dist != nullptr) {
                (*dist)[r] = dot ? 1.0F - acc[best] : std::max(0.0F, xnorm + best_score);
            }
        }
    });
}

/// `k` distinct indices of [0, n).
std::vector<std::size_t> sample_indices(std::size_t n, std::size_t k, std::mt19937_64& rng) {
    std::vector<std::size_t> out;
    if (k >= n) {
        out.resize(n);
        std::iota(out.begin(), out.end(), std::size_t{0});
        return out;
    }
    if (k * 2U > n) {
        out.resize(n);
        std::iota(out.begin(), out.end(), std::size_t{0});
        for (std::size_t i = 0; i < k; ++i) {
            std::uniform_int_distribution<std::size_t> pick(i, n - 1U);
            std::swap(out[i], out[pick(rng)]);
        }
        out.resize(k);
        std::sort(out.begin(), out.end());
        return out;
    }
    // Floyd's algorithm.
    std::unordered_set<std::size_t> chosen;
    for (std::size_t j = n - k; j < n; ++j) {
        std::uniform_int_distribution<std::size_t> pick(0, j);
        const std::size_t t = pick(rng);
        chosen.insert(chosen.count(t) != 0U ? j : t);
    }
    out.assign(chosen.begin(), chosen.end());
    std::sort(out.begin(), out.end());
    return out;
}

Rows gather(const Rows& x, const std::vector<std::size_t>& which) {
    Rows out;
    out.d = x.d;
    out.n = which.size();
    out.v.resize(out.n * out.d);
    for (std::size_t i = 0; i < which.size(); ++i) {
        std::memcpy(out.v.data() + i * out.d, x.row(which[i]), out.d * sizeof(float));
    }
    return out;
}

/// Lloyd's k-means of `x` into `k` centroids (k <= x.n): random initial centroids; stops after
/// `max_iters` or when the loss changes by less than kTolerance of itself.
std::vector<float> kmeans_flat(const Rows& x, std::size_t k, bool dot, std::uint32_t max_iters, std::mt19937_64& rng,
                               double& loss) {
    const std::size_t d = x.d;
    std::vector<float> centroids(k * d);
    const auto init = sample_indices(x.n, k, rng);
    for (std::size_t c = 0; c < k; ++c) {
        std::memcpy(centroids.data() + c * d, x.row(init[c]), d * sizeof(float));
    }
    std::vector<std::uint32_t> label;
    std::vector<float> dist;
    double previous = std::numeric_limits<double>::infinity();
    loss = 0;
    for (std::uint32_t it = 0; it < std::max<std::uint32_t>(max_iters, 1U); ++it) {
        assign(x, centroids, k, dot, label, &dist);
        loss = 0;
        for (const float v : dist) {
            loss += v;
        }
        std::vector<double> sums(k * d, 0.0);
        std::vector<std::size_t> counts(k, 0);
        for (std::size_t r = 0; r < x.n; ++r) {
            const auto c = label[r];
            ++counts[c];
            const float* xr = x.row(r);
            double* s = sums.data() + static_cast<std::size_t>(c) * d;
            for (std::size_t j = 0; j < d; ++j) {
                s[j] += xr[j];
            }
        }
        for (std::size_t c = 0; c < k; ++c) {
            if (counts[c] != 0U) {
                for (std::size_t j = 0; j < d; ++j) {
                    centroids[c * d + j] = static_cast<float>(sums[c * d + j] / static_cast<double>(counts[c]));
                }
            }
        }
        // An empty cluster splits the largest one in two.
        for (std::size_t c = 0; c < k; ++c) {
            if (counts[c] != 0U) {
                continue;
            }
            const auto big = static_cast<std::size_t>(std::max_element(counts.begin(), counts.end()) - counts.begin());
            if (counts[big] < 2U) {
                break;
            }
            constexpr float eps = 1.0F / 1024.0F;
            for (std::size_t j = 0; j < d; ++j) {
                const float v = centroids[big * d + j];
                const float nudge = (j % 2U == 0U ? eps : -eps) * (std::fabs(v) + eps);
                centroids[c * d + j] = v + nudge;
                centroids[big * d + j] = v - nudge;
            }
            counts[c] = counts[big] / 2U;
            counts[big] -= counts[c];
        }
        if (std::isfinite(previous) && std::fabs(previous - loss) <= kTolerance * std::fabs(previous)) {
            break;
        }
        previous = loss;
    }
    return centroids;
}

/// k-means as Lance trains IVF: flat up to 256 centroids, hierarchical past that (each level splits
/// its vectors 16 ways and gives each part centroids in proportion to its size).
std::vector<float> kmeans(const Rows& x, std::size_t k, bool dot, std::uint32_t max_iters, std::mt19937_64& rng,
                          double& loss) {
    if (k <= 256U || k < kHierarchyFanOut * 2U) {
        return kmeans_flat(x, k, dot, max_iters, rng, loss);
    }
    double top_loss = 0;
    const auto top = kmeans_flat(x, kHierarchyFanOut, dot, max_iters, rng, top_loss);
    std::vector<std::uint32_t> label;
    assign(x, top, kHierarchyFanOut, dot, label, nullptr);
    std::vector<std::vector<std::size_t>> parts(kHierarchyFanOut);
    for (std::size_t r = 0; r < x.n; ++r) {
        parts[label[r]].push_back(r);
    }
    // Centroids per part: proportional to its rows (largest remainders), at least 1 for a non-empty
    // part, never more than its rows.
    std::vector<std::size_t> share(kHierarchyFanOut, 0);
    std::vector<std::pair<double, std::size_t>> remainders;
    std::size_t given = 0;
    for (std::size_t i = 0; i < kHierarchyFanOut; ++i) {
        if (parts[i].empty()) {
            continue;
        }
        const double exact = static_cast<double>(k) * static_cast<double>(parts[i].size()) / static_cast<double>(x.n);
        share[i] = std::min(parts[i].size(), std::max<std::size_t>(1U, static_cast<std::size_t>(exact)));
        given += share[i];
        remainders.emplace_back(exact - std::floor(exact), i);
    }
    std::sort(remainders.begin(), remainders.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    while (given < k) {
        bool any = false;
        for (const auto& [r, i] : remainders) {
            if (given < k && share[i] < parts[i].size()) {
                ++share[i];
                ++given;
                any = true;
            }
        }
        if (!any) {
            break;
        }
    }
    while (given > k) {
        const auto big = static_cast<std::size_t>(std::max_element(share.begin(), share.end()) - share.begin());
        --share[big];
        --given;
    }
    std::vector<float> out;
    loss = 0;
    for (std::size_t i = 0; i < kHierarchyFanOut; ++i) {
        if (share[i] == 0U) {
            continue;
        }
        double part_loss = 0;
        const auto c = kmeans(gather(x, parts[i]), share[i], dot, max_iters, rng, part_loss);
        loss += part_loss;
        out.insert(out.end(), c.begin(), c.end());
    }
    return out;
}

// ── Arrow output ────────────────────────────────────────────────────────────────────────────────

bool fsl_schema(const char* name, ArrowType item, std::int64_t size, bool nullable, ArrowSchema& out) {
    ArrowSchemaInit(&out);
    if (ArrowSchemaSetTypeFixedSize(&out, NANOARROW_TYPE_FIXED_SIZE_LIST, static_cast<std::int32_t>(size)) !=
            NANOARROW_OK ||
        ArrowSchemaSetType(out.children[0], item) != NANOARROW_OK || ArrowSchemaSetName(out.children[0], "item") !=
                                                                         NANOARROW_OK ||
        ArrowSchemaSetName(&out, name) != NANOARROW_OK) {
        return false;
    }
    out.flags = nullable ? ARROW_FLAG_NULLABLE : 0;
    return true;
}

bool scalar_schema(const char* name, ArrowType type, bool nullable, ArrowSchema& out) {
    ArrowSchemaInit(&out);
    if (ArrowSchemaSetType(&out, type) != NANOARROW_OK || ArrowSchemaSetName(&out, name) != NANOARROW_OK) {
        return false;
    }
    out.flags = nullable ? ARROW_FLAG_NULLABLE : 0;
    return true;
}

/// A struct schema owning `children` (moved in).
bool table_schema(std::vector<ArrowSchema>& children, ArrowSchema& out) {
    ArrowSchemaInit(&out);
    if (ArrowSchemaSetTypeStruct(&out, static_cast<std::int64_t>(children.size())) != NANOARROW_OK) {
        return false;
    }
    for (std::size_t i = 0; i < children.size(); ++i) {
        out.children[i]->release(out.children[i]);
        ArrowSchemaMove(&children[i], out.children[i]);
    }
    return true;
}

bool u64_array(const std::vector<std::uint64_t>& values, ArrowArray& out) {
    std::string ignored;
    return index_files::uint_array(NANOARROW_TYPE_UINT64, values.data(), static_cast<std::int64_t>(values.size()), 8, out,
                                   ignored);
}

/// A fixed-size list array of `n` lists of `size` items of `item` (`width` bytes each).
bool fsl_array(ArrowType item, std::size_t width, const void* values, std::int64_t n, std::int64_t size, ArrowArray& out) {
    if (ArrowArrayInitFromType(&out, NANOARROW_TYPE_FIXED_SIZE_LIST) != NANOARROW_OK ||
        ArrowArrayAllocateChildren(&out, 1) != NANOARROW_OK ||
        ArrowArrayInitFromType(out.children[0], item) != NANOARROW_OK ||
        ArrowBufferAppend(ArrowArrayBuffer(out.children[0], 1), values,
                          n * size * static_cast<std::int64_t>(width)) != NANOARROW_OK) {
        return false;
    }
    out.children[0]->length = n * size;
    out.children[0]->null_count = 0;
    out.length = n;
    out.null_count = 0;
    return ArrowArrayFinishBuildingDefault(&out, nullptr) == NANOARROW_OK;
}

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out + "\"";
}

}  // namespace

bool dataset_create_vector_index(const std::filesystem::path& dataset_path, const std::string& column,
                                 const VectorIndexOptions& options, std::uint64_t& new_version, std::string& error) {
    error.clear();
    const bool pq = options.type == "IVF_PQ";
    if (!pq && options.type != "IVF_FLAT") {
        error = "vector index type '" + options.type + "' is not supported (IVF_FLAT and IVF_PQ are)";
        return false;
    }
    if (pq && options.num_bits != 8U && options.num_bits != 4U) {
        error = "ProductQuantization: num_bits " + std::to_string(options.num_bits) + " not supported";
        return false;
    }
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        return false;
    }
    if ((manifest.reader_feature_flags & pb::kFlagStableRowIds) != 0U) {
        error = "a vector index on a dataset with stable row ids is not supported";
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
    if (existing != manifest.indices.end() && !options.replace) {
        error = "Index name '" + name + "' already exists, please specify a different name or use replace=True";
        return false;
    }

    // The vectors, with their row addresses: null vectors and vectors with NaN / infinite values out.
    LanceScanRequest request;
    request.has_version = true;
    request.version = version;
    const std::vector<std::string> columns = {parts.front()};
    request.columns = &columns;
    request.with_row_address = true;
    OwnedSchema scanned;
    OwnedBatches batches;
    if (!lance_dataset_scan(dataset_path, request, scanned.s, batches.v, error)) {
        return false;
    }
    const ArrowSchema* type = scanned.s.children[0];
    for (std::size_t p = 1; p < parts.size(); ++p) {
        const ArrowSchema* next = nullptr;
        for (std::int64_t c = 0; c < type->n_children; ++c) {
            if (type->children[c]->name != nullptr && parts[p] == type->children[c]->name) {
                next = type->children[c];
            }
        }
        if (next == nullptr) {
            error = "column '" + column + "' not found";
            return false;
        }
        type = next;
    }
    const std::string format = type->format == nullptr ? "" : type->format;
    const std::string item = type->n_children == 1 && type->children[0]->format != nullptr ? type->children[0]->format : "";
    if (format.rfind("+w:", 0) != 0 || item != "f") {
        error = "Vector column " + column + " must be a fixed-size list of float32 (got '" + format + "' of '" + item +
                "')";
        return false;
    }
    const std::size_t dim = static_cast<std::size_t>(std::strtoull(format.c_str() + 3, nullptr, 10));
    if (dim == 0U) {
        error = "Vector column " + column + " has dimension 0";
        return false;
    }
    if (pq && (options.num_sub_vectors == 0U)) {
        error = "num_partitions and num_sub_vectors are required for IVF_PQ";
        return false;
    }
    if (pq && dim % options.num_sub_vectors != 0U) {
        error = "dimension (" + std::to_string(dim) + ") must be divisible by num_sub_vectors (" +
                std::to_string(options.num_sub_vectors) + ")";
        return false;
    }
    if (pq && options.num_bits == 4U && options.num_sub_vectors % 2U != 0U) {
        error = "4-bit PQ needs an even num_sub_vectors (got " + std::to_string(options.num_sub_vectors) + ")";
        return false;
    }

    Rows data;
    data.d = dim;
    std::vector<std::uint64_t> ids;
    for (const auto& batch : batches.v) {
        ArrowArrayView view;
        ArrowError e{};
        if (ArrowArrayViewInitFromSchema(&view, &scanned.s, &e) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &e) != NANOARROW_OK) {
            ArrowArrayViewReset(&view);
            error = e.message;
            return false;
        }
        const ArrowArrayView* list = view.children[0];
        // Nested vector columns: walk down the struct children by name.
        const ArrowSchema* level = scanned.s.children[0];
        for (std::size_t p = 1; p < parts.size(); ++p) {
            for (std::int64_t c = 0; c < level->n_children; ++c) {
                if (level->children[c]->name != nullptr && parts[p] == level->children[c]->name) {
                    list = list->children[c];
                    level = level->children[c];
                    break;
                }
            }
        }
        const ArrowArrayView* items = list->children[0];
        const ArrowArrayView* addrs = view.children[view.n_children - 1];
        const float* values = items->buffer_views[1].data.as_float + items->offset;
        for (std::int64_t r = 0; r < batch.length; ++r) {
            const std::int64_t row = view.offset + r;
            if (ArrowArrayViewIsNull(list, row)) {
                continue;
            }
            const float* v = values + (list->offset + row) * static_cast<std::int64_t>(dim);
            if (!std::all_of(v, v + dim, [](float x) { return std::isfinite(x); })) {
                continue;
            }
            if (options.metric == VectorMetric::Cosine) {
                float norm = 0;
                for (std::size_t j = 0; j < dim; ++j) {
                    norm += v[j] * v[j];
                }
                norm = std::sqrt(norm);
                if (!(norm > 0.0F)) {
                    continue;
                }
                for (std::size_t j = 0; j < dim; ++j) {
                    data.v.push_back(v[j] / norm);
                }
            } else {
                data.v.insert(data.v.end(), v, v + dim);
            }
            ids.push_back(ArrowArrayViewGetUIntUnsafe(addrs, row));
        }
        ArrowArrayViewReset(&view);
    }
    data.n = ids.size();

    std::size_t partitions = 0;
    if (options.num_partitions) {
        partitions = *options.num_partitions;
    } else {
        const std::size_t target = options.target_partition_size.value_or(pq ? 8192U : 4096U);
        partitions = std::clamp<std::size_t>(data.n / std::max<std::size_t>(target, 1U), 1U, kMaxPartitions);
    }
    if (partitions == 0U) {
        error = "num_partitions must be at least 1";
        return false;
    }
    if (partitions > data.n) {
        error = "KMeans cannot train " + std::to_string(partitions) + " centroids with " + std::to_string(data.n) +
                " vectors; choose a smaller K (< " + std::to_string(data.n) + ")";
        return false;
    }
    const std::size_t nc = std::size_t{1} << options.num_bits;
    if (pq && data.n < nc) {
        error = "Not enough rows to train PQ. Requires " + std::to_string(nc) + " rows but only " +
                std::to_string(data.n) + " available";
        return false;
    }
    std::mt19937_64 rng(options.seed.value_or(std::random_device{}() ^
                                              static_cast<std::uint64_t>(
                                                  std::chrono::steady_clock::now().time_since_epoch().count())));

    // IVF: train on a sample, then every vector to its nearest centroid.
    const bool dot_metric = options.metric == VectorMetric::Dot;
    double loss = 0;
    const auto train = gather(data, sample_indices(data.n, std::size_t{options.sample_rate} * partitions, rng));
    const auto centroids = kmeans(train, partitions, dot_metric, options.max_iters, rng, loss);
    std::vector<std::uint32_t> label;
    assign(data, centroids, partitions, dot_metric, label, nullptr);
    std::vector<std::uint64_t> lengths(partitions, 0);
    for (const auto l : label) {
        ++lengths[l];
    }
    std::vector<std::uint64_t> offsets(partitions, 0);
    for (std::size_t p = 1; p < partitions; ++p) {
        offsets[p] = offsets[p - 1] + lengths[p - 1];
    }
    std::vector<std::size_t> order(data.n);  // rows grouped by partition, ascending address within
    {
        auto at = offsets;
        for (std::size_t r = 0; r < data.n; ++r) {
            order[at[label[r]]++] = r;
        }
    }
    std::vector<std::uint64_t> sorted_ids(data.n);
    for (std::size_t i = 0; i < data.n; ++i) {
        sorted_ids[i] = ids[order[i]];
    }

    // PQ: a codebook per sub-vector from a sample of residuals; every row's codes.
    const std::size_t m = pq ? options.num_sub_vectors : 0U;
    const std::size_t w = pq ? dim / m : 0U;
    const std::size_t code_bytes = pq ? (options.num_bits == 4U ? m / 2U : m) : 0U;
    std::vector<float> codebook;           // [m][nc][w]
    std::vector<std::uint8_t> codes;       // per partition: [code_bytes][n_p], partitions in order
    if (pq) {
        const auto residual = [&](std::size_t r, float* out) {
            const float* v = data.row(r);
            const float* c = centroids.data() + static_cast<std::size_t>(label[r]) * dim;
            for (std::size_t j = 0; j < dim; ++j) {
                out[j] = dot_metric ? v[j] : v[j] - c[j];
            }
        };
        const auto picked = sample_indices(data.n, std::size_t{options.sample_rate} * nc, rng);
        std::vector<float> res(dim);
        codebook.resize(m * nc * w);
        std::vector<Rows> subs(m);
        for (std::size_t s = 0; s < m; ++s) {
            subs[s].d = w;
            subs[s].n = picked.size();
            subs[s].v.resize(picked.size() * w);
        }
        for (std::size_t i = 0; i < picked.size(); ++i) {
            residual(picked[i], res.data());
            for (std::size_t s = 0; s < m; ++s) {
                std::memcpy(subs[s].v.data() + i * w, res.data() + s * w, w * sizeof(float));
            }
        }
        // The sub-vectors train side by side, each on its own seed (so a build is reproducible
        // whatever the thread count); L2 for every metric, as Lance's v3 builder trains its PQ (dot
        // only scores the codes).
        std::vector<std::uint64_t> seeds(m);
        for (auto& seed : seeds) {
            seed = rng();
        }
        parallel::for_each(m, [&](std::size_t s) {
            std::mt19937_64 sub_rng(seeds[s]);
            double sub_loss = 0;
            const auto c = kmeans_flat(subs[s], nc, false, options.max_iters, sub_rng, sub_loss);
            std::copy(c.begin(), c.end(), codebook.begin() + static_cast<std::ptrdiff_t>(s * nc * w));
        });
        // Codes: each sub-vector's nearest codebook entry by L2, whatever the metric.
        std::vector<std::uint8_t> row_codes(data.n * m);  // [row][m], rows in `order`
        parallel::for_each(m, [&](std::size_t s) {
            std::vector<float> res(dim);
            Rows sub;
            sub.d = w;
            sub.n = data.n;
            sub.v.resize(data.n * w);
            for (std::size_t i = 0; i < data.n; ++i) {
                residual(order[i], res.data());
                std::memcpy(sub.v.data() + i * w, res.data() + s * w, w * sizeof(float));
            }
            std::vector<std::uint32_t> nearest;
            const std::vector<float> book(codebook.begin() + static_cast<std::ptrdiff_t>(s * nc * w),
                                          codebook.begin() + static_cast<std::ptrdiff_t>((s + 1U) * nc * w));
            assign(sub, book, nc, false, nearest, nullptr);
            for (std::size_t i = 0; i < data.n; ++i) {
                row_codes[i * m + s] = static_cast<std::uint8_t>(nearest[i]);
            }
        });
        // Transposed per partition: [code byte][row of the partition].
        codes.resize(data.n * code_bytes);
        for (std::size_t p = 0; p < partitions; ++p) {
            const std::size_t first = offsets[p];
            const std::size_t n = lengths[p];
            std::uint8_t* out = codes.data() + first * code_bytes;
            for (std::size_t r = 0; r < n; ++r) {
                const std::uint8_t* rc = row_codes.data() + (first + r) * m;
                for (std::size_t b = 0; b < code_bytes; ++b) {
                    out[b * n + r] = options.num_bits == 4U
                                         ? static_cast<std::uint8_t>((rc[2U * b] & 0x0FU) | (rc[2U * b + 1U] << 4U))
                                         : rc[b];
                }
            }
        }
    }

    // The files.
    const auto uuid = new_uuid();
    const auto dir = dataset_path / "_indices" / pb::uuid_string(uuid);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }
    const auto fail = [&](const std::string& why) {
        error = why;
        std::filesystem::remove_all(dir, ec);
        return false;
    };
    const std::string metric_name = vector_metric_name(options.metric);
    std::vector<WrittenFile> files;
    {
        // auxiliary.idx: the rows, grouped by partition.
        std::vector<ArrowSchema> children(2);
        OwnedSchema schema;
        if (!scalar_schema("_rowid", NANOARROW_TYPE_UINT64, true, children[0]) ||
            !(pq ? fsl_schema("__pq_code", NANOARROW_TYPE_UINT8, static_cast<std::int64_t>(code_bytes), true, children[1])
                 : fsl_schema("flat", NANOARROW_TYPE_FLOAT, static_cast<std::int64_t>(dim), false, children[1])) ||
            !table_schema(children, schema.s)) {
            return fail("out of memory");
        }
        OwnedArray rowid;
        OwnedArray values;
        std::vector<float> flat;
        if (!pq) {
            flat.resize(data.n * dim);
            for (std::size_t i = 0; i < data.n; ++i) {
                std::memcpy(flat.data() + i * dim, data.row(order[i]), dim * sizeof(float));
            }
        }
        if (!u64_array(sorted_ids, rowid.a) ||
            !(pq ? fsl_array(NANOARROW_TYPE_UINT8, 1, codes.data(), static_cast<std::int64_t>(data.n),
                             static_cast<std::int64_t>(code_bytes), values.a)
                 : fsl_array(NANOARROW_TYPE_FLOAT, 4, flat.data(), static_cast<std::int64_t>(data.n),
                             static_cast<std::int64_t>(dim), values.a))) {
            return fail("out of memory");
        }
        OwnedArray batch;
        if (!struct_batch({&rowid.a, &values.a}, static_cast<std::int64_t>(data.n), batch.a, error)) {
            return fail(error);
        }
        LanceFileExtras extras;
        extras.schema_metadata["lance:ivf"] = bytes_of("1");
        extras.schema_metadata["distance_type"] = bytes_of(metric_name);
        std::string storage;
        if (pq) {
            storage = "{\"codebook_position\":2,\"nbits\":" + std::to_string(options.num_bits) +
                      ",\"num_sub_vectors\":" + std::to_string(m) + ",\"dimension\":" + std::to_string(dim) +
                      ",\"codebook_tensor\":[],\"transposed\":true}";
        } else {
            storage = "{\"dim\":" + std::to_string(dim) + "}";
        }
        extras.schema_metadata["storage_metadata"] = bytes_of("[" + json_string(storage) + "]");
        extras.global_buffers.push_back(ivf_message(offsets, lengths, nullptr, nullptr));
        if (pq) {
            extras.global_buffers.push_back(tensor_message({nc, dim}, codebook));
        }
        if (!write_file(dir, "auxiliary.idx", schema.s, batch.a, extras, files, error)) {
            return fail(error);
        }
    }
    {
        // index.idx: no rows; the centroids.
        std::vector<ArrowSchema> children(1);
        OwnedSchema schema;
        OwnedArray marker;
        OwnedArray batch;
        if (!scalar_schema("__flat_marker", NANOARROW_TYPE_UINT64, false, children[0]) ||
            !table_schema(children, schema.s) || !u64_array({}, marker.a) ||
            !struct_batch({&marker.a}, 0, batch.a, error)) {
            return fail(error.empty() ? "out of memory" : error);
        }
        LanceFileExtras extras;
        std::string flat_list = "[";
        for (std::size_t p = 0; p < partitions; ++p) {
            flat_list += p == 0U ? "\"\"" : ",\"\"";
        }
        extras.schema_metadata["lance:flat"] = bytes_of(flat_list + "]");
        extras.schema_metadata["lance:index"] =
            bytes_of("{\"type\":\"" + options.type + "\",\"distance_type\":\"" + metric_name + "\"}");
        extras.schema_metadata["lance:ivf"] = bytes_of("1");
        const auto tensor = tensor_message({partitions, dim}, centroids);
        const std::vector<std::uint64_t> zeros(partitions, 0);
        extras.global_buffers.push_back(ivf_message(zeros, zeros, &tensor, &loss));
        if (!write_file(dir, "index.idx", schema.s, batch.a, extras, files, error)) {
            return fail(error);
        }
    }

    // The manifest entry: VectorIndexDetails { metric_type, target_partition_size, pq | flat, hints }.
    std::vector<std::uint8_t> details;
    if (options.metric != VectorMetric::L2) {
        put_key(details, 1, 0);
        put_varint(details, options.metric == VectorMetric::Cosine ? 1U : 2U);
    }
    if (!options.num_partitions) {
        put_key(details, 2, 0);
        put_varint(details, options.target_partition_size.value_or(pq ? 8192U : 4096U));
    }
    if (pq) {
        std::vector<std::uint8_t> pq_details;
        put_key(pq_details, 1, 0);
        put_varint(pq_details, options.num_bits);
        put_key(pq_details, 2, 0);
        put_varint(pq_details, m);
        put_bytes(details, 4, pq_details.data(), pq_details.size());
    } else {
        put_bytes(details, 8, nullptr, 0);
    }
    const auto hint = [&](const std::string& key, const std::string& value) {
        std::vector<std::uint8_t> entry;
        put_bytes(entry, 1, key.data(), key.size());
        put_bytes(entry, 2, value.data(), value.size());
        put_bytes(details, 9, entry.data(), entry.size());
    };
    hint("lance.ivf.max_iters", std::to_string(options.max_iters));
    hint("lance.ivf.sample_rate", std::to_string(options.sample_rate));
    if (pq) {
        hint("lance.pq.max_iters", std::to_string(options.max_iters));
        hint("lance.pq.sample_rate", std::to_string(options.sample_rate));
        hint("lance.pq.kmeans_redos", "1");
    }

    std::vector<std::uint32_t> fragment_ids;
    for (const auto& f : manifest.fragments) {
        fragment_ids.push_back(static_cast<std::uint32_t>(f.id));
    }
    std::sort(fragment_ids.begin(), fragment_ids.end());
    std::vector<pb::IndexMetadata::File> index_files;
    for (const auto& f : files) {
        index_files.push_back({f.name, f.size});
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (existing != manifest.indices.end()) {
        manifest.indices.erase(std::remove_if(manifest.indices.begin(), manifest.indices.end(),
                                              [&](const pb::IndexMetadata& i) { return i.name == name; }),
                               manifest.indices.end());
    }
    manifest.indices.push_back(pb::make_index_metadata(uuid, {field->id}, name, version, fragment_ids,
                                                       "/lance.index.pb.VectorIndexDetails", 1,
                                                       static_cast<std::uint64_t>(now), index_files, details));
    if (!commit_next_version(dataset_path, std::move(manifest), new_version, error)) {
        std::filesystem::remove_all(dir, ec);
        return false;
    }
    return true;
}

}  // namespace nano_lance
