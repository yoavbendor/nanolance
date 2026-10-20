// nlbench_index: the index workloads of tools/lance_rs_bench (scalar, vector and full-text indexes,
// built and then queried), run on nanolance's C++ library. The same workload file, the same protocol,
// the same JSON shape, so tools/bench_indexes.py can put the two side by side.
//
//   nlbench_index <workload.txt> <scalar|vector|fts> <work_dir> <base.lance> <out.json>
//
// The base dataset is unindexed and never changed: each build works on a fresh copy made outside the
// timed region. Builds are timed until their timed total reaches the workload's min_seconds (and at
// least three builds ran). Queries repeat until their timed total reaches min_seconds: a scalar query
// is one sample per call; a vector or full-text search is one sample per pass over its query set,
// divided by the number of queries. The workload format is described in lance_rs_bench/src/index_bench.rs.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nanolance/fts_search.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/scalar_index.hpp"
#include "nanolance/vector_search.hpp"

namespace fs = std::filesystem;
using namespace nano_lance;

namespace {

struct ScalarQuery {
    std::string name;
    std::string filter;
    std::optional<std::vector<std::string>> columns;
};

struct VectorSearch {
    std::string name;
    std::uint64_t k = 10;
    std::optional<std::uint32_t> nprobes;
    std::optional<std::uint32_t> refine_factor;
};

struct FtsSearch {
    std::string name;
    std::optional<std::uint64_t> limit;
    bool and_operator = false;
    std::vector<std::string> texts;
};

struct Workload {
    double min_seconds = 10.0;
    std::vector<std::pair<std::string, std::string>> scalar_builds;
    std::vector<ScalarQuery> scalar_queries;
    std::string vector_column;
    std::size_t partitions = 0;
    std::size_t dim = 0;
    std::size_t vector_builds = 5;  // builds per vector index kind, averaged over
    std::vector<std::vector<float>> vector_queries;
    std::vector<std::pair<std::string, std::string>> vector_kinds;
    std::vector<VectorSearch> vector_searches;
    std::string fts_column;
    std::vector<FtsSearch> fts_searches;
};

std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::string field;
    std::istringstream in(line);
    while (std::getline(in, field, sep)) out.push_back(field);
    return out;
}

std::optional<std::uint64_t> optional_number(const std::string& s) {
    const auto n = std::stoull(s);
    if (n == 0) return std::nullopt;
    return n;
}

Workload parse_workload(const fs::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot read workload " + path.string());
    Workload w;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = split(line, '\t');
        const auto& kind = f.at(0);
        if (kind == "min_seconds") {
            w.min_seconds = std::stod(f.at(1));
        } else if (kind == "scalar_build") {
            w.scalar_builds.emplace_back(f.at(1), f.at(2));
        } else if (kind == "scalar_query") {
            ScalarQuery q{f.at(1), f.at(2), std::nullopt};
            if (f.at(3) != "*") q.columns = split(f.at(3), ',');
            w.scalar_queries.push_back(std::move(q));
        } else if (kind == "vector_config") {
            w.vector_column = f.at(1);
            w.partitions = std::stoull(f.at(2));
            w.dim = std::stoull(f.at(3));
            if (f.size() > 4) w.vector_builds = std::stoull(f[4]);
        } else if (kind == "vector_kind") {
            w.vector_kinds.emplace_back(f.at(1), f.at(2));
        } else if (kind == "vector_query") {
            std::vector<float> q;
            for (const auto& x : split(f.at(1), ',')) q.push_back(std::stof(x));
            w.vector_queries.push_back(std::move(q));
        } else if (kind == "vector_search") {
            VectorSearch s;
            s.name = f.at(1);
            s.k = std::stoull(f.at(2));
            if (const auto n = optional_number(f.at(3))) s.nprobes = static_cast<std::uint32_t>(*n);
            if (const auto r = optional_number(f.at(4))) s.refine_factor = static_cast<std::uint32_t>(*r);
            w.vector_searches.push_back(std::move(s));
        } else if (kind == "fts_config") {
            w.fts_column = f.at(1);
        } else if (kind == "fts_search") {
            FtsSearch s;
            s.name = f.at(1);
            s.limit = optional_number(f.at(2));
            s.and_operator = f.at(3) == "1";
            w.fts_searches.push_back(std::move(s));
        } else if (kind == "fts_text") {
            w.fts_searches.at(w.fts_searches.size() - 1).texts.push_back(f.at(1));
        } else {
            throw std::runtime_error("unknown workload record " + kind);
        }
    }
    return w;
}

// Recursive copy of a dataset directory (called outside the timed regions).
void copy_dataset(const fs::path& from, const fs::path& to) {
    fs::remove_all(to);
    fs::copy(from, to, fs::copy_options::recursive);
}

struct Stat {
    double median_ms = 0.0;
    double min_ms = 0.0;
    std::size_t samples = 0;
};

Stat summarise(std::vector<double> xs) {
    std::sort(xs.begin(), xs.end());
    return Stat{xs[xs.size() / 2], xs.front(), xs.size()};
}

// Rows a scan returns: the filter applied with the scalar indices, as nanolance reads them.
std::uint64_t scan_rows(const fs::path& dataset, const ScalarQuery& q) {
    LanceScanRequest request;
    request.columns = q.columns ? &*q.columns : nullptr;
    request.filter = &q.filter;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    if (!lance_dataset_scan(dataset, request, schema, batches, error)) {
        throw std::runtime_error("scan " + q.filter + ": " + error);
    }
    std::uint64_t rows = 0;
    for (auto& batch : batches) {
        rows += static_cast<std::uint64_t>(batch.length);
        if (batch.release) batch.release(&batch);
    }
    if (schema.release) schema.release(&schema);
    return rows;
}

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string json_stat(const Stat& s) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "{\"median_ms\": %.6f, \"min_ms\": %.6f, \"samples\": %zu}", s.median_ms,
                  s.min_ms, s.samples);
    return buf;
}

std::string join_json(const std::vector<std::string>& items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) out += (i ? ", " : "") + items[i];
    return out + "]";
}

template <class T>
std::string json_array(const std::vector<T>& xs) {
    std::ostringstream out;
    out.precision(9);  // a float32 score or distance round-trips
    out << "[";
    for (std::size_t i = 0; i < xs.size(); ++i) out << (i ? ", " : "") << xs[i];
    out << "]";
    return out.str();
}

std::string json_object(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out = "{";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        out += (i ? ", " : "") + json_string(fields[i].first) + ": " + fields[i].second;
    }
    return out + "}";
}

// Times `once` (the timed call, which returns the seconds it took) until the timed total reaches
// min_seconds and at least `min_samples` ran. Each sample is the time per unit: `once` covers `units`
// of work (a pass over a query set covers one unit per query).
Stat sample_until(double min_seconds, std::size_t min_samples, const std::function<double()>& once,
                  double units = 1.0) {
    std::vector<double> samples;
    double total = 0.0;
    while (samples.size() < min_samples || total < min_seconds) {
        const double secs = once();
        total += secs;
        samples.push_back(secs * 1000.0 / units);
    }
    return summarise(samples);
}

double timed_seconds(const std::function<void()>& call) {
    const auto t0 = std::chrono::steady_clock::now();
    call();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::string run_scalar(const Workload& w, const fs::path& work, const fs::path& base) {
    std::vector<std::pair<std::string, std::string>> builds;
    for (const auto& [column, kind] : w.scalar_builds) {
        ScalarIndexType type{};
        if (!parse_scalar_index_type(kind, type)) throw std::runtime_error("unsupported scalar index " + kind);
        const fs::path keep = work / ("cpp_build_" + column + "_" + kind + ".lance");
        ScalarIndexOptions options;
        const auto stat = sample_until(w.min_seconds, 3, [&] {
            copy_dataset(base, keep);
            std::uint64_t version = 0;
            std::string error;
            return timed_seconds([&] {
                if (!dataset_create_scalar_index(keep, column, type, options, version, error)) {
                    throw std::runtime_error("build " + column + " " + kind + ": " + error);
                }
            });
        });
        builds.emplace_back(column + " " + kind, json_stat(stat));
    }

    // One dataset with every scalar index, for the queries.
    const fs::path serving = work / "cpp_scalar.lance";
    copy_dataset(base, serving);
    for (const auto& [column, kind] : w.scalar_builds) {
        ScalarIndexType type{};
        parse_scalar_index_type(kind, type);
        std::uint64_t version = 0;
        std::string error;
        if (!dataset_create_scalar_index(serving, column, type, ScalarIndexOptions{}, version, error)) {
            throw std::runtime_error("build " + column + " " + kind + ": " + error);
        }
    }

    std::vector<std::pair<std::string, std::string>> queries;
    for (const auto& q : w.scalar_queries) {
        const auto rows = scan_rows(serving, q);
        const auto stat = sample_until(w.min_seconds, 20, [&] {
            return timed_seconds([&] { scan_rows(serving, q); });
        });
        std::string record = json_stat(stat);
        record.insert(1, "\"rows\": " + std::to_string(rows) + ", ");
        queries.emplace_back(q.name, record);
    }
    return "{\"builds\": " + json_object(builds) + ", \"queries\": " + json_object(queries) + "}";
}

// Each vector index is built w.vector_builds times, each on its own copy, because its training is random:
// recall is measured on every build and averaged, and each search's time is pooled over the builds.
std::string run_vector(const Workload& w, const fs::path& work, const fs::path& base) {
    std::vector<std::pair<std::string, std::string>> builds, searches, hits, distances;
    for (const auto& [name, type] : w.vector_kinds) {
        VectorIndexOptions options;
        options.type = type;
        options.metric = VectorMetric::L2;
        options.num_partitions = static_cast<std::uint32_t>(w.partitions);
        if (type == "IVF_PQ") options.num_sub_vectors = static_cast<std::uint32_t>(w.dim / 8);
        if (type == "IVF_RQ") options.num_bits = 5;  // pylance's default: 1 binary bit and 4 extended bits

        std::vector<fs::path> copies;
        std::vector<double> build_ms;
        double total = 0.0;
        while (build_ms.size() < w.vector_builds || total < w.min_seconds) {
            const fs::path copy = work / ("cpp_" + name + "_" + std::to_string(build_ms.size()) + ".lance");
            copy_dataset(base, copy);
            std::uint64_t version = 0;
            std::string error;
            const double secs = timed_seconds([&] {
                if (!dataset_create_vector_index(copy, w.vector_column, options, version, error)) {
                    throw std::runtime_error("build " + name + ": " + error);
                }
            });
            total += secs;
            build_ms.push_back(secs * 1000.0);
            copies.push_back(copy);
        }
        builds.emplace_back(name, json_stat(summarise(build_ms)));

        for (const auto& s : w.vector_searches) {
            const std::string key = name + "|" + s.name;
            const auto query_pass = [&](const fs::path& dataset, std::vector<std::vector<std::uint64_t>>* ids,
                                        std::vector<std::vector<float>>* dists) {
                for (const auto& q : w.vector_queries) {
                    NearestQuery nq;
                    nq.column = w.vector_column;
                    nq.key = q;
                    nq.k = s.k;
                    if (s.nprobes) {
                        nq.minimum_nprobes = *s.nprobes;
                        nq.maximum_nprobes = *s.nprobes;
                    }
                    if (s.refine_factor) nq.refine_factor = *s.refine_factor;
                    NearestResult result;
                    std::string error;
                    if (!dataset_nearest(dataset, nq, result, error)) throw std::runtime_error("search " + key + ": " + error);
                    if (ids) ids->push_back(result.row_ids);
                    if (dists) dists->push_back(result.distances);
                }
            };
            // One pass per build: each build's answers, for recall and for the correctness checks.
            std::vector<std::string> per_build_ids, per_build_dists;
            for (const auto& copy : copies) {
                std::vector<std::vector<std::uint64_t>> got_ids;
                std::vector<std::vector<float>> got_dists;
                query_pass(copy, &got_ids, &got_dists);
                std::vector<std::string> id_rows, dist_rows;
                for (const auto& ids : got_ids) id_rows.push_back(json_array(ids));
                for (const auto& d : got_dists) dist_rows.push_back(json_array(d));
                per_build_ids.push_back(join_json(id_rows));
                per_build_dists.push_back(join_json(dist_rows));
            }
            hits.emplace_back(key, join_json(per_build_ids));
            distances.emplace_back(key, join_json(per_build_dists));

            // Timed passes, round-robin over the builds, until min_seconds of search (and a pass per build).
            const std::size_t passes = std::max<std::size_t>(5, copies.size());
            std::vector<double> samples;
            double search_total = 0.0;
            for (std::size_t turn = 0; samples.size() < passes || search_total < w.min_seconds; ++turn) {
                const auto& copy = copies[turn % copies.size()];
                const double secs = timed_seconds([&] { query_pass(copy, nullptr, nullptr); });
                search_total += secs;
                samples.push_back(secs * 1000.0 / static_cast<double>(w.vector_queries.size()));
            }
            searches.emplace_back(key, json_stat(summarise(samples)));
        }
        for (const auto& copy : copies) fs::remove_all(copy);
    }
    return "{\"builds\": " + json_object(builds) + ", \"searches\": " + json_object(searches) +
           ", \"hits\": " + json_object(hits) + ", \"distances\": " + json_object(distances) + "}";
}

std::string run_fts(const Workload& w, const fs::path& work, const fs::path& base) {
    const fs::path keep = work / "cpp_fts.lance";
    const auto build = sample_until(w.min_seconds, 3, [&] {
        copy_dataset(base, keep);
        std::uint64_t version = 0;
        std::string error;
        return timed_seconds([&] {
            if (!dataset_create_inverted_index(keep, w.fts_column, InvertedIndexOptions{}, version, error)) {
                throw std::runtime_error("build INVERTED: " + error);
            }
        });
    });

    std::vector<std::pair<std::string, std::string>> searches, hits, scores;
    for (const auto& s : w.fts_searches) {
        const auto query_pass = [&](std::vector<std::vector<std::uint64_t>>* ids, std::vector<std::vector<float>>* sc) {
            for (const auto& text : s.texts) {
                FtsSearchRequest request;
                request.query.kind = FtsQuery::Kind::Match;
                request.query.text = text;
                request.query.columns = {w.fts_column};
                request.query.and_operator = s.and_operator;
                request.limit = s.limit;
                FtsSearchResult result;
                std::string error;
                if (!dataset_full_text_search(keep, request, result, error)) {
                    throw std::runtime_error("search " + s.name + ": " + error);
                }
                if (ids) ids->push_back(result.row_ids);
                if (sc) sc->push_back(result.scores);
            }
        };
        std::vector<std::vector<std::uint64_t>> got_ids;
        std::vector<std::vector<float>> got_scores;
        query_pass(&got_ids, &got_scores);
        std::vector<std::string> id_rows, score_rows;
        for (const auto& ids : got_ids) id_rows.push_back(json_array(ids));
        for (const auto& sc : got_scores) score_rows.push_back(json_array(sc));
        hits.emplace_back(s.name, join_json(id_rows));
        scores.emplace_back(s.name, join_json(score_rows));
        const auto stat = sample_until(w.min_seconds, 5, [&] {
            return timed_seconds([&] { query_pass(nullptr, nullptr); });
        }, static_cast<double>(s.texts.size()));
        searches.emplace_back(s.name, json_stat(stat));
    }
    return "{\"builds\": " + json_object({{"INVERTED", json_stat(build)}}) + ", \"searches\": " +
           json_object(searches) + ", \"hits\": " + json_object(hits) + ", \"scores\": " + json_object(scores) + "}";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: nlbench_index <workload.txt> <scalar|vector|fts> <work_dir> <base.lance> <out.json>\n");
        return 2;
    }
    try {
        const auto workload = parse_workload(argv[1]);
        const std::string section = argv[2];
        const fs::path work = argv[3];
        const fs::path base = argv[4];
        const fs::path out = argv[5];
        fs::create_directories(work);

        std::string body;
        if (section == "scalar") body = run_scalar(workload, work, base);
        else if (section == "vector") body = run_vector(workload, work, base);
        else if (section == "fts") body = run_fts(workload, work, base);
        else throw std::runtime_error("unknown section " + section);

        const auto threads = std::max(1U, std::thread::hardware_concurrency());
        std::ofstream file(out);
        file << "{\"engine\": \"nanolance C++\", \"threads\": " << threads << ", \"" << section
             << "\": " << body << "}\n";
        return file ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "nlbench_index: %s\n", e.what());
        return 1;
    }
}
