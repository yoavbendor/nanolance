// SPDX-License-Identifier: Apache-2.0
//
// The HNSW graph of one IVF_HNSW_SQ partition, after lance-index 12's HnswBuilder
// (vector/hnsw/builder.rs): node levels from a SmallRng seeded with 42, insertion with a beam of
// ef_construction, Algorithm 4's neighbour heuristic (keeping pruned connections), reciprocal edges
// pruned to 2m on level 0 and m above, then a pass linking nodes that level 0 cannot reach from the
// entry point. Distances are SQ distances between codes (SQDistCalculator from a stored id).

#include "hnsw_build.hpp"

#include "nanolance/parallel.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <limits>
#include <optional>
#include <queue>
#include <unordered_map>

namespace nano_lance::hnsw_build {
namespace {

/// rand 0.9's SmallRng on 64-bit targets: Xoshiro256++, seeded by SplitMix64.
class SmallRng {
public:
    explicit SmallRng(std::uint64_t seed) {
        for (auto& s : s_) {
            seed += 0x9e3779b97f4a7c15ULL;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
            s = z ^ (z >> 31U);
        }
    }
    std::uint64_t next_u64() {
        const std::uint64_t res = rotl(s_[0] + s_[3], 23) + s_[0];
        const std::uint64_t t = s_[1] << 17U;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return res;
    }
    /// rng.random::<f32>(): the top 24 bits of next_u32, in [0, 1).
    float next_f32() {
        const auto u = static_cast<std::uint32_t>(next_u64() >> 32U);
        return static_cast<float>(u >> 8U) * (1.0F / 16777216.0F);
    }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    std::uint64_t s_[4]{};
};

constexpr std::uint64_t kLevelSeed = 42;  // HNSW_LEVEL_RNG_SEED
constexpr std::size_t kChunk = 65536;     // 255^2 * 65536 < 2^32: a chunk's sum fits in 32 bits

/// Squared L2 between two code rows, summed in 32-bit chunks (which compilers vectorize).
std::uint64_t l2_u8(const std::uint8_t* x, const std::uint8_t* y, std::size_t dim) {
    std::uint64_t sum = 0;
    for (std::size_t at = 0; at < dim; at += kChunk) {
        std::uint32_t part = 0;
        for (std::size_t j = at; j < std::min(dim, at + kChunk); ++j) {
            const int e = static_cast<int>(x[j]) - static_cast<int>(y[j]);
            part += static_cast<std::uint32_t>(e * e);
        }
        sum += part;
    }
    return sum;
}

/// OrderedFloat's order (NaN greatest).
bool less(float a, float b) {
    const bool an = std::isnan(a);
    const bool bn = std::isnan(b);
    if (an || bn) {
        return !an && bn;
    }
    return a < b;
}

/// A tiny lock per node (Lance's RwLock): held only to read or rewrite one node's lists.
class SpinLock {
public:
    void lock() {
        while (flag_.test_and_set(std::memory_order_acquire)) {
        }
    }
    void unlock() { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

class Builder {
public:
    Builder(const Codes& codes, const Params& params) : c_(codes), p_(params) {
        value_scale_ = static_cast<float>(c_.end - c_.start) / 255.0F;
        scale_ = value_scale_ * value_scale_;
        lower_ = static_cast<float>(c_.start);
        if (c_.dot) {
            sums_.resize(c_.n);
            for (std::size_t i = 0; i < c_.n; ++i) {
                std::uint32_t s = 0;
                for (std::size_t j = 0; j < c_.dim; ++j) {
                    s += code(static_cast<std::uint32_t>(i))[j];
                }
                sums_[i] = static_cast<float>(s);
            }
        }
    }

    Graph run() {
        Graph g;
        g.level_count.assign(p_.max_level, 0);
        const std::size_t n = c_.n;
        g.nodes.resize(n);
        if (n == 0U) {
            return g;
        }
        // Levels, from Lance's fixed-seed draws: the first node of the highest level is the entry.
        SmallRng rng(kLevelSeed);
        const float ml = 1.0F / std::log(static_cast<float>(p_.m));
        std::uint16_t highest = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const float x = -std::log(rng.next_f32()) * ml;
            // Rust's `as u16` saturates (and -ln(0) is infinite).
            const auto drawn = !(x < 65535.0F) ? std::uint16_t{65535} : static_cast<std::uint16_t>(x);
            const auto level = std::min<std::uint16_t>(drawn, static_cast<std::uint16_t>(p_.max_level - 1U));
            if (level > highest) {
                highest = level;
                g.entry_point = static_cast<std::uint32_t>(i);
            }
            g.nodes[i].resize(level + 1U);
        }
        nodes_ = &g.nodes;
        entry_ = g.entry_point;
        locks_ = std::vector<SpinLock>(n);
        std::vector<std::atomic<std::size_t>> counts(p_.max_level);
        for (std::size_t l = 0; l < p_.max_level; ++l) {
            counts[l] = l < g.nodes[entry_].size() ? 1U : 0U;
        }
        counts_ = &counts;

        // Insert every other node, in order, on nanolance's threads.
        std::atomic<std::size_t> next{0};
        const std::size_t workers = std::min<std::size_t>(parallel::threads(), n);
        parallel::for_each(workers, [&](std::size_t) {
            Scratch scratch(n);
            for (;;) {
                const std::size_t i = next.fetch_add(1);
                if (i >= n) {
                    break;
                }
                if (i != entry_) {
                    insert(static_cast<std::uint32_t>(i), scratch);
                }
            }
        });
        connect_stranded();
        for (std::size_t l = 0; l < p_.max_level; ++l) {
            g.level_count[l] = counts[l].load();
        }
        return g;
    }

private:
    struct Scratch {
        explicit Scratch(std::size_t n) : seen(n, 0U) {}
        std::vector<std::uint32_t> seen;  // generation stamps
        std::uint32_t generation = 0;
        std::vector<std::uint32_t> ids;
        void reset() {
            if (++generation == 0U) {
                std::fill(seen.begin(), seen.end(), 0U);
                generation = 1;
            }
        }
        bool visit(std::uint32_t id) {
            if (seen[id] == generation) {
                return false;
            }
            seen[id] = generation;
            return true;
        }
    };

    const std::uint8_t* code(std::uint32_t id) const { return c_.data + static_cast<std::size_t>(id) * c_.dim; }

    /// SQDistCalculator::from_id(a).distance(b).
    float dist(std::uint32_t a, std::uint32_t b) const {
        const std::uint8_t* x = code(a);
        const std::uint8_t* y = code(b);
        if (c_.dot) {
            std::uint64_t dot = 0;
            for (std::size_t at = 0; at < c_.dim; at += kChunk) {  // u32 sums of at most 2^16 products
                std::uint32_t part = 0;
                for (std::size_t j = at; j < std::min(c_.dim, at + kChunk); ++j) {
                    part += static_cast<std::uint32_t>(x[j]) * y[j];
                }
                dot += part;
            }
            const float d = static_cast<float>(c_.dim);
            return 1.0F - (d * lower_ * lower_ + lower_ * value_scale_ * (sums_[b] + sums_[a]) +
                           scale_ * static_cast<float>(dot));
        }
        return static_cast<float>(l2_u8(x, y, c_.dim)) * scale_;
    }

    /// A node's neighbour ids on `level`, copied under its lock.
    void neighbors(std::uint32_t id, std::size_t level, std::vector<std::uint32_t>& out) {
        out.clear();
        locks_[id].lock();
        const auto& levels = (*nodes_)[id];
        if (level < levels.size()) {
            for (const auto& e : levels[level]) {
                out.push_back(e.id);
            }
        }
        locks_[id].unlock();
    }

    Edge greedy(Edge ep, std::uint32_t q, std::size_t level, Scratch& s) {
        for (;;) {
            std::optional<std::uint32_t> next;
            neighbors(ep.id, level, s.ids);
            for (const auto nb : s.ids) {
                const float d = dist(q, nb);
                if (less(d, ep.dist)) {
                    ep.dist = d;
                    next = nb;
                }
            }
            if (!next) {
                return ep;
            }
            ep.id = *next;
        }
    }

    /// beam_search with ef_construction and no filter: ascending by distance.
    std::vector<Edge> beam(const Edge& ep, std::uint32_t q, std::size_t level, Scratch& s) {
        const auto far_first = [](const Edge& a, const Edge& b) { return less(a.dist, b.dist); };
        const auto near_first = [](const Edge& a, const Edge& b) { return less(b.dist, a.dist); };
        std::priority_queue<Edge, std::vector<Edge>, decltype(near_first)> candidates(near_first);
        std::priority_queue<Edge, std::vector<Edge>, decltype(far_first)> results(far_first);
        const std::size_t ef = p_.ef_construction;
        s.reset();
        s.visit(ep.id);
        candidates.push(ep);
        results.push(ep);
        std::vector<std::uint32_t> ids;
        while (!candidates.empty()) {
            const Edge current = candidates.top();
            candidates.pop();
            if (less(results.top().dist, current.dist) && results.size() == ef) {
                break;
            }
            neighbors(current.id, level, ids);
            for (const auto id : ids) {
                if (!s.visit(id)) {
                    continue;
                }
                const float d = dist(q, id);
                if (!less(results.top().dist, d) || results.size() < ef) {
                    if (results.size() < ef) {
                        results.push(Edge{d, id});
                    } else if (less(d, results.top().dist)) {
                        results.pop();
                        results.push(Edge{d, id});
                    }
                    candidates.push(Edge{d, id});
                }
            }
        }
        std::vector<Edge> out(results.size());
        for (std::size_t i = out.size(); i-- > 0U;) {
            out[i] = results.top();
            results.pop();
        }
        return out;
    }

    /// select_neighbors_heuristic_owned: closest first, a candidate kept when it is closer to the
    /// node than to every one kept so far; the rest refill what is left of `k`.
    void prune(std::vector<Edge>& ranked, std::size_t k) const {
        if (ranked.size() <= k) {
            return;
        }
        std::sort(ranked.begin(), ranked.end(), [](const Edge& a, const Edge& b) { return less(a.dist, b.dist); });
        std::vector<Edge> kept;
        std::vector<Edge> pruned;
        kept.reserve(k);
        for (const auto& candidate : ranked) {
            if (kept.size() >= k) {
                break;
            }
            const bool prefers = std::all_of(kept.begin(), kept.end(), [&](const Edge& other) {
                return less(candidate.dist, dist(candidate.id, other.id));
            });
            (kept.empty() || prefers ? kept : pruned).push_back(candidate);
        }
        for (std::size_t i = 0; i < pruned.size() && kept.size() < k; ++i) {
            kept.push_back(pruned[i]);
        }
        ranked = std::move(kept);
    }

    void insert(std::uint32_t node, Scratch& s) {
        auto& levels = (*nodes_)[node];
        const std::size_t target = levels.size() - 1U;
        const std::size_t entry_level = (*nodes_)[entry_].size() - 1U;
        Edge ep{dist(node, entry_), entry_};
        for (std::size_t level = entry_level; level > target; --level) {
            ep = greedy(ep, node, level, s);
        }
        std::vector<std::vector<Edge>> selected(target + 1U);
        for (std::size_t level = std::min(target, entry_level) + 1U; level-- > 0U;) {
            (*counts_)[level].fetch_add(1);
            auto found = beam(ep, node, level, s);
            locks_[node].lock();
            auto& ranked = levels[level];
            ranked.insert(ranked.end(), found.begin(), found.end());
            prune(ranked, p_.m);
            selected[level] = ranked;
            locks_[node].unlock();
            ep = found.front();
        }
        // Levels above the entry point's own: nothing to link to (Lance never draws them, as the
        // entry point is the first node of the highest level).
        for (std::size_t level = 0; level < selected.size(); ++level) {
            const std::size_t limit = level == 0U ? p_.m * 2U : p_.m;
            for (const auto& edge : selected[level]) {
                locks_[edge.id].lock();
                auto& ranked = (*nodes_)[edge.id][level];
                ranked.push_back(Edge{edge.dist, node});
                prune(ranked, limit);
                locks_[edge.id].unlock();
            }
        }
    }

    /// connect_stranded_nodes: link every node that level 0 cannot reach from the entry point
    /// from a reachable node near it (each anchor takes at most one such node).
    void connect_stranded() {
        auto& nodes = *nodes_;
        const std::size_t n = nodes.size();
        std::vector<std::uint8_t> reachable(n, 0U);
        std::deque<std::uint32_t> queue;
        const auto mark = [&](std::uint32_t start) {
            std::vector<std::uint32_t> fresh;
            if (reachable[start] != 0U) {
                return fresh;
            }
            reachable[start] = 1U;
            queue.push_back(start);
            while (!queue.empty()) {
                const auto current = queue.front();
                queue.pop_front();
                fresh.push_back(current);
                for (const auto& e : nodes[current][0]) {
                    if (reachable[e.id] == 0U) {
                        reachable[e.id] = 1U;
                        queue.push_back(e.id);
                    }
                }
            }
            return fresh;
        };
        mark(entry_);
        std::vector<std::uint32_t> stranded;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (reachable[i] == 0U) {
                stranded.push_back(i);
            }
        }
        if (stranded.empty()) {
            return;
        }
        std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> waiting_on;
        for (const auto node : stranded) {
            for (const auto& e : nodes[node][0]) {
                if (reachable[e.id] == 0U) {
                    waiting_on[e.id].push_back(node);
                }
            }
        }
        std::vector<std::uint8_t> anchored(n, 0U);
        std::optional<std::uint32_t> chain_tail;
        const auto link = [&](const Edge& anchor, std::uint32_t node) {
            nodes[anchor.id][0].push_back(Edge{anchor.dist, node});
            anchored[anchor.id] = 1U;
            chain_tail = node;
        };
        std::vector<std::uint32_t> isolated;
        std::deque<std::uint32_t> ready(stranded.begin(), stranded.end());
        while (!ready.empty()) {
            const auto node = ready.front();
            ready.pop_front();
            if (reachable[node] != 0U) {
                continue;
            }
            std::vector<Edge> candidates;
            for (const auto& e : nodes[node][0]) {
                if (reachable[e.id] != 0U) {
                    candidates.push_back(e);
                }
            }
            if (candidates.empty()) {
                isolated.push_back(node);
                continue;
            }
            const auto by_dist = [](const Edge& a, const Edge& b) { return less(a.dist, b.dist); };
            const Edge nearest = *std::min_element(candidates.begin(), candidates.end(), by_dist);
            Edge closest = nearest;
            for (std::size_t step = 0; step < p_.ef_construction; ++step) {
                std::optional<Edge> best;
                for (const auto& e : nodes[closest.id][0]) {
                    if (reachable[e.id] == 0U) {
                        continue;
                    }
                    const Edge cand{dist(node, e.id), e.id};
                    if (!best || less(cand.dist, best->dist)) {
                        best = cand;
                    }
                }
                if (best && less(best->dist, closest.dist)) {
                    closest = *best;
                } else {
                    break;
                }
            }
            std::sort(candidates.begin(), candidates.end(), by_dist);
            std::optional<Edge> anchor;
            if (anchored[closest.id] == 0U) {
                anchor = closest;
            } else {
                for (const auto& cand : candidates) {
                    if (anchored[cand.id] == 0U) {
                        anchor = cand;
                        break;
                    }
                }
            }
            if (!anchor && chain_tail) {
                anchor = Edge{dist(node, *chain_tail), *chain_tail};
            }
            link(anchor.value_or(nearest), node);
            for (const auto fresh : mark(node)) {
                const auto it = waiting_on.find(fresh);
                if (it != waiting_on.end()) {
                    ready.insert(ready.end(), it->second.begin(), it->second.end());
                    waiting_on.erase(it);
                }
            }
        }
        for (const auto node : isolated) {
            if (reachable[node] != 0U) {
                continue;
            }
            const std::uint32_t anchor = chain_tail.value_or(entry_);
            link(Edge{dist(anchor, node), anchor}, node);
            mark(node);
        }
    }

    Codes c_;
    Params p_;
    float value_scale_ = 0;
    float scale_ = 0;
    float lower_ = 0;
    std::vector<float> sums_;
    std::vector<std::vector<std::vector<Edge>>>* nodes_ = nullptr;
    std::uint32_t entry_ = 0;
    std::vector<SpinLock> locks_;
    std::vector<std::atomic<std::size_t>>* counts_ = nullptr;
};

}  // namespace

Graph build(const Codes& codes, const Params& params) {
    return Builder(codes, params).run();
}

}  // namespace nano_lance::hnsw_build
