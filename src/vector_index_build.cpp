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

#include "index_build.hpp"
#include "rabitq_build.hpp"
#include "nanolance/vector_search.hpp"

#include "float16.hpp"
#include "float_text.hpp"
#include "hnsw_build.hpp"
#include "index_files.hpp"

#include "nanolance/dataset_commit.hpp"
#include "nanolance/lance_file_writer.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/parallel.hpp"
#include "nanolance/row_ids.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <random>
#include <set>
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

/// Lance's IndexType::target_partition_size.
std::size_t default_partition_size(const std::string& type) {
    return type == "IVF_FLAT" ? 4096U : (type.rfind("IVF_HNSW_", 0) == 0 ? std::size_t{1} << 20U : 8192U);
}
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

/// Tensor { data_type = FLOAT16, shape, data }: the values, already rounded to binary16 (pylance's
/// centroids of a float16 multivector index).
std::vector<std::uint8_t> tensor_message_half(const std::vector<std::uint64_t>& shape, const std::vector<float>& data) {
    std::vector<std::uint16_t> bits(data.size());
    for (std::size_t i = 0; i < data.size(); ++i) {
        bits[i] = float16_bits(data[i]);
    }
    std::vector<std::uint8_t> out;
    put_key(out, 1, 0);
    put_varint(out, 1);  // FLOAT16
    put_packed(out, 2, shape);
    put_bytes(out, 3, bits.data(), bits.size() * sizeof(std::uint16_t));
    return out;
}

/// Tensor { data_type = UINT8, shape, data }: a Hamming index's centroids, as bytes.
std::vector<std::uint8_t> tensor_message_bytes(const std::vector<std::uint64_t>& shape,
                                               const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> out;
    put_key(out, 1, 0);
    put_varint(out, 4);  // UINT8
    put_packed(out, 2, shape);
    put_bytes(out, 3, data.data(), data.size());
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

/// Four rows (consecutive, `d` floats each, from `x0`) against every centroid of `ct` ([d][k]): their dot
/// products into `acc` ([4][k], zeroed) and their squared norms into `xnorm`. Also built for AVX2 (below);
/// never with FMA, so every multiply and add rounds as in the baseline build and the results are
/// bit-identical on every machine.
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
inline void dot_block4_body(const float* x0, std::size_t d, const float* ct, std::size_t k, float* acc, float* xnorm) {
    const float* x1 = x0 + d;
    const float* x2 = x1 + d;
    const float* x3 = x2 + d;
    for (std::size_t j = 0; j < d; ++j) {
        xnorm[0] += x0[j] * x0[j];
        xnorm[1] += x1[j] * x1[j];
        xnorm[2] += x2[j] * x2[j];
        xnorm[3] += x3[j] * x3[j];
    }
    // Sixteen centroids at a time, their four rows' sums kept in registers across every dimension: each
    // sum still adds the dimensions in order from zero, as the one-column-at-a-time loop does.
    constexpr std::size_t kTile = 16;
    std::size_t c0 = 0;
    for (; c0 + kTile <= k; c0 += kTile) {
        float a0[kTile] = {};
        float a1[kTile] = {};
        float a2[kTile] = {};
        float a3[kTile] = {};
        for (std::size_t j = 0; j < d; ++j) {
            const float* w = ct + j * k + c0;
            const float v0 = x0[j];
            const float v1 = x1[j];
            const float v2 = x2[j];
            const float v3 = x3[j];
            for (std::size_t c = 0; c < kTile; ++c) {
                a0[c] += v0 * w[c];
                a1[c] += v1 * w[c];
                a2[c] += v2 * w[c];
                a3[c] += v3 * w[c];
            }
        }
        for (std::size_t c = 0; c < kTile; ++c) {
            acc[c0 + c] = a0[c];
            acc[k + c0 + c] = a1[c];
            acc[2 * k + c0 + c] = a2[c];
            acc[3 * k + c0 + c] = a3[c];
        }
    }
    for (std::size_t j = 0; j < d && c0 < k; ++j) {  // the last k % 16 centroids
        const float* col = ct + j * k;
        for (std::size_t c = c0; c < k; ++c) {
            acc[c] += x0[j] * col[c];
            acc[k + c] += x1[j] * col[c];
            acc[2 * k + c] += x2[j] * col[c];
            acc[3 * k + c] += x3[j] * col[c];
        }
    }
}

// The AVX2 build is picked by a CPU check on first use, not by target_clones: its load-time resolver (an
// IFUNC) crashed in vector index builds on a machine whose process loads another GCC's libgcc_s
// (2026-10-22, tools/crash_probe.py: the same code without target_clones passed). NANOLANCE_NO_AVX2=1
// keeps the baseline build.
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__) && !defined(NANOLANCE_NO_TARGET_CLONES)
__attribute__((target("avx2"))) void dot_block4_avx2(const float* x0, std::size_t d, const float* ct, std::size_t k,
                                                     float* acc, float* xnorm) {
    dot_block4_body(x0, d, ct, k, acc, xnorm);
}

void dot_block4_baseline(const float* x0, std::size_t d, const float* ct, std::size_t k, float* acc, float* xnorm) {
    dot_block4_body(x0, d, ct, k, acc, xnorm);
}

void dot_block4(const float* x0, std::size_t d, const float* ct, std::size_t k, float* acc, float* xnorm) {
    static const bool avx2 = [] {
        const char* off = std::getenv("NANOLANCE_NO_AVX2");
        if (off != nullptr && *off != '\0' && *off != '0') {
            return false;
        }
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") != 0;
    }();
    (avx2 ? dot_block4_avx2 : dot_block4_baseline)(x0, d, ct, k, acc, xnorm);
}
#else
void dot_block4(const float* x0, std::size_t d, const float* ct, std::size_t k, float* acc, float* xnorm) {
    dot_block4_body(x0, d, ct, k, acc, xnorm);
}
#endif

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
    // Rows go four at a time, so each centroid column loaded from cache serves four rows. Every row's
    // dot products are summed in the same order as one row at a time: the results are bit-identical.
    constexpr std::size_t kBlock = 4;
    parallel::for_each(chunks, [&](std::size_t chunk) {
        std::vector<float> acc(kBlock * k);
        const std::size_t end = std::min(x.n, (chunk + 1) * kChunk);
        for (std::size_t r0 = chunk * kChunk; r0 < end; r0 += kBlock) {
            const std::size_t rows = std::min(kBlock, end - r0);
            std::fill(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(rows * k), 0.0F);
            float xnorm[kBlock] = {0.0F, 0.0F, 0.0F, 0.0F};
            if (rows == kBlock) {
                dot_block4(x.row(r0), d, ct.data(), k, acc.data(), xnorm);
            } else {
                for (std::size_t i = 0; i < rows; ++i) {
                    const float* xr = x.row(r0 + i);
                    float* a = acc.data() + i * k;
                    for (std::size_t j = 0; j < d; ++j) {
                        const float xj = xr[j];
                        xnorm[i] += xj * xj;
                        const float* col = ct.data() + j * k;
                        for (std::size_t c = 0; c < k; ++c) {
                            a[c] += xj * col[c];
                        }
                    }
                }
            }
            for (std::size_t i = 0; i < rows; ++i) {
                const float* a = acc.data() + i * k;
                std::size_t best = 0;
                float best_score = std::numeric_limits<float>::infinity();
                for (std::size_t c = 0; c < k; ++c) {
                    const float score = dot ? -a[c] : norm[c] - 2.0F * a[c];
                    if (score < best_score) {
                        best_score = score;
                        best = c;
                    }
                }
                label[r0 + i] = static_cast<std::uint32_t>(best);
                if (dist != nullptr) {
                    (*dist)[r0 + i] = dot ? 1.0F - a[best] : std::max(0.0F, xnorm[i] + best_score);
                }
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

// ── Binary (Hamming) IVF: k-modes. Separate from the float k-means above, which it does not touch. ──

/// `n` vectors of `d` bytes, row-major (a uint8 column's vectors, as stored).
struct ByteRows {
    std::vector<std::uint8_t> v;
    std::size_t n = 0;
    std::size_t d = 0;
    const std::uint8_t* row(std::size_t i) const { return v.data() + i * d; }
};

ByteRows gather_bytes(const ByteRows& x, const std::vector<std::size_t>& which) {
    ByteRows out;
    out.d = x.d;
    out.n = which.size();
    out.v.resize(out.n * out.d);
    for (std::size_t i = 0; i < which.size(); ++i) {
        std::memcpy(out.v.data() + i * out.d, x.row(which[i]), out.d);
    }
    return out;
}

/// Bits in which the `n` bytes of `a` and `b` differ.
std::uint32_t hamming_bits(const std::uint8_t* a, const std::uint8_t* b, std::size_t n) {
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < n; ++i) {
        bits += static_cast<std::uint32_t>(std::popcount(static_cast<unsigned>(a[i] ^ b[i])));
    }
    return bits;
}

/// Each row's nearest centroid by Hamming distance (the first one at the least distance), and the distance.
void hamming_assign(const ByteRows& x, const std::vector<std::uint8_t>& centroids, std::size_t k,
                    std::vector<std::uint32_t>& label, std::vector<float>* dist) {
    label.resize(x.n);
    if (dist != nullptr) {
        dist->resize(x.n);
    }
    constexpr std::size_t kChunk = 1024;
    const std::size_t chunks = (x.n + kChunk - 1U) / kChunk;
    parallel::for_each(chunks, [&](std::size_t chunk) {
        for (std::size_t r = chunk * kChunk; r < std::min(x.n, (chunk + 1U) * kChunk); ++r) {
            std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
            std::uint32_t arg = 0;
            for (std::size_t c = 0; c < k; ++c) {
                const std::uint32_t h = hamming_bits(x.row(r), centroids.data() + c * x.d, x.d);
                if (h < best) {
                    best = h;
                    arg = static_cast<std::uint32_t>(c);
                }
            }
            label[r] = arg;
            if (dist != nullptr) {
                (*dist)[r] = static_cast<float>(best);
            }
        }
    });
}

/// Lance's binary k-means (KModeAlgo): Hamming assignment, then each centroid bit is 1 when more than half
/// of its cluster's vectors have it set (a tie is 0), bits least significant first within each byte.
/// Initial centroids are k sampled rows, as the float k-means; an empty cluster keeps its centroid.
/// Stops after `max_iters` or when the loss changes by less than kTolerance of itself.
std::vector<std::uint8_t> kmodes(const ByteRows& x, std::size_t k, std::uint32_t max_iters, std::mt19937_64& rng,
                                 double& loss) {
    const std::size_t d = x.d;
    std::vector<std::uint8_t> centroids(k * d);
    const auto init = sample_indices(x.n, k, rng);
    for (std::size_t c = 0; c < k; ++c) {
        std::memcpy(centroids.data() + c * d, x.row(init[c]), d);
    }
    std::vector<std::uint32_t> label;
    std::vector<float> dist;
    double previous = std::numeric_limits<double>::infinity();
    loss = 0;
    for (std::uint32_t it = 0; it < std::max<std::uint32_t>(max_iters, 1U); ++it) {
        hamming_assign(x, centroids, k, label, &dist);
        loss = 0;
        for (const float v : dist) {
            loss += v;
        }
        std::vector<std::vector<std::size_t>> members(k);
        for (std::size_t r = 0; r < x.n; ++r) {
            members[label[r]].push_back(r);
        }
        for (std::size_t c = 0; c < k; ++c) {
            const auto& rows = members[c];
            if (rows.empty()) {
                continue;
            }
            std::vector<std::size_t> ones(d * 8U, 0);  // per bit: the cluster's vectors with it set
            for (const auto r : rows) {
                const std::uint8_t* xr = x.row(r);
                for (std::size_t j = 0; j < d; ++j) {
                    for (std::size_t b = 0; b < 8U; ++b) {
                        ones[j * 8U + b] += (xr[j] >> b) & 1U;
                    }
                }
            }
            for (std::size_t j = 0; j < d; ++j) {
                std::uint8_t byte = 0;
                for (std::size_t b = 0; b < 8U; ++b) {
                    if (ones[j * 8U + b] * 2U > rows.size()) {
                        byte = static_cast<std::uint8_t>(byte | (1U << b));
                    }
                }
                centroids[c * d + j] = byte;
            }
        }
        if (std::isfinite(previous) && std::fabs(previous - loss) <= kTolerance * std::fabs(previous)) {
            break;
        }
        previous = loss;
    }
    return centroids;
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

bool list_schema(const char* name, ArrowType item, ArrowSchema& out) {
    ArrowSchemaInit(&out);
    if (ArrowSchemaSetType(&out, NANOARROW_TYPE_LIST) != NANOARROW_OK ||
        ArrowSchemaSetType(out.children[0], item) != NANOARROW_OK ||
        ArrowSchemaSetName(out.children[0], "item") != NANOARROW_OK || ArrowSchemaSetName(&out, name) != NANOARROW_OK) {
        return false;
    }
    out.flags = ARROW_FLAG_NULLABLE;
    return true;
}

/// A list array: list i is values[offsets[i], offsets[i + 1]) of `item` (`width` bytes each).
bool list_array(ArrowType item, std::size_t width, const std::vector<std::int32_t>& offsets, const void* values,
                ArrowArray& out) {
    const std::int64_t count = offsets.back();
    if (ArrowArrayInitFromType(&out, NANOARROW_TYPE_LIST) != NANOARROW_OK ||
        ArrowArrayAllocateChildren(&out, 1) != NANOARROW_OK ||
        ArrowArrayInitFromType(out.children[0], item) != NANOARROW_OK ||
        ArrowBufferAppend(ArrowArrayBuffer(&out, 1), offsets.data(),
                          static_cast<std::int64_t>(offsets.size() * sizeof(std::int32_t))) != NANOARROW_OK ||
        ArrowBufferAppend(ArrowArrayBuffer(out.children[0], 1), values,
                          count * static_cast<std::int64_t>(width)) != NANOARROW_OK) {
        return false;
    }
    out.children[0]->length = count;
    out.children[0]->null_count = 0;
    out.length = static_cast<std::int64_t>(offsets.size()) - 1;
    out.null_count = 0;
    return ArrowArrayFinishBuildingDefault(&out, nullptr) == NANOARROW_OK;
}

/// A double as serde_json writes it: the shortest round-trip form, with ".0" when integral.
std::string json_double(double x) {
    std::string s = float_text::to_text(x);
    if (s.find_first_of(".eEn") == std::string::npos) {
        s += ".0";
    }
    return s;
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

namespace {

constexpr std::size_t kMaxPartitionSizeFactor = 4;    // lance-index MAX_PARTITION_SIZE_FACTOR
constexpr std::size_t kMinPartitionSizePercent = 25;  // lance-index MIN_PARTITION_SIZE_PERCENT
constexpr std::size_t kReassignRange = 64;            // lance REASSIGN_RANGE
constexpr std::size_t kSplitSampleRate = 256;         // lance SPLIT_SAMPLE_RATE
constexpr std::size_t kMaxSplitWays = 1024;           // lance MAX_SPLIT_WAYS

float distance(const float* a, const float* b, std::size_t d, bool dot) {
    float s = 0;
    if (dot) {
        for (std::size_t j = 0; j < d; ++j) {
            s += a[j] * b[j];
        }
        return -s;
    }
    for (std::size_t j = 0; j < d; ++j) {
        const float t = a[j] - b[j];
        s += t * t;
    }
    return s;
}

/// The `count` centroids nearest to `c` (by `dot` or L2), `skip` and `removed` left out.
std::vector<std::uint32_t> nearest_centroids(const std::vector<float>& centroids, std::size_t k, std::size_t d,
                                             const float* c, std::size_t skip, const std::vector<bool>& removed,
                                             std::size_t count, bool dot) {
    std::vector<std::pair<float, std::uint32_t>> all;
    for (std::size_t p = 0; p < k; ++p) {
        if (p != skip && !removed[p]) {
            all.emplace_back(distance(c, centroids.data() + p * d, d, dot), static_cast<std::uint32_t>(p));
        }
    }
    const std::size_t n = std::min(count, all.size());
    std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n), all.end());
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(all[i].second);
    }
    return out;
}

/// Lance 12's partition adjustment when it merges rows into an IVF index (lance/src/index/vector/
/// builder.rs, plan_partition_adjustment): every partition over 4x the target is split straight to
/// the target size, by k-means on a sample of its rows; when none is, partitions under a quarter of
/// the target are joined away, smallest first, as long as the survivors have room below the split
/// threshold for every joined row, each row going to the nearest of the 64 survivors nearest its old
/// centroid that has room. Updates `centroids`, `k` and `label`.
void adjust_partitions(const Rows& data, std::vector<float>& centroids, std::size_t& k,
                       std::vector<std::uint32_t>& label, std::size_t target, bool dot,
                       std::uint32_t max_iters) {
    const std::size_t d = data.d;
    const std::size_t split_threshold = kMaxPartitionSizeFactor * target;
    const std::size_t join_threshold = kMinPartitionSizePercent * target / 100U;
    std::vector<std::size_t> sizes(k, 0);
    for (const auto l : label) {
        ++sizes[l];
    }
    std::vector<std::vector<std::size_t>> members(k);
    for (std::size_t r = 0; r < data.n; ++r) {
        members[label[r]].push_back(r);
    }
    std::vector<std::pair<std::size_t, std::size_t>> splits;  // partition, ways
    for (std::size_t p = 0; p < k; ++p) {
        if (sizes[p] > split_threshold) {
            splits.emplace_back(p, std::clamp<std::size_t>((sizes[p] + target - 1U) / target, 2U, kMaxSplitWays));
        }
    }
    const std::vector<bool> none(k, false);
    if (!splits.empty()) {
        const std::vector<float> old = centroids;
        std::set<std::size_t> affected;
        bool split_any = false;
        for (const auto& [p, ways_wanted] : splits) {
            // A seeded sample of the partition's rows (seeded per partition, as Lance seeds its split).
            std::mt19937_64 rng(p);
            const auto picked = sample_indices(members[p].size(), kSplitSampleRate * ways_wanted, rng);
            Rows sample;
            sample.d = d;
            for (const auto i : picked) {
                const float* v = data.row(members[p][i]);
                sample.v.insert(sample.v.end(), v, v + d);
            }
            sample.n = picked.size();
            const std::size_t ways = std::min(ways_wanted, sample.n);
            if (ways < 2U) {
                continue;
            }
            double loss = 0;
            const auto trained = kmeans(sample, ways, dot, max_iters, rng, loss);
            // Keep the centroids that win a sampled row from the old centroids around the partition.
            const auto neighbors = nearest_centroids(old, k, d, old.data() + p * d, p, none, kReassignRange, dot);
            std::vector<bool> wins(ways, false);
            for (std::size_t i = 0; i < sample.n; ++i) {
                const float* v = sample.v.data() + i * d;
                std::size_t best = 0;
                float best_d = std::numeric_limits<float>::infinity();
                for (std::size_t c = 0; c < ways; ++c) {
                    const float dd = distance(v, trained.data() + c * d, d, dot);
                    if (dd < best_d) {
                        best_d = dd;
                        best = c;
                    }
                }
                float old_d = std::numeric_limits<float>::infinity();
                for (const auto q : neighbors) {
                    old_d = std::min(old_d, distance(v, old.data() + std::size_t{q} * d, d, dot));
                }
                if (best_d < old_d) {
                    wins[best] = true;
                }
            }
            std::vector<std::size_t> kept;
            for (std::size_t c = 0; c < ways; ++c) {
                if (wins[c]) {
                    kept.push_back(c);
                }
            }
            if (kept.size() < 2U) {
                continue;  // too alike to split
            }
            split_any = true;
            std::copy_n(trained.data() + kept[0] * d, d, centroids.data() + p * d);
            affected.insert(p);
            for (std::size_t i = 1; i < kept.size(); ++i) {
                centroids.insert(centroids.end(), trained.data() + kept[i] * d, trained.data() + (kept[i] + 1U) * d);
                affected.insert(centroids.size() / d - 1U);
            }
            for (const auto q : neighbors) {
                affected.insert(q);
            }
        }
        if (!split_any) {
            return;
        }
        // The rows of every affected partition go to their nearest centroid of the new set.
        k = centroids.size() / d;
        for (std::size_t r = 0; r < data.n; ++r) {
            if (affected.count(label[r]) == 0U) {
                continue;
            }
            const float* v = data.row(r);
            std::uint32_t best = label[r];
            float best_d = std::numeric_limits<float>::infinity();
            for (std::size_t c = 0; c < k; ++c) {
                const float dd = distance(v, centroids.data() + c * d, d, dot);
                if (dd < best_d) {
                    best_d = dd;
                    best = static_cast<std::uint32_t>(c);
                }
            }
            label[r] = best;
        }
        return;
    }
    if (k < 2U) {
        return;
    }
    std::size_t total = 0;
    for (const auto n : sizes) {
        total += n;
    }
    const std::size_t keep_at_least = std::max<std::size_t>((total + split_threshold - 1U) / split_threshold, 1U);
    const std::size_t max_joins = k > keep_at_least ? k - keep_at_least : 0U;
    std::vector<std::pair<std::size_t, std::size_t>> candidates;  // size, partition
    for (std::size_t p = 0; p < k; ++p) {
        if (sizes[p] < join_threshold) {
            candidates.emplace_back(sizes[p], p);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    if (candidates.size() > max_joins) {
        candidates.resize(max_joins);
    }
    if (candidates.empty()) {
        return;
    }
    std::vector<std::size_t> joins;
    std::vector<bool> removed(k, false);
    for (const auto& c : candidates) {
        joins.push_back(c.second);
        removed[c.second] = true;
    }
    std::sort(joins.begin(), joins.end());
    std::vector<std::size_t> room(k, 0);
    for (std::size_t p = 0; p < k; ++p) {
        room[p] = removed[p] ? 0U : split_threshold - std::min(split_threshold, sizes[p]);
    }
    for (const auto p : joins) {
        const auto window = nearest_centroids(centroids, k, d, centroids.data() + p * d, p, removed, kReassignRange, dot);
        for (const auto r : members[p]) {
            const float* v = data.row(r);
            const auto nearest_of = [&](const std::vector<std::uint32_t>& ids, bool need_room) {
                std::int64_t best = -1;
                float best_d = std::numeric_limits<float>::infinity();
                for (const auto q : ids) {
                    if (need_room && room[q] == 0U) {
                        continue;
                    }
                    const float dd = distance(v, centroids.data() + std::size_t{q} * d, d, dot);
                    if (dd < best_d) {
                        best_d = dd;
                        best = q;
                    }
                }
                return best;
            };
            std::int64_t dest = nearest_of(window, true);
            if (dest < 0) {
                std::vector<std::uint32_t> anywhere;
                for (std::size_t q = 0; q < k; ++q) {
                    if (!removed[q]) {
                        anywhere.push_back(static_cast<std::uint32_t>(q));
                    }
                }
                dest = nearest_of(anywhere, true);
                if (dest < 0) {
                    dest = nearest_of(window, false);
                }
            }
            label[r] = static_cast<std::uint32_t>(dest);
            if (room[static_cast<std::size_t>(dest)] > 0U) {
                --room[static_cast<std::size_t>(dest)];
            }
        }
    }
    // The survivors, renumbered in order.
    std::vector<std::uint32_t> renumber(k, 0);
    std::vector<float> kept;
    std::uint32_t next = 0;
    for (std::size_t p = 0; p < k; ++p) {
        if (!removed[p]) {
            renumber[p] = next++;
            kept.insert(kept.end(), centroids.data() + p * d, centroids.data() + (p + 1U) * d);
        }
    }
    for (auto& l : label) {
        l = renumber[l];
    }
    centroids = std::move(kept);
    k = next;
}

/// Lance's PQ pairwise distance table: for each sub-vector, the distance between every two of its
/// codebook entries under the metric's sub-vector distance (PQ's build_pairwise_distance_table).
/// Layout [m][nc][nc]; the HNSW graph over PQ codes sums its entries.
std::vector<float> pairwise_table(const std::vector<float>& codebook, std::size_t m, std::size_t nc, std::size_t w,
                                  VectorMetric metric) {
    std::vector<float> table(m * nc * nc);
    for (std::size_t s = 0; s < m; ++s) {
        const float* book = codebook.data() + s * nc * w;
        for (std::size_t a = 0; a < nc; ++a) {
            for (std::size_t b = 0; b < nc; ++b) {
                const float* x = book + a * w;
                const float* y = book + b * w;
                float dot = 0;
                float nx = 0;
                float ny = 0;
                float l2 = 0;
                for (std::size_t j = 0; j < w; ++j) {
                    dot += x[j] * y[j];
                    nx += x[j] * x[j];
                    ny += y[j] * y[j];
                    const float t = x[j] - y[j];
                    l2 += t * t;
                }
                float d = l2;
                if (metric == VectorMetric::Dot) {
                    d = 1.0F - dot;
                } else if (metric == VectorMetric::Cosine) {
                    d = 1.0F - dot / std::sqrt(nx * ny);
                }
                table[(s * nc + a) * nc + b] = d;
            }
        }
    }
    return table;
}

// ── IVF_RQ (RaBitQ) build: Lance 12's transform (lance-index bq/transform.rs, builder.rs, ex_dot.rs), in
// its layout. Rows are residuals (v - centroid; cosine rows are normalized first, so the L2 formulas apply).

constexpr float kRabitErrorEpsilon = 1.9F;        // lance-index bq/transform.rs RABIT_ERROR_EPSILON
constexpr float kExQuantizationEpsilon = 1.0e-5F;  // builder.rs EX_QUANTIZATION_EPSILON
constexpr float kExTightStart[9] = {0.0F, 0.15F, 0.20F, 0.52F, 0.59F, 0.71F, 0.75F, 0.77F, 0.81F};
constexpr std::size_t kRqBlockDims = 64;           // ex_dot.rs EX_DOT_BLOCK_DIMS
// ex_dot.rs / storage.rs: the row order inside a 16-row half of a packed 32-row batch.
constexpr std::uint8_t kRqPerm0Inverse[16] = {0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15};

float float_from_bits(std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

std::uint32_t float_to_bits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

float factor_ratio(float numerator, float denominator) {
    return denominator == 0.0F ? 0.0F : numerator / denominator;
}

// builder.rs best_ex_rescale_factor: the rescale t that maximizes the inner product of the quantized
// absolute values (ties to the earlier threshold, as Lance's sorted scan does).
float best_ex_rescale_factor(const std::vector<float>& abs_normalized, unsigned ex_bits) {
    float max_value = 0.0F;
    for (const float value : abs_normalized) {
        if (std::isfinite(value) && value > max_value) {
            max_value = value;
        }
    }
    if (max_value <= 0.0F) {
        return 0.0F;
    }
    const std::size_t max_code = (std::size_t{1} << ex_bits) - 1U;
    const float t_end = static_cast<float>(max_code + 10U) / max_value;
    const float t_start = t_end * kExTightStart[ex_bits];
    std::vector<std::size_t> current(abs_normalized.size(), 0);
    float squared_denominator = static_cast<float>(abs_normalized.size()) * 0.25F;
    float numerator = 0.0F;
    std::vector<std::uint64_t> thresholds;
    for (std::size_t idx = 0; idx < abs_normalized.size(); ++idx) {
        const float value = abs_normalized[idx];
        if (value <= 0.0F || !std::isfinite(value)) {
            continue;
        }
        const float code = std::clamp(std::floor((t_start * value) + kExQuantizationEpsilon), 0.0F,
                                      static_cast<float>(max_code));
        const std::size_t cur = static_cast<std::size_t>(code);
        current[idx] = cur;
        squared_denominator += static_cast<float>(cur * cur + cur);
        numerator += (static_cast<float>(cur) + 0.5F) * value;
        for (std::size_t next = cur + 1U; next <= max_code; ++next) {
            const float threshold = static_cast<float>(next) / value;
            if (threshold < t_end) {
                thresholds.push_back((static_cast<std::uint64_t>(float_to_bits(threshold)) << 32U) | idx);
            }
        }
    }
    std::sort(thresholds.begin(), thresholds.end());
    float best_inner_product = numerator / std::sqrt(squared_denominator);
    float best_t = t_start;
    for (const std::uint64_t packed : thresholds) {
        const float threshold = float_from_bits(static_cast<std::uint32_t>(packed >> 32U));
        const std::size_t idx = static_cast<std::uint32_t>(packed);
        current[idx] += 1U;
        squared_denominator += static_cast<float>(2U * current[idx]);
        numerator += abs_normalized[idx];
        const float inner_product = numerator / std::sqrt(squared_denominator);
        if (inner_product > best_inner_product) {
            best_inner_product = inner_product;
            best_t = threshold;
        }
    }
    return best_t;
}

// builder.rs quantize_ex_code: the extended code of one rotated residual into `values` (one per
// dimension, ex_bits wide, negative dimensions complemented). Returns the residual's dot with the
// full code (the ex res dot).
float quantize_ex_code(const float* rotated, std::size_t dim, unsigned ex_bits, std::uint8_t* values) {
    float norm_squared = 0.0F;
    for (std::size_t j = 0; j < dim; ++j) {
        norm_squared += rotated[j] * rotated[j];
    }
    if (norm_squared <= std::numeric_limits<float>::epsilon() || !std::isfinite(norm_squared)) {
        std::fill(values, values + dim, std::uint8_t{0});
        return 0.0F;
    }
    const float norm = std::sqrt(norm_squared);
    std::vector<float> abs_normalized(dim);
    for (std::size_t j = 0; j < dim; ++j) {
        abs_normalized[j] = std::fabs(rotated[j]) / norm;
    }
    const float t = best_ex_rescale_factor(abs_normalized, ex_bits);
    const auto mask = static_cast<std::uint8_t>((1U << ex_bits) - 1U);
    const float code_bias = -(static_cast<float>(1U << ex_bits) - 0.5F);
    float residual_dot_code = 0.0F;
    for (std::size_t j = 0; j < dim; ++j) {
        auto ex = static_cast<std::uint8_t>(std::clamp(std::floor((t * abs_normalized[j]) + kExQuantizationEpsilon),
                                                       0.0F, static_cast<float>(mask)));
        const bool negative = std::signbit(rotated[j]);
        if (negative) {
            ex = static_cast<std::uint8_t>((~ex) & mask);
        }
        const std::uint32_t sign_code = negative ? 0U : 1U;
        const std::uint32_t full_code = (sign_code << ex_bits) + ex;
        residual_dot_code += rotated[j] * (static_cast<float>(full_code) + code_bias);
        values[j] = ex;
    }
    return residual_dot_code;
}

// ex_dot.rs pack_top_plane: bit `top_bit` of the 64 values, as the 8 bytes stored after a block's codes.
std::uint64_t rq_top_plane(const std::uint8_t* v, unsigned top_bit) {
    std::uint64_t plane = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        for (std::size_t b = 0; b < 16; ++b) {
            const std::uint64_t bit = (v[16U * k + b] >> top_bit) & 1U;
            plane |= bit << (8U * (b % 8U) + 2U * k + b / 8U);
        }
    }
    return plane;
}

void rq_put_u64_le(std::uint64_t plane, std::uint8_t* out) {
    for (std::size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>(plane >> (8U * i));
    }
}

// ex_dot.rs pack_block: 64 values of ex_bits each into 8 * ex_bits bytes (the blocked layout).
void rq_pack_block(unsigned ex_bits, const std::uint8_t* v, std::uint8_t* out) {
    switch (ex_bits) {
        case 1:
            for (std::size_t b = 0; b < 8; ++b) {
                std::uint8_t byte = 0;
                for (std::size_t t = 0; t < 8; ++t) {
                    byte = static_cast<std::uint8_t>(byte | ((v[8U * b + t] & 1U) << t));
                }
                out[b] = byte;
            }
            return;
        case 2:
        case 3:
            for (std::size_t b = 0; b < 16; ++b) {
                out[b] = static_cast<std::uint8_t>((v[b] & 0x3U) | ((v[16U + b] & 0x3U) << 2U) |
                                                   ((v[32U + b] & 0x3U) << 4U) | ((v[48U + b] & 0x3U) << 6U));
            }
            if (ex_bits == 3) {
                rq_put_u64_le(rq_top_plane(v, 2), out + 16);
            }
            return;
        case 4:
            for (std::size_t unit = 0; unit < 4; ++unit) {
                for (std::size_t b = 0; b < 8; ++b) {
                    out[8U * unit + b] = static_cast<std::uint8_t>((v[16U * unit + b] & 0x0FU) |
                                                                   ((v[16U * unit + 8U + b] & 0x0FU) << 4U));
                }
            }
            return;
        case 5:
            for (std::size_t b = 0; b < 16; ++b) {
                out[b] = static_cast<std::uint8_t>((v[b] & 0x0FU) | ((v[16U + b] & 0x0FU) << 4U));
                out[16U + b] = static_cast<std::uint8_t>((v[32U + b] & 0x0FU) | ((v[48U + b] & 0x0FU) << 4U));
            }
            rq_put_u64_le(rq_top_plane(v, 4), out + 32);
            return;
        case 6:
        case 7:
            for (std::size_t k = 0; k < 3; ++k) {
                for (std::size_t b = 0; b < 16; ++b) {
                    out[16U * k + b] = static_cast<std::uint8_t>((v[16U * k + b] & 0x3FU) |
                                                                 (((v[48U + b] >> (2U * k)) & 0x3U) << 6U));
                }
            }
            if (ex_bits == 7) {
                rq_put_u64_le(rq_top_plane(v, 6), out + 48);
            }
            return;
        default:  // 8
            std::copy(v, v + 64, out);
            return;
    }
}

// The blocked extended code of one row: dim padded to 64-dimension blocks, each block packed.
void rq_pack_blocked_row(const std::uint8_t* values, std::size_t dim, unsigned ex_bits, std::uint8_t* out) {
    const std::size_t blocks = (dim + kRqBlockDims - 1U) / kRqBlockDims;
    const std::size_t block_bytes = kRqBlockDims * ex_bits / 8U;
    for (std::size_t block = 0; block < blocks; ++block) {
        std::uint8_t block_values[kRqBlockDims] = {};
        const std::size_t base = block * kRqBlockDims;
        std::copy(values + base, values + std::min(dim, base + kRqBlockDims), block_values);
        rq_pack_block(ex_bits, block_values, out + block * block_bytes);
    }
}

// The packed 1-bit codes of one partition (ex_dot-free layout of storage.rs rq_row_codes): full 32-row
// batches as [column][32 bytes] with two rows per byte, then the remainder column by column.
void rq_pack_partition_codes(const std::uint8_t* rows, std::size_t n, std::size_t code_len, std::uint8_t* out) {
    const std::size_t rem = n % 32U;
    const std::size_t full = n - rem;
    for (std::size_t base = 0; base < full; base += 32U) {
        std::uint8_t* batch = out + base * code_len;
        for (std::size_t col = 0; col < code_len; ++col) {
            for (std::size_t r = 0; r < 16U; ++r) {
                const std::uint8_t lo = rows[(base + r) * code_len + col];
                const std::uint8_t hi = rows[(base + r + 16U) * code_len + col];
                const std::size_t idx = kRqPerm0Inverse[r];
                batch[col * 32U + idx] = static_cast<std::uint8_t>((lo & 0x0FU) | ((hi & 0x0FU) << 4U));
                batch[col * 32U + idx + 16U] = static_cast<std::uint8_t>(((lo >> 4U) & 0x0FU) | (((hi >> 4U) & 0x0FU) << 4U));
            }
        }
    }
    std::uint8_t* rest = out + full * code_len;
    for (std::size_t off = 0; off < rem; ++off) {
        for (std::size_t col = 0; col < code_len; ++col) {
            rest[col * rem + off] = rows[(full + off) * code_len + col];
        }
    }
}

// The IVF_RQ auxiliary.idx: one row per vector, grouped by partition, with Lance's columns and factors.
bool write_rq_auxiliary(const Rows& data, const std::vector<float>& centroids, const std::vector<std::size_t>& order,
                        const std::vector<std::uint64_t>& offsets, const std::vector<std::uint64_t>& lengths,
                        const std::vector<std::uint64_t>& sorted_ids, const std::vector<std::uint8_t>& signs,
                        std::uint32_t num_bits, bool dot, bool skip_transpose, const std::string& metric_name,
                        const std::filesystem::path& dir, std::vector<WrittenFile>& files, std::string& error) {
    const std::size_t n = data.n;
    const std::size_t dim = data.d;
    const std::size_t partitions = lengths.size();
    const std::size_t code_bytes = dim / 8U;
    const bool ex = num_bits > 1U;
    const auto ex_bits = static_cast<unsigned>(num_bits - 1U);
    const std::size_t ex_len = ex ? (dim + kRqBlockDims - 1U) / kRqBlockDims * kRqBlockDims * ex_bits / 8U : 0U;
    // The centroids in the rotated space (Lance's rotated_centroids, raw-query estimator).
    std::vector<float> rotated_centroids(partitions * dim);
    for (std::size_t p = 0; p < partitions; ++p) {
        rq_build::rotate(centroids.data() + p * dim, dim, signs.data(), rotated_centroids.data() + p * dim);
    }
    std::vector<std::uint32_t> part_of(n);
    for (std::size_t p = 0; p < partitions; ++p) {
        std::fill(part_of.begin() + static_cast<std::ptrdiff_t>(offsets[p]),
                  part_of.begin() + static_cast<std::ptrdiff_t>(offsets[p] + lengths[p]), static_cast<std::uint32_t>(p));
    }
    std::vector<std::uint8_t> codes(n * code_bytes, 0);
    std::vector<std::uint8_t> ex_codes(n * ex_len, 0);
    std::vector<float> add(n), scale(n), error_factor(n), add_ex(ex ? n : 0U), scale_ex(ex ? n : 0U);
    const std::size_t block_rows = 1024;
    parallel::for_each((n + block_rows - 1U) / block_rows, [&](std::size_t block) {
        std::vector<float> residual(dim), rotated(dim), rc(dim);
        std::vector<std::uint8_t> values(dim);
        for (std::size_t i = block * block_rows; i < std::min(n, (block + 1U) * block_rows); ++i) {
            const std::size_t p = part_of[i];
            const float* v = data.row(order[i]);
            const float* c = centroids.data() + p * dim;
            float norm_square = 0.0F;
            for (std::size_t j = 0; j < dim; ++j) {
                residual[j] = v[j] - c[j];
                norm_square += residual[j] * residual[j];
            }
            rq_build::rotate(residual.data(), dim, signs.data(), rotated.data());
            const float* rotated_centroid = rotated_centroids.data() + p * dim;
            float binary_res_dot = 0.0F;
            float binary_cent_dot = 0.0F;
            float residual_centroid_dot = 0.0F;
            std::uint8_t* row_codes = codes.data() + i * code_bytes;
            std::fill(row_codes, row_codes + code_bytes, std::uint8_t{0});
            for (std::size_t j = 0; j < dim; ++j) {
                const bool positive = !std::signbit(rotated[j]);
                const float binary_factor = positive ? 0.5F : -0.5F;
                binary_res_dot += rotated[j] * binary_factor;
                binary_cent_dot += rotated_centroid[j] * binary_factor;
                residual_centroid_dot += rotated[j] * rotated_centroid[j];
                if (positive) {
                    row_codes[j / 8U] = static_cast<std::uint8_t>(row_codes[j / 8U] | (1U << (j % 8U)));
                }
            }
            const float binary_correction = factor_ratio(norm_square * binary_cent_dot, binary_res_dot);
            // lance-index bq/transform.rs error_factor_value
            float error = 0.0F;
            if (dim > 1U && norm_square > 0.0F && binary_res_dot != 0.0F) {
                const float code_norm_square = static_cast<float>(dim) * 0.25F;
                const float alignment = norm_square * code_norm_square / (binary_res_dot * binary_res_dot);
                const float angular_error = std::sqrt(std::max(alignment - 1.0F, 0.0F) / (static_cast<float>(dim) - 1.0F));
                error = std::sqrt(norm_square) * kRabitErrorEpsilon * angular_error;
                error = dot ? error : 2.0F * error;
            }
            error_factor[i] = error;
            float ex_res_dot = 0.0F;
            float ex_correction = 0.0F;
            if (ex) {
                ex_res_dot = quantize_ex_code(rotated.data(), dim, ex_bits, values.data());
                const float ex_bias = -(static_cast<float>(1U << ex_bits) - 0.5F);
                float ex_cent_dot = 0.0F;
                for (std::size_t j = 0; j < dim; ++j) {
                    const std::uint32_t bit = std::signbit(rotated[j]) ? 0U : 1U;
                    ex_cent_dot += rotated_centroid[j] * (static_cast<float>(((bit << ex_bits) + values[j])) + ex_bias);
                }
                ex_correction = factor_ratio(norm_square * ex_cent_dot, ex_res_dot);
                rq_pack_blocked_row(values.data(), dim, ex_bits, ex_codes.data() + i * ex_len);
            }
            if (!dot) {  // L2 (cosine is L2 over normalized rows)
                add[i] = norm_square + 2.0F * binary_correction;
                scale[i] = factor_ratio(-2.0F * norm_square, binary_res_dot);
                if (ex) {
                    add_ex[i] = norm_square + 2.0F * ex_correction;
                    scale_ex[i] = factor_ratio(-2.0F * norm_square, ex_res_dot);
                }
            } else {
                const float dot_base = 1.0F - residual_centroid_dot;
                add[i] = dot_base + binary_correction;
                scale[i] = factor_ratio(-norm_square, binary_res_dot);
                if (ex) {
                    add_ex[i] = dot_base + ex_correction;
                    scale_ex[i] = factor_ratio(-norm_square, ex_res_dot);
                }
            }
        }
    });
    // The 1-bit codes: packed per partition (Lance's default), or row by row (skip_transpose).
    if (!skip_transpose) {
        std::vector<std::uint8_t> packed(codes.size());
        for (std::size_t p = 0; p < partitions; ++p) {
            rq_pack_partition_codes(codes.data() + offsets[p] * code_bytes, lengths[p], code_bytes,
                                    packed.data() + offsets[p] * code_bytes);
        }
        codes.swap(packed);
    }

    std::vector<ArrowSchema> children;
    children.resize(ex ? 8U : 5U);
    OwnedSchema schema;
    OwnedArray rowid;
    OwnedArray codes_array;
    OwnedArray add_array;
    OwnedArray scale_array;
    OwnedArray error_array;
    OwnedArray ex_codes_array;
    OwnedArray add_ex_array;
    OwnedArray scale_ex_array;
    const auto float_column = [&](const std::vector<float>& values, OwnedArray& out) {
        if (ArrowArrayInitFromType(&out.a, NANOARROW_TYPE_FLOAT) != NANOARROW_OK ||
            ArrowArrayStartAppending(&out.a) != NANOARROW_OK) {
            return false;
        }
        for (const float value : values) {
            if (ArrowArrayAppendDouble(&out.a, static_cast<double>(value)) != NANOARROW_OK) {
                return false;
            }
        }
        return ArrowArrayFinishBuildingDefault(&out.a, nullptr) == NANOARROW_OK;
    };
    if (!scalar_schema("_rowid", NANOARROW_TYPE_UINT64, true, children[0]) ||
        !fsl_schema("_rabit_codes", NANOARROW_TYPE_UINT8, static_cast<std::int64_t>(code_bytes), true, children[1]) ||
        !scalar_schema("__add_factors", NANOARROW_TYPE_FLOAT, true, children[2]) ||
        !scalar_schema("__scale_factors", NANOARROW_TYPE_FLOAT, true, children[3]) ||
        !scalar_schema("__error_factors", NANOARROW_TYPE_FLOAT, true, children[4]) ||
        (ex && (!fsl_schema("__blocked_ex_codes", NANOARROW_TYPE_UINT8, static_cast<std::int64_t>(ex_len), true,
                            children[5]) ||
                !scalar_schema("__add_factors_ex", NANOARROW_TYPE_FLOAT, true, children[6]) ||
                !scalar_schema("__scale_factors_ex", NANOARROW_TYPE_FLOAT, true, children[7]))) ||
        !table_schema(children, schema.s)) {
        error = "out of memory";
        return false;
    }
    if (!u64_array(sorted_ids, rowid.a) ||
        !fsl_array(NANOARROW_TYPE_UINT8, 1, codes.data(), static_cast<std::int64_t>(n),
                   static_cast<std::int64_t>(code_bytes), codes_array.a) ||
        !float_column(add, add_array) || !float_column(scale, scale_array) || !float_column(error_factor, error_array)) {
        error = "out of memory";
        return false;
    }
    std::vector<ArrowArray*> arrays = {&rowid.a, &codes_array.a, &add_array.a, &scale_array.a, &error_array.a};
    if (ex) {
        if (!fsl_array(NANOARROW_TYPE_UINT8, 1, ex_codes.data(), static_cast<std::int64_t>(n),
                       static_cast<std::int64_t>(ex_len), ex_codes_array.a) ||
            !float_column(add_ex, add_ex_array) || !float_column(scale_ex, scale_ex_array)) {
            error = "out of memory";
            return false;
        }
        arrays.insert(arrays.end(), {&ex_codes_array.a, &add_ex_array.a, &scale_ex_array.a});
    }
    OwnedArray batch;
    if (!struct_batch(arrays, static_cast<std::int64_t>(n), batch.a, error)) {
        return false;
    }
    std::string sign_list = "[";
    for (std::size_t i = 0; i < signs.size(); ++i) {
        sign_list += (i == 0U ? "" : ",") + std::to_string(signs[i]);
    }
    sign_list += "]";
    const std::string storage = "{\"rotate_mat_position\":null,\"fast_rotation_signs\":" + sign_list +
                                ",\"rotation_type\":\"fast\",\"code_dim\":" + std::to_string(dim) +
                                ",\"num_bits\":" + std::to_string(num_bits) + ",\"packed\":" +
                                (skip_transpose ? "false" : "true") + ",\"query_estimator\":\"raw_query\"}";
    LanceFileExtras extras;
    extras.schema_metadata["lance:ivf"] = bytes_of("1");
    extras.schema_metadata["distance_type"] = bytes_of(metric_name);
    extras.schema_metadata["storage_metadata"] = bytes_of("[" + json_string(storage) + "]");
    extras.global_buffers.push_back(ivf_message(offsets, lengths, nullptr, nullptr));
    return write_file(dir, "auxiliary.idx", schema.s, batch.a, extras, files, error);
}

bool create_vector_index(const std::filesystem::path& dataset_path, const std::string& column,
                         const VectorIndexOptions& given, const index_build::SegmentTarget* target,
                         std::uint64_t& new_version, std::string& error) {
    error.clear();
    const index_build::VectorModel* model = target != nullptr ? target->model : nullptr;
    VectorIndexOptions options = given;
    if (model != nullptr) {
        // An existing segment's model: its type, metric and PQ settings, its partitions.
        options.type = model->type;
        options.metric = model->metric;
        options.num_bits = model->nbits;
        options.num_sub_vectors = static_cast<std::uint32_t>(model->m);
        options.num_partitions = static_cast<std::uint32_t>(model->partitions);
        options.hnsw_m = model->hnsw_m;
        options.hnsw_ef_construction = model->hnsw_ef_construction;
        options.hnsw_max_level = model->hnsw_max_level;
    }
    // The storage (PQ codes, SQ codes or floats) and the sub-index (FLAT, or an HNSW graph per partition).
    const bool pq = options.type == "IVF_PQ" || options.type == "IVF_HNSW_PQ";
    const bool sq_storage = options.type == "IVF_SQ" || options.type == "IVF_HNSW_SQ";
    const bool flat_storage = options.type == "IVF_FLAT" || options.type == "IVF_HNSW_FLAT";
    const bool hnsw = options.type.rfind("IVF_HNSW_", 0) == 0;
    const bool rq = options.type == "IVF_RQ";
    if (!pq && !sq_storage && !flat_storage && !rq) {
        error = "vector index type '" + options.type +
                "' is not supported (IVF_FLAT, IVF_SQ, IVF_PQ, IVF_RQ, IVF_HNSW_FLAT, IVF_HNSW_SQ and IVF_HNSW_PQ are)";
        return false;
    }
    // A Hamming index (over uint8 vectors, as pylance builds it) is IVF_FLAT: byte centroids and rows.
    const bool binary = options.metric == VectorMetric::Hamming;
    if (binary && options.type != "IVF_FLAT") {
        error = "vector index: a Hamming index must be IVF_FLAT";
        return false;
    }
    if (hnsw) {
        // lance-index HnswBuildParams::validate.
        if (options.hnsw_max_level == 0U || options.hnsw_max_level > 65535U) {
            error = "HnswBuildParams::max_level must be greater than 0, got " + std::to_string(options.hnsw_max_level);
            return false;
        }
        if (options.hnsw_m < 4U) {
            error = "HnswBuildParams::m must be at least 4 to avoid severely fragmented graphs, got " +
                    std::to_string(options.hnsw_m);
            return false;
        }
        if (options.hnsw_ef_construction < options.hnsw_m) {
            error = "HnswBuildParams::ef_construction must be at least m (" + std::to_string(options.hnsw_m) +
                    "), got " + std::to_string(options.hnsw_ef_construction);
            return false;
        }
    }
    if (pq && options.num_bits != 8U && options.num_bits != 4U) {
        error = "ProductQuantization: num_bits " + std::to_string(options.num_bits) + " not supported";
        return false;
    }
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (target != nullptr && target->manifest != nullptr) {
        manifest = *target->manifest;
        version = target->version;
    } else if (!load_read_version(dataset_path, options.read_version, manifest, version, error)) {
        return false;
    }
    // Training alone (target->trained) commits nothing either.
    const bool commit = target == nullptr || (target->out == nullptr && target->trained == nullptr);
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
    request.fragment_ids = target != nullptr ? target->fragments : nullptr;
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
    // A multivector column (a list of fixed-size lists): its vectors are the list's items, and Lance
    // builds these with cosine only (its message, lance-12 index/vector.rs).
    const bool multi = type->format != nullptr && std::string(type->format) == "+l";
    if (multi) {
        if (options.metric != VectorMetric::Cosine) {
            error = "Build Vector Index: multivector type supports only cosine distance";
            return false;
        }
        if (type->n_children != 1 || type->children[0] == nullptr) {
            error = "Vector column " + column + " must be a list of fixed-size lists of float32";
            return false;
        }
        type = type->children[0];
    }
    const std::string format = type->format == nullptr ? "" : type->format;
    const std::string item = type->n_children == 1 && type->children[0]->format != nullptr ? type->children[0]->format : "";
    // Training alone (IndicesBuilder) takes float16 and float64 vectors too, as Lance's does; the
    // index files nanolance writes hold float32. A multivector index over float16 vectors stores
    // float16, as pylance's does (auxiliary.idx "flat" is halffloat).
    const bool trains_only = target != nullptr && target->trained != nullptr;
    const bool half_storage = multi && item == "e";
    if (binary && item != "C") {
        error = "Distance type hamming does not support Float32 vectors, use l2, cosine or dot";
        return false;
    }
    if (!binary && item == "C") {
        error = std::string("Distance type ") + vector_metric_name(options.metric) +
                " does not support UInt8 vectors, use hamming";
        return false;
    }
    if (format.rfind("+w:", 0) != 0 ||
        !(item == "f" || item == "C" || half_storage || (trains_only && (item == "e" || item == "g")))) {
        error = "Vector column " + column + " must be a fixed-size list of float32 (got '" + format + "' of '" + item +
                "')";
        return false;
    }
    const std::size_t dim = static_cast<std::size_t>(std::strtoull(format.c_str() + 3, nullptr, 10));
    if (dim == 0U) {
        error = "Vector column " + column + " has dimension 0";
        return false;
    }
    if (model != nullptr &&
        (model->dim != dim ||
         (binary ? model->binary_centroids.size() : model->centroids.size()) != model->partitions * dim ||
         (pq && model->has_codebook && model->codebook.size() != (std::size_t{1} << model->nbits) * dim))) {
        error = "vector index on " + column + ": its model does not match the column's dimension " +
                std::to_string(dim);
        return false;
    }
    if (rq && dim % 8U != 0U) {
        error = "vector dimension must be divisible by 8 for IVF_RQ";
        return false;
    }
    if (rq && (options.num_bits < 1U || options.num_bits > 9U)) {
        error = "Invalid user input: IVF_RQ num_bits must be in 1..=9, got " + std::to_string(options.num_bits);
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
    std::vector<std::uint64_t> ids;  // one per vector: a multivector row's id repeats for each of its vectors
    std::vector<std::size_t> row_starts;  // multivector: the first vector of each row that has vectors (+ the end)
    const bool widen = item != "f" && !binary;
    const bool half = item == "e";
    std::vector<float> widened(widen ? dim : 0U);
    ByteRows binary_rows;  // a Hamming index: the vectors' bytes
    binary_rows.d = dim;
    // Appends the vector at `at` (its index in `fsl`, offset applied) unless it holds NaN or infinite
    // values; for cosine it is normalized first. False when it is left out.
    const auto add_vector = [&](const ArrowArrayView* fsl, std::int64_t at) {
        const ArrowArrayView* items = fsl->children[0];
        if (binary) {
            const std::uint8_t* src =
                items->buffer_views[1].data.as_uint8 + items->offset + at * static_cast<std::int64_t>(dim);
            binary_rows.v.insert(binary_rows.v.end(), src, src + dim);
            return true;
        }
        const float* v = nullptr;
        if (widen) {
            const std::int64_t first = items->offset + at * static_cast<std::int64_t>(dim);
            for (std::size_t j = 0; j < dim; ++j) {
                widened[j] = half ? ArrowHalfFloatToFloat(items->buffer_views[1].data.as_uint16[first + j])
                                  : static_cast<float>(items->buffer_views[1].data.as_double[first + j]);
            }
            v = widened.data();
        } else {
            v = items->buffer_views[1].data.as_float + items->offset + at * static_cast<std::int64_t>(dim);
        }
        if (!std::all_of(v, v + dim, [](float x) { return std::isfinite(x); })) {
            return false;
        }
        if (options.metric == VectorMetric::Cosine) {
            float norm = 0;
            for (std::size_t j = 0; j < dim; ++j) {
                norm += v[j] * v[j];
            }
            norm = std::sqrt(norm);
            if (!(norm > 0.0F)) {
                return false;
            }
            for (std::size_t j = 0; j < dim; ++j) {
                data.v.push_back(v[j] / norm);
            }
        } else {
            data.v.insert(data.v.end(), v, v + dim);
        }
        if (half_storage) {  // the index stores float16: the vector is rounded as it is stored
            for (std::size_t j = data.v.size() - dim; j < data.v.size(); ++j) {
                data.v[j] = ArrowHalfFloatToFloat(float16_bits(data.v[j]));
            }
        }
        return true;
    };
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
        const ArrowArrayView* addrs = view.children[view.n_children - 1];
        // A multivector column: `list` holds the rows' vector lists, `fsl` the vectors themselves.
        const ArrowArrayView* fsl = multi ? list->children[0] : list;
        for (std::int64_t r = 0; r < batch.length; ++r) {
            const std::int64_t row = view.offset + r;
            if (ArrowArrayViewIsNull(list, row)) {
                continue;
            }
            const std::uint64_t id = to_row_id(ArrowArrayViewGetUIntUnsafe(addrs, row));
            if (!multi) {
                if (add_vector(fsl, fsl->offset + row)) {
                    ids.push_back(id);
                }
                continue;
            }
            const std::size_t first = ids.size();
            const std::int32_t* offsets = list->buffer_views[1].data.as_int32;
            for (std::int32_t e = offsets[list->offset + row]; e < offsets[list->offset + row + 1]; ++e) {
                if (!ArrowArrayViewIsNull(fsl, e) && add_vector(fsl, fsl->offset + e)) {
                    ids.push_back(id);
                }
            }
            if (ids.size() > first) {
                row_starts.push_back(first);
            }
        }
        ArrowArrayViewReset(&view);
    }
    data.n = ids.size();
    binary_rows.n = ids.size();
    row_starts.push_back(data.n);  // the end of the last row

    std::size_t partitions = 0;
    if (options.num_partitions) {
        partitions = *options.num_partitions;
    } else {
        const std::size_t target = options.target_partition_size.value_or(default_partition_size(options.type));
        partitions = std::clamp<std::size_t>(data.n / std::max<std::size_t>(target, 1U), 1U, kMaxPartitions);
    }
    if (partitions == 0U) {
        error = "num_partitions must be at least 1";
        return false;
    }
    if (model == nullptr && partitions > data.n) {
        error = "KMeans cannot train " + std::to_string(partitions) + " centroids with " + std::to_string(data.n) +
                " vectors; choose a smaller K (< " + std::to_string(data.n) + ")";
        return false;
    }
    const std::size_t nc = std::size_t{1} << options.num_bits;
    if ((model == nullptr || !model->has_codebook) && pq && data.n < nc) {
        error = "Not enough rows to train PQ. Requires " + std::to_string(nc) + " rows but only " +
                std::to_string(data.n) + " available";
        return false;
    }
    std::mt19937_64 rng(options.seed.value_or(std::random_device{}() ^
                                              static_cast<std::uint64_t>(
                                                  std::chrono::steady_clock::now().time_since_epoch().count())));

    // IVF_RQ: the fast rotation's sign bytes (4 rounds of dim / 8): the caller's (build_rq_model), the
    // model's, else random from the seed.
    std::vector<std::uint8_t> rq_signs;
    if (rq) {
        const std::size_t sign_bytes = 4U * (dim / 8U);
        if (!options.rq_signs.empty()) {
            rq_signs = options.rq_signs;
        } else if (model != nullptr && !model->rq_signs.empty()) {
            rq_signs = model->rq_signs;
        } else {
            rq_signs.resize(sign_bytes);
            for (auto& b : rq_signs) {
                b = static_cast<std::uint8_t>(rng());
            }
        }
        if (rq_signs.size() != sign_bytes) {
            error = "IVF_RQ rotation has " + std::to_string(rq_signs.size()) + " sign bytes, not " +
                    std::to_string(sign_bytes) + " for dimension " + std::to_string(dim);
            return false;
        }
    }

    // The training sample. A multivector column is sampled by rows, as Lance samples it (lance-12
    // index/vector/utils.rs maybe_sample_training_data): about hint / (vectors per row) rows, at least
    // 128, each with all its vectors. Vectors per row: the mean, rounded up. Other columns: the vectors.
    const auto training_sample = [&](std::size_t hint) {
        if (!multi) {
            return sample_indices(data.n, hint, rng);
        }
        const std::size_t rows_n = row_starts.size() - 1U;
        std::vector<std::size_t> all(data.n);
        std::iota(all.begin(), all.end(), std::size_t{0});
        if (rows_n == 0U) {
            return all;
        }
        const std::size_t per_row = (data.n + rows_n - 1U) / rows_n;
        const std::size_t want = std::max<std::size_t>(128U, (hint + per_row - 1U) / per_row);
        if (rows_n <= want) {
            return all;
        }
        std::vector<std::size_t> out;
        for (const auto s : sample_indices(rows_n, want, rng)) {
            for (std::size_t v = row_starts[s]; v < row_starts[s + 1U]; ++v) {
                out.push_back(v);
            }
        }
        return out;
    };

    // IVF: train on a sample, then every vector to its nearest centroid.
    const bool dot_metric = options.metric == VectorMetric::Dot;
    const index_build::Progress* progress = target != nullptr ? target->progress : nullptr;
    index_build::VectorModel* trained = target != nullptr ? target->trained : nullptr;
    double loss = 0;
    std::vector<float> centroids;
    std::vector<std::uint8_t> binary_centroids;  // Hamming: [partitions][dim] bytes; `centroids` stays empty
    index_build::stage(progress, "train_ivf", options.max_iters, "iterations", [&] {
        if (binary) {
            if (model != nullptr) {
                binary_centroids = model->binary_centroids;
            } else {
                const auto train =
                    gather_bytes(binary_rows, training_sample(std::size_t{options.sample_rate} * partitions));
                binary_centroids = kmodes(train, partitions, options.max_iters, rng, loss);
            }
            return true;
        }
        if (model != nullptr) {
            centroids = model->centroids;
        } else {
            const auto train =
                gather(data, training_sample(std::size_t{options.sample_rate} * partitions));
            centroids = kmeans(train, partitions, dot_metric, options.max_iters, rng, loss);
        }
        if (half_storage) {  // stored as float16: assigned against the rounded centroids
            for (auto& c : centroids) {
                c = ArrowHalfFloatToFloat(float16_bits(c));
            }
        }
        return true;
    });
    if (trained != nullptr) {
        trained->type = options.type;
        trained->metric = options.metric;
        trained->dim = dim;
        trained->partitions = partitions;
        trained->centroids = centroids;
        trained->binary_centroids = binary_centroids;
        trained->nbits = options.num_bits;
        trained->m = pq ? options.num_sub_vectors : 0U;
        trained->has_codebook = false;
        trained->has_sq = false;
        trained->rq_signs = rq_signs;
        if (!pq) {
            return true;
        }
    }
    // A Hamming index keeps its partitions as they are: Lance's split and join are float k-means.
    const bool shuffle_first = model != nullptr && target->rebalance_target != 0U && !binary;
    std::vector<std::uint32_t> label;
    bool labelled = false;  // the codebook's sample covered every row: the labels are complete already
    const auto shuffle = [&] {
        index_build::stage(progress, "shuffle", data.n, "rows", [&] {
            if (!labelled) {
                if (binary) {
                    hamming_assign(binary_rows, binary_centroids, partitions, label, nullptr);
                } else {
                    assign(data, centroids, partitions, dot_metric, label, nullptr);
                }
            }
            if (model != nullptr && target->rebalance_target != 0U && !binary) {
                adjust_partitions(data, centroids, partitions, label, target->rebalance_target, dot_metric,
                                  options.max_iters);
            }
            if (progress != nullptr) {  // every row is labelled
                (*progress)(1, "shuffle", data.n, "rows", data.n);
            }
            return true;
        });
    };
    if (shuffle_first) {
        shuffle();
    }
    // PQ: a codebook per sub-vector from a sample of residuals; every row's codes.
    const std::size_t m = pq ? options.num_sub_vectors : 0U;
    const std::size_t w = pq ? dim / m : 0U;
    const std::size_t code_bytes = pq ? (options.num_bits == 4U ? m / 2U : m) : 0U;
    std::vector<float> codebook;           // [m][nc][w]
    std::vector<std::uint8_t> codes;       // per partition: [code_bytes][n_p], partitions in order
    std::vector<std::uint8_t> pq_rows;     // HNSW over PQ codes: [row][code_bytes], rows in `order`
    std::vector<float> pq_table;           // HNSW over PQ codes: Lance's pairwise table
    const auto residual = [&](std::size_t r, float* out) {
        const float* v = data.row(r);
        const float* c = centroids.data() + static_cast<std::size_t>(label[r]) * dim;
        for (std::size_t j = 0; j < dim; ++j) {
            out[j] = dot_metric ? v[j] : v[j] - c[j];
        }
    };
    if (pq) {
        codebook.resize(m * nc * w);
        if (progress != nullptr) {
            (*progress)(0, "train_quantizer", 0, "", 0);
        }
        if (model != nullptr && model->has_codebook) {
            codebook = model->codebook;
        } else {
        if (!shuffle_first) {
            label.resize(data.n);  // the sample's rows are labelled below; the shuffle labels every row later
        }
        const auto picked = training_sample(std::size_t{options.sample_rate} * nc);
        if (!shuffle_first) {
            std::vector<std::uint32_t> picked_label;
            assign(gather(data, picked), centroids, partitions, dot_metric, picked_label, nullptr);
            for (std::size_t i = 0; i < picked.size(); ++i) {
                label[picked[i]] = picked_label[i];
            }
            labelled = picked.size() == data.n;  // a sample of every row (k >= n) is the identity order
        }
        std::vector<float> res(dim);
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
        }
        if (half_storage) {  // stored as float16: the codes are computed against the rounded codebook
            for (auto& c : codebook) {
                c = ArrowHalfFloatToFloat(float16_bits(c));
            }
        }
        if (progress != nullptr) {
            (*progress)(2, "train_quantizer", 0, "", 0);
        }
    }
    if (!shuffle_first) {
        shuffle();
    }
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

    if (pq) {
        if (trained != nullptr) {
            trained->codebook = codebook;
            trained->has_codebook = true;
            return true;
        }
        if (progress != nullptr) {
            (*progress)(0, "merge_partitions", partitions, "partitions", 0);
        }
        if (hnsw) {
            pq_table = pairwise_table(codebook, m, nc, w, options.metric);
        }
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
        // Transposed per partition: [code byte][row of the partition]; skip_transpose: [row][code byte].
        codes.resize(data.n * code_bytes);
        if (hnsw) {
            pq_rows.resize(data.n * code_bytes);
        }
        for (std::size_t p = 0; p < partitions; ++p) {
            const std::size_t first = offsets[p];
            const std::size_t n = lengths[p];
            std::uint8_t* out = codes.data() + first * code_bytes;
            for (std::size_t r = 0; r < n; ++r) {
                const std::uint8_t* rc = row_codes.data() + (first + r) * m;
                for (std::size_t b = 0; b < code_bytes; ++b) {
                    const std::uint8_t code = options.num_bits == 4U
                                                  ? static_cast<std::uint8_t>((rc[2U * b] & 0x0FU) | (rc[2U * b + 1U] << 4U))
                                                  : rc[b];
                    if (options.skip_transpose) {
                        out[r * code_bytes + b] = code;
                    } else {
                        out[b * n + r] = code;
                    }
                    if (hnsw) {
                        pq_rows[(first + r) * code_bytes + b] = code;
                    }
                }
            }
        }
    }

    // SQ: the bounds of a sample's values (or the model's), every row's 8-bit codes (Lance's
    // scale_to_u8). HNSW: each partition's graph over its storage: SQ codes, floats or PQ codes.
    double sq_start = 0;
    double sq_end = 0;
    std::vector<std::uint8_t> sq;  // [row][dim], rows in `order`
    std::vector<float> flat_rows;  // HNSW_FLAT: [row][dim], rows in `order`
    std::vector<hnsw_build::Graph> graphs;
    if (sq_storage) {
        if (progress != nullptr) {
            (*progress)(0, "train_quantizer", 0, "", 0);
        }
        if (model != nullptr && model->has_sq) {
            sq_start = model->sq_start;
            sq_end = model->sq_end;
        } else {
            sq_start = std::numeric_limits<double>::max();
            sq_end = std::numeric_limits<double>::lowest();
            for (const auto r : training_sample(std::size_t{options.sample_rate} * 256U)) {
                const float* x = data.row(r);
                for (std::size_t j = 0; j < dim; ++j) {
                    sq_start = std::min(sq_start, static_cast<double>(x[j]));
                    sq_end = std::max(sq_end, static_cast<double>(x[j]));
                }
            }
        }
        if (progress != nullptr) {
            (*progress)(2, "train_quantizer", 0, "", 0);
            (*progress)(0, "merge_partitions", partitions, "partitions", 0);
        }
        sq.resize(data.n * dim);
        const double range = sq_end - sq_start;
        parallel::for_each((data.n + 4095U) / 4096U, [&](std::size_t block) {
            for (std::size_t i = block * 4096U; i < std::min(data.n, (block + 1U) * 4096U); ++i) {
                const float* x = data.row(order[i]);
                std::uint8_t* out = sq.data() + i * dim;
                for (std::size_t j = 0; j < dim; ++j) {
                    const double s = range == 0.0 ? 0.0 : (static_cast<double>(x[j]) - sq_start) * 255.0 / range;
                    out[j] = !(s > 0.0) ? 0 : (s >= 255.0 ? 255 : static_cast<std::uint8_t>(s));
                }
            }
        });
    }
    if (hnsw) {
        if (progress != nullptr && !sq_storage) {
            (*progress)(0, "train_quantizer", 0, "", 0);
            (*progress)(2, "train_quantizer", 0, "", 0);
            (*progress)(0, "merge_partitions", partitions, "partitions", 0);
        }
        if (flat_storage) {
            flat_rows.resize(data.n * dim);
            for (std::size_t i = 0; i < data.n; ++i) {
                std::memcpy(flat_rows.data() + i * dim, data.row(order[i]), dim * sizeof(float));
            }
        }
        hnsw_build::Params params;
        params.m = options.hnsw_m;
        params.ef_construction = options.hnsw_ef_construction;
        params.max_level = static_cast<std::uint16_t>(options.hnsw_max_level);
        graphs.resize(partitions);
        for (std::size_t p = 0; p < partitions; ++p) {
            hnsw_build::Codes codes;
            codes.n = lengths[p];
            codes.dim = dim;
            codes.dot = dot_metric;
            if (flat_storage) {
                codes.storage = hnsw_build::Storage::Flat;
                codes.vectors = flat_rows.data() + offsets[p] * dim;
            } else if (pq) {
                codes.storage = hnsw_build::Storage::Pq;
                codes.data = pq_rows.data() + offsets[p] * code_bytes;
                codes.dim = code_bytes;
                codes.pq_table = pq_table.data();
                codes.m = m;
                codes.nbits = options.num_bits;
            } else {
                codes.data = sq.data() + offsets[p] * dim;
                codes.start = sq_start;
                codes.end = sq_end;
            }
            graphs[p] = hnsw_build::build(codes, params);
            if (progress != nullptr) {
                (*progress)(1, "merge_partitions", 0, "", p + 1U);
            }
        }
    }
    if (!pq && !sq_storage && progress != nullptr) {
        (*progress)(0, "train_quantizer", 0, "", 0);
        (*progress)(2, "train_quantizer", 0, "", 0);
        (*progress)(0, "merge_partitions", partitions, "partitions", 0);
    }

    // The files.
    const auto uuid = target != nullptr && target->uuid != nullptr ? *target->uuid : new_uuid();
    const auto dir = dataset_path / "_indices" / pb::uuid_string(uuid);
    if (!index_files::create_index_dir(dir, error)) {
        return false;
    }
    const auto fail = [&](const std::string& why) {
        error = why;
        index_files::remove_index_dir(dir);
        return false;
    };
    const std::string metric_name = vector_metric_name(options.metric);
    std::vector<WrittenFile> files;
    if (rq) {
        if (!write_rq_auxiliary(data, centroids, order, offsets, lengths, sorted_ids, rq_signs, options.num_bits,
                                dot_metric, options.skip_transpose, metric_name, dir, files, error)) {
            return fail(error);
        }
    } else {
        // auxiliary.idx: the rows, grouped by partition.
        std::vector<ArrowSchema> children(2);
        OwnedSchema schema;
        if (!scalar_schema("_rowid", NANOARROW_TYPE_UINT64, true, children[0]) ||
            !(pq     ? fsl_schema("__pq_code", NANOARROW_TYPE_UINT8, static_cast<std::int64_t>(code_bytes), true,
                                  children[1])
              : sq_storage ? fsl_schema("__sq_code", NANOARROW_TYPE_UINT8, static_cast<std::int64_t>(dim), true, children[1])
                     : fsl_schema("flat",
                                  binary ? NANOARROW_TYPE_UINT8
                                         : half_storage ? NANOARROW_TYPE_HALF_FLOAT : NANOARROW_TYPE_FLOAT,
                                  static_cast<std::int64_t>(dim), false, children[1])) ||
            !table_schema(children, schema.s)) {
            return fail("out of memory");
        }
        OwnedArray rowid;
        OwnedArray values;
        std::vector<float> flat;
        std::vector<std::uint16_t> flat_half;  // a float16 multivector index: the same values, as halffloat
        std::vector<std::uint8_t> flat_bytes;  // a Hamming index: the rows' bytes, grouped by partition
        if (flat_storage && binary) {
            flat_bytes.resize(data.n * dim);
            for (std::size_t i = 0; i < data.n; ++i) {
                std::memcpy(flat_bytes.data() + i * dim, binary_rows.row(order[i]), dim);
            }
        } else if (flat_storage && half_storage) {
            flat_half.resize(data.n * dim);
            for (std::size_t i = 0; i < data.n; ++i) {
                for (std::size_t j = 0; j < dim; ++j) {
                    flat_half[i * dim + j] = float16_bits(data.row(order[i])[j]);
                }
            }
        } else if (flat_storage) {
            flat.resize(data.n * dim);
            for (std::size_t i = 0; i < data.n; ++i) {
                std::memcpy(flat.data() + i * dim, data.row(order[i]), dim * sizeof(float));
            }
        }
        if (!u64_array(sorted_ids, rowid.a) ||
            !(pq     ? fsl_array(NANOARROW_TYPE_UINT8, 1, codes.data(), static_cast<std::int64_t>(data.n),
                                 static_cast<std::int64_t>(code_bytes), values.a)
              : sq_storage ? fsl_array(NANOARROW_TYPE_UINT8, 1, sq.data(), static_cast<std::int64_t>(data.n),
                                 static_cast<std::int64_t>(dim), values.a)
                     : binary ? fsl_array(NANOARROW_TYPE_UINT8, 1, flat_bytes.data(), static_cast<std::int64_t>(data.n),
                                          static_cast<std::int64_t>(dim), values.a)
                     : half_storage
                         ? fsl_array(NANOARROW_TYPE_HALF_FLOAT, 2, flat_half.data(), static_cast<std::int64_t>(data.n),
                                     static_cast<std::int64_t>(dim), values.a)
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
                      ",\"codebook_tensor\":[],\"transposed\":" + (options.skip_transpose ? "false" : "true") + "}";
        } else if (sq_storage) {
            storage = "{\"dim\":" + std::to_string(dim) + ",\"num_bits\":8,\"bounds\":{\"start\":" +
                      json_double(sq_start) + ",\"end\":" + json_double(sq_end) + "}}";
        } else {
            storage = "{\"dim\":" + std::to_string(dim) + "}";
        }
        extras.schema_metadata["storage_metadata"] = bytes_of("[" + json_string(storage) + "]");
        extras.global_buffers.push_back(ivf_message(offsets, lengths, nullptr, nullptr));
        if (pq) {
            extras.global_buffers.push_back(half_storage ? tensor_message_half({nc, dim}, codebook)
                                                         : tensor_message({nc, dim}, codebook));
        }
        if (!write_file(dir, "auxiliary.idx", schema.s, batch.a, extras, files, error)) {
            return fail(error);
        }
    }
    if (hnsw) {
        // index.idx: the graphs, partition after partition, each level after level (a row per node
        // on the level: its id, neighbours and their distances); the centroids.
        std::vector<std::uint32_t> vector_ids;
        std::vector<std::int32_t> list_offsets{0};
        std::vector<std::uint32_t> neighbor_ids;
        std::vector<float> neighbor_dists;
        std::vector<std::uint64_t> graph_offsets(partitions, 0);
        std::vector<std::uint64_t> graph_lengths(partitions, 0);
        std::string hnsw_list = "[";
        for (std::size_t p = 0; p < partitions; ++p) {
            const auto& g = graphs[p];
            graph_offsets[p] = vector_ids.size();
            std::string level_offsets = "[0";
            std::uint64_t at = 0;
            for (std::size_t level = 0; level < g.level_count.size(); ++level) {
                for (std::size_t i = 0; i < g.nodes.size(); ++i) {
                    if (level >= g.nodes[i].size()) {
                        continue;
                    }
                    vector_ids.push_back(static_cast<std::uint32_t>(i));
                    for (const auto& e : g.nodes[i][level]) {
                        neighbor_ids.push_back(e.id);
                        neighbor_dists.push_back(e.dist);
                    }
                    if (neighbor_ids.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
                        return fail("vector index: an HNSW graph with more than 2^31 edges");
                    }
                    list_offsets.push_back(static_cast<std::int32_t>(neighbor_ids.size()));
                }
                at += g.level_count[level];
                level_offsets += "," + std::to_string(at);
            }
            graph_lengths[p] = vector_ids.size() - graph_offsets[p];
            const std::string meta = "{\"entry_point\":" + std::to_string(g.entry_point) +
                                     ",\"params\":{\"max_level\":" + std::to_string(options.hnsw_max_level) +
                                     ",\"m\":" + std::to_string(options.hnsw_m) +
                                     ",\"ef_construction\":" + std::to_string(options.hnsw_ef_construction) +
                                     ",\"prefetch_distance\":2},\"level_offsets\":" + level_offsets + "]}";
            hnsw_list += (p == 0U ? "" : ",") + json_string(meta);
        }
        hnsw_list += "]";
        std::vector<ArrowSchema> children(3);
        OwnedSchema schema;
        OwnedArray ids_array;
        OwnedArray neighbors_array;
        OwnedArray dists_array;
        OwnedArray batch;
        std::string ignored;
        if (!scalar_schema("__vector_id", NANOARROW_TYPE_UINT32, true, children[0]) ||
            !list_schema("__neighbors", NANOARROW_TYPE_UINT32, children[1]) ||
            !list_schema("_distance", NANOARROW_TYPE_FLOAT, children[2]) || !table_schema(children, schema.s) ||
            !index_files::uint_array(NANOARROW_TYPE_UINT32, vector_ids.data(),
                                     static_cast<std::int64_t>(vector_ids.size()), 4, ids_array.a, ignored) ||
            !list_array(NANOARROW_TYPE_UINT32, 4, list_offsets, neighbor_ids.data(), neighbors_array.a) ||
            !list_array(NANOARROW_TYPE_FLOAT, 4, list_offsets, neighbor_dists.data(), dists_array.a) ||
            !struct_batch({&ids_array.a, &neighbors_array.a, &dists_array.a},
                          static_cast<std::int64_t>(vector_ids.size()), batch.a, error)) {
            return fail(error.empty() ? "out of memory" : error);
        }
        LanceFileExtras extras;
        extras.schema_metadata["lance:hnsw"] = bytes_of(hnsw_list);
        extras.schema_metadata["lance:index"] =
            bytes_of("{\"type\":\"" + options.type + "\",\"distance_type\":\"" + metric_name + "\"}");
        extras.schema_metadata["lance:ivf"] = bytes_of("1");
        const auto tensor = half_storage ? tensor_message_half({partitions, dim}, centroids)
                                         : tensor_message({partitions, dim}, centroids);
        extras.global_buffers.push_back(ivf_message(graph_offsets, graph_lengths, &tensor, &loss));
        if (!write_file(dir, "index.idx", schema.s, batch.a, extras, files, error)) {
            return fail(error);
        }
    } else {
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
        const auto tensor = binary ? tensor_message_bytes({partitions, dim}, binary_centroids)
                                   : half_storage ? tensor_message_half({partitions, dim}, centroids)
                                                  : tensor_message({partitions, dim}, centroids);
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
        put_varint(details, options.metric == VectorMetric::Cosine  ? 1U
                            : options.metric == VectorMetric::Hamming ? 3U
                                                                      : 2U);
    }
    if (!options.num_partitions) {
        put_key(details, 2, 0);
        put_varint(details, options.target_partition_size.value_or(default_partition_size(options.type)));
    }
    if (hnsw) {
        std::vector<std::uint8_t> hnsw_details;  // HnswParameters { max_connections, construction_ef, max_level }
        put_key(hnsw_details, 1, 0);
        put_varint(hnsw_details, options.hnsw_m);
        put_key(hnsw_details, 2, 0);
        put_varint(hnsw_details, options.hnsw_ef_construction);
        put_key(hnsw_details, 3, 0);
        put_varint(hnsw_details, options.hnsw_max_level);
        put_bytes(details, 3, hnsw_details.data(), hnsw_details.size());
    }
    if (pq) {
        std::vector<std::uint8_t> pq_details;
        put_key(pq_details, 1, 0);
        put_varint(pq_details, options.num_bits);
        put_key(pq_details, 2, 0);
        put_varint(pq_details, m);
        put_bytes(details, 4, pq_details.data(), pq_details.size());
    } else if (sq_storage) {
        std::vector<std::uint8_t> sq_details;  // ScalarQuantization { num_bits }
        put_key(sq_details, 1, 0);
        put_varint(sq_details, 8U);
        put_bytes(details, 5, sq_details.data(), sq_details.size());
    } else if (rq) {
        std::vector<std::uint8_t> rq_details;  // RabitQuantization { num_bits }
        put_key(rq_details, 1, 0);
        put_varint(rq_details, options.num_bits);
        put_bytes(details, 6, rq_details.data(), rq_details.size());
    } else {
        put_bytes(details, 8, nullptr, 0);
    }
    const auto hint = [&](const std::string& key, const std::string& value) {
        std::vector<std::uint8_t> entry;
        put_bytes(entry, 1, key.data(), key.size());
        put_bytes(entry, 2, value.data(), value.size());
        put_bytes(details, 9, entry.data(), entry.size());
    };
    if (hnsw) {
        hint("lance.hnsw.prefetch_distance", "2");
    }
    hint("lance.ivf.max_iters", std::to_string(options.max_iters));
    hint("lance.ivf.sample_rate", std::to_string(options.sample_rate));
    if (rq) {
        hint("lance.skip_transpose", options.skip_transpose ? "true" : "false");
    }
    if (pq) {
        hint("lance.pq.max_iters", std::to_string(options.max_iters));
        hint("lance.pq.sample_rate", std::to_string(options.sample_rate));
        hint("lance.pq.kmeans_redos", "1");
    }
    if (sq_storage) {
        hint("lance.sq.sample_rate", std::to_string(options.sample_rate));
    }
    if (options.skip_transpose) {  // as pylance stores it; its default ("false") is not written here
        hint("lance.skip_transpose", "true");
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
    if (target != nullptr && target->details != nullptr) {
        details = *target->details;
    }
    if (progress != nullptr) {
        if (!hnsw) {  // the files hold every partition: each is reported once they are written
            for (std::size_t p = 1; p <= partitions; ++p) {
                (*progress)(1, "merge_partitions", partitions, "partitions", p);
            }
            (*progress)(2, "merge_partitions", partitions, "partitions", partitions);
        } else {
            (*progress)(2, "merge_partitions", 0, "", 0);
        }
    }
    auto entry = pb::make_index_metadata(uuid, {field->id}, name, version, fragment_ids,
                                         "/lance.index.pb.VectorIndexDetails", 1, static_cast<std::uint64_t>(now),
                                         index_files, details);
    if (!commit) {
        *target->out = std::move(entry);
        return true;
    }
    if (existing != manifest.indices.end()) {
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

bool dataset_create_vector_index(const std::filesystem::path& dataset_path, const std::string& column,
                                 const VectorIndexOptions& options, std::uint64_t& new_version, std::string& error) {
    if (options.metric == VectorMetric::Hamming && !options.ivf_centroids.empty()) {
        error = "vector index: supplied ivf_centroids are not supported for a Hamming index (its centroids are bytes)";
        return false;
    }
    if (options.ivf_centroids.empty() && !options.progress) {
        return retry_from_read_version(
            options.read_version,
            [&](ReadVersion from) {
                VectorIndexOptions o = options;
                o.read_version = from;
                return create_vector_index(dataset_path, column, o, nullptr, new_version, error);
            },
            error);
    }
    // Supplied centroids (and codebook) are a model, as a distributed build's segments share theirs;
    // the build still commits. Its progress goes to the callback.
    index_build::VectorModel model;
    index_build::Progress progress = options.progress;
    index_build::SegmentTarget target;
    if (!options.ivf_centroids.empty()) {
        if (options.num_partitions.value_or(0U) == 0U ||
            options.ivf_centroids.size() % options.num_partitions.value_or(1U) != 0U) {
            error = "ivf_centroids does not match num_partitions";
            return false;
        }
        model.type = options.type;
        model.metric = options.metric;
        model.partitions = *options.num_partitions;
        model.dim = options.ivf_centroids.size() / model.partitions;
        model.centroids = options.ivf_centroids;
        model.nbits = options.num_bits;
        model.m = options.type == "IVF_PQ" || options.type == "IVF_HNSW_PQ" ? options.num_sub_vectors : 0U;
        model.has_codebook = !options.pq_codebook.empty();
        model.codebook = options.pq_codebook;
        model.has_sq = false;
        model.hnsw_m = options.hnsw_m;
        model.hnsw_ef_construction = options.hnsw_ef_construction;
        model.hnsw_max_level = options.hnsw_max_level;
        target.model = &model;
    }
    if (options.progress) {
        target.progress = &progress;
    }
    return create_vector_index(dataset_path, column, options, &target, new_version, error);
}

bool index_build::build_vector_segment(const std::filesystem::path& dataset_path, const std::string& column,
                                       const VectorIndexOptions& options, const SegmentTarget& target,
                                       std::string& error) {
    if (options.metric == VectorMetric::Hamming && target.model != nullptr && target.model->binary_centroids.empty()) {
        error = "vector index: a Hamming index's model must hold byte centroids (not float ones)";
        return false;
    }
    std::uint64_t unused = 0;
    return create_vector_index(dataset_path, column, options, &target, unused, error);
}

}  // namespace nano_lance
