// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Shared by the layer benchmarks (bench/<layer>/*.cpp): the command line, the Arrow columns a benchmark writes, the
// check of what it reads back, the median of its timed runs, and the table and JSON it prints. Header-only, so each
// benchmark is one .cpp file. A benchmark writes its own .lance files in a scratch directory that is removed at exit,
// checks the values it reads before it times anything, and exits 1 with a message when they are wrong.

#pragma once

#include "nanolance/column_values.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Set by bench/layers.cmake. Without them the JSON header says "unknown".
#ifndef NL_BENCH_SOURCE_DIR
#define NL_BENCH_SOURCE_DIR "."
#endif
#ifndef NL_BENCH_BUILD_TYPE
#define NL_BENCH_BUILD_TYPE ""
#endif

namespace nl_bench {

/// What a column's rows hold.
enum class Kind { Int64, Float64, Bool, Utf8 };

/// One column of a benchmark's data, one value per row. Int64 and Float64 rows are the value's 8 raw bytes, Bool rows
/// one byte (0 or 1), Utf8 rows the text. No nulls.
struct Column {
    std::string name;
    Kind kind = Kind::Int64;
    std::vector<std::string> rows;
};

/// The 8 raw bytes of a value, as a Column row.
inline std::string le_bytes(std::int64_t value) {
    std::string out(sizeof value, '\0');
    std::memcpy(out.data(), &value, sizeof value);
    return out;
}
inline std::string le_bytes(double value) {
    std::string out(sizeof value, '\0');
    std::memcpy(out.data(), &value, sizeof value);
    return out;
}

/// The C Arrow structs for a struct batch of the columns, as the Arrow C data interface lays them out. They point at
/// storage this object owns, so it is neither copied nor moved, and it must outlive every call that reads its schema
/// or array. Utf8 offsets are 32-bit, so a column may hold up to 2 GB of text.
class ArrowBatch {
public:
    explicit ArrowBatch(const std::vector<Column>& columns) {
        for (const auto& column : columns) {
            auto child = std::make_unique<Child>();
            child->name = column.name;
            fill(*child, column);
            child_schemas_.push_back(&child->schema);
            child_arrays_.push_back(&child->array);
            children_.push_back(std::move(child));
        }
        schema_.format = "+s";
        schema_.n_children = static_cast<std::int64_t>(children_.size());
        schema_.children = child_schemas_.data();
        array_.length = columns.empty() ? 0 : static_cast<std::int64_t>(columns.front().rows.size());
        array_.n_buffers = 1;
        array_.n_children = schema_.n_children;
        array_.buffers = root_buffers_;
        array_.children = child_arrays_.data();
    }
    ArrowBatch(const ArrowBatch&) = delete;
    ArrowBatch& operator=(const ArrowBatch&) = delete;

    ArrowSchema& schema() { return schema_; }
    ArrowArray& array() { return array_; }

private:
    struct Child {
        std::string name;
        ArrowSchema schema{};
        ArrowArray array{};
        std::vector<std::uint8_t> data;
        std::vector<std::int32_t> offsets;
        const void* buffers[3] = {nullptr, nullptr, nullptr};
    };

    static void fill(Child& child, const Column& column) {
        const auto n = column.rows.size();
        child.schema.name = child.name.c_str();
        child.array.length = static_cast<std::int64_t>(n);
        child.array.buffers = child.buffers;
        switch (column.kind) {
            case Kind::Int64:
            case Kind::Float64:
                child.schema.format = column.kind == Kind::Int64 ? "l" : "g";
                child.array.n_buffers = 2;
                child.data.resize(8U * n);
                for (std::size_t i = 0; i < n; ++i) {
                    std::memcpy(child.data.data() + 8U * i, column.rows[i].data(), 8);
                }
                child.buffers[1] = child.data.data();
                break;
            case Kind::Bool:
                child.schema.format = "b";
                child.array.n_buffers = 2;
                child.data.assign((n + 7U) / 8U, 0);  // Arrow's booleans are bit-packed, LSB first
                for (std::size_t i = 0; i < n; ++i) {
                    if (!column.rows[i].empty() && column.rows[i][0] != 0) {
                        child.data[i / 8U] = static_cast<std::uint8_t>(child.data[i / 8U] | (1U << (i % 8U)));
                    }
                }
                child.buffers[1] = child.data.data();
                break;
            case Kind::Utf8: {
                child.schema.format = "u";
                child.array.n_buffers = 3;
                child.offsets.resize(n + 1);
                std::size_t total = 0;
                for (std::size_t i = 0; i < n; ++i) {
                    child.offsets[i] = static_cast<std::int32_t>(total);
                    total += column.rows[i].size();
                }
                child.offsets[n] = static_cast<std::int32_t>(total);
                child.data.reserve(total);
                for (const auto& row : column.rows) {
                    child.data.insert(child.data.end(), row.begin(), row.end());
                }
                child.buffers[1] = child.offsets.data();
                child.buffers[2] = child.data.data();
                break;
            }
        }
    }

    std::vector<std::unique_ptr<Child>> children_;
    std::vector<ArrowSchema*> child_schemas_;
    std::vector<ArrowArray*> child_arrays_;
    ArrowSchema schema_{};
    ArrowArray array_{};
    const void* root_buffers_[1] = {nullptr};
};

/// Row numbers a check looks at: the first, second, middle and last row, each once.
inline std::vector<std::uint64_t> sample_rows(std::uint64_t n) {
    std::vector<std::uint64_t> rows;
    for (const std::uint64_t r : {std::uint64_t{0}, std::uint64_t{1}, n / 2, n == 0 ? 0 : n - 1}) {
        if (r < n && std::find(rows.begin(), rows.end(), r) == rows.end()) {
            rows.push_back(r);
        }
    }
    return rows;
}

/// Checks a decoded column against the rows it was written from: the row count, then the values at sample_rows().
/// Returns false with `why` set on a mismatch.
inline bool check_column(const nano_lance::ColumnValues& values, const Column& column, std::string& why) {
    const std::uint64_t n = column.rows.size();
    if (column.kind == Kind::Utf8) {
        if (values.kind != nano_lance::ColumnValues::Kind::VariableWidth) {
            why = column.name + ": decoded as fixed-width, want text";
            return false;
        }
        const std::size_t width = values.variable.large ? 8 : 4;
        const auto& offs = values.variable.offsets;
        if (offs.size() < width || offs.size() / width - 1 != n) {
            why = column.name + ": decoded " + std::to_string(offs.size() < width ? 0 : offs.size() / width - 1) +
                  " rows, want " + std::to_string(n);
            return false;
        }
        auto offset_at = [&](std::uint64_t r) -> std::uint64_t {
            if (width == 8) {
                std::uint64_t v = 0;
                std::memcpy(&v, offs.data() + 8 * r, 8);
                return v;
            }
            std::uint32_t v = 0;
            std::memcpy(&v, offs.data() + 4 * r, 4);
            return v;
        };
        for (const std::uint64_t r : sample_rows(n)) {
            const auto begin = offset_at(r);
            const auto end = offset_at(r + 1);
            const std::string got = end >= begin && end <= values.variable.data.size()
                                        ? std::string(values.variable.data.begin() + static_cast<std::ptrdiff_t>(begin),
                                                      values.variable.data.begin() + static_cast<std::ptrdiff_t>(end))
                                        : std::string("<bad offsets>");
            if (got != column.rows[r]) {
                why = column.name + ": row " + std::to_string(r) + " is '" + got + "', want '" + column.rows[r] + "'";
                return false;
            }
        }
        return true;
    }
    if (values.kind != nano_lance::ColumnValues::Kind::FixedWidth) {
        why = column.name + ": decoded as variable-width, want fixed-width";
        return false;
    }
    const std::size_t width = column.kind == Kind::Bool ? 1 : 8;
    if (values.fixed.size() != n * width) {
        why = column.name + ": decoded " + std::to_string(values.fixed.size() / width) + " values, want " +
              std::to_string(n);
        return false;
    }
    for (const std::uint64_t r : sample_rows(n)) {
        if (std::memcmp(values.fixed.data() + r * width, column.rows[r].data(), width) != 0) {
            why = column.name + ": value at row " + std::to_string(r) + " differs";
            return false;
        }
    }
    return true;
}

/// Bytes a decoded column holds in memory: its fixed-width values, or its offsets and text.
inline std::uint64_t decoded_bytes(const nano_lance::ColumnValues& values) {
    return values.fixed.size() + values.variable.data.size() + values.variable.offsets.size();
}

/// The median, in nanoseconds, of `runs` timed calls of `op` after one untimed warm-up call. `op` returns false with
/// `error` set on failure, which stops the timing.
inline bool median_nanoseconds(int runs, const std::function<bool(std::string&)>& op, std::uint64_t& median,
                               std::string& error) {
    if (!op(error)) {
        return false;
    }
    std::vector<std::uint64_t> samples;
    for (int r = 0; r < std::max(runs, 1); ++r) {
        const auto start = std::chrono::steady_clock::now();
        if (!op(error)) {
            return false;
        }
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()));
    }
    std::sort(samples.begin(), samples.end());
    const auto mid = samples.size() / 2;
    median = samples.size() % 2 == 1 ? samples[mid] : (samples[mid - 1] + samples[mid]) / 2;
    return true;
}

/// Command-line options every benchmark takes.
struct Options {
    std::uint64_t rows = 0;  // --rows: the main scale; 0 means the program's default for the mode
    int runs = 0;            // --runs: timed runs per case; 0 means 2 with --quick, 7 otherwise
    bool quick = false;      // --quick: small sizes, for a test that only checks the program runs
    std::string json_path;   // --json: where to write the results; empty for none
};

/// Reads the command line into `options`. Returns -1 to run the benchmark, or the exit code to return now.
inline int parse_options(int argc, char** argv, const char* program, Options& options) {
    const std::string usage = std::string("usage: ") + program + " [--rows N] [--runs N] [--quick] [--json PATH]";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--quick") {
            options.quick = true;
        } else if ((arg == "--rows" || arg == "--runs") && has_value) {
            const auto value = std::strtoull(argv[++i], nullptr, 10);
            if (value == 0) {
                std::cerr << usage << "\n" << arg << " needs a positive number\n";
                return 2;
            }
            if (arg == "--rows") {
                options.rows = value;
            } else {
                options.runs = static_cast<int>(value);
            }
        } else if (arg == "--json" && has_value) {
            options.json_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << usage << "\n";
            return 0;
        } else {
            std::cerr << usage << "\nunknown or incomplete argument: " << arg << "\n";
            return 2;
        }
    }
    return -1;
}

/// A scratch directory under the temp directory, removed when the object goes (also on an early return).
class ScratchDir {
public:
    explicit ScratchDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() /
                (name + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

/// Prints the failed check and returns the exit code.
inline int fail(const std::string& message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

/// Standard output of one command, without its trailing newline; empty when it fails.
inline std::string shell_output(const std::string& command) {
    std::string out;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return out;
    }
    char buffer[256];
    while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) {
        out += buffer;
    }
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

/// What the JSON header says about the run: the program, the commit it was built from, the compiler and build type,
/// the core count, and the scale and runs it used.
struct RunInfo {
    std::string program;
    std::string git_commit = "unknown";
    bool git_dirty = false;
    std::string compiler;
    std::string build_type;
    unsigned cores = 0;
    bool quick = false;
    std::uint64_t rows = 0;
    int runs = 0;
};

inline RunInfo make_run_info(const char* program, const Options& options, std::uint64_t rows, int runs) {
    RunInfo info;
    info.program = program;
    const std::string source = std::string("git -C '") + NL_BENCH_SOURCE_DIR + "' ";
    const std::string commit = shell_output(source + "rev-parse HEAD 2>/dev/null");
    if (!commit.empty()) {
        info.git_commit = commit;
        info.git_dirty = !shell_output(source + "status --porcelain --untracked-files=no 2>/dev/null").empty();
    }
#if defined(__clang__)
    info.compiler = std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    info.compiler = std::string("gcc ") + __VERSION__;
#else
    info.compiler = "unknown";
#endif
    info.build_type = std::string(NL_BENCH_BUILD_TYPE).empty() ? "unspecified" : NL_BENCH_BUILD_TYPE;
    info.cores = std::thread::hardware_concurrency();
    info.quick = options.quick;
    info.rows = rows;
    info.runs = runs;
    return info;
}

/// One timed case. `bytes` is the decoded size the case produced, or empty for an operation that decodes no values
/// (a footer or metadata read), which has no MB/s. rows/s is the rows of the file the case covers, per second.
struct Result {
    std::string name;
    std::uint64_t rows = 0;
    std::optional<std::uint64_t> bytes;
    std::uint64_t median_ns = 0;

    double rows_per_s() const { return median_ns == 0 ? 0.0 : static_cast<double>(rows) * 1e9 / static_cast<double>(median_ns); }
    /// Megabytes (10^6 bytes) per second.
    std::optional<double> mb_per_s() const {
        if (!bytes || median_ns == 0) {
            return std::nullopt;
        }
        return static_cast<double>(*bytes) * 1e3 / static_cast<double>(median_ns);
    }
};

inline void print_run_info(const RunInfo& info) {
    std::cout << info.program << ": " << (info.quick ? "quick, " : "") << info.rows << " rows, " << info.runs
              << " timed runs after 1 warm-up, " << info.build_type << " build with " << info.compiler << ", "
              << info.cores << " cores, commit " << info.git_commit << (info.git_dirty ? " (uncommitted changes)" : "")
              << "\n";
}

/// `value` with `precision` decimals.
inline std::string fixed_text(double value, int precision) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(precision) << value;
    return text.str();
}

inline void print_results(const std::vector<Result>& results) {
    std::cout << std::left << std::setw(34) << "case" << std::right << std::setw(11) << "rows" << std::setw(13)
              << "bytes" << std::setw(13) << "median ms" << std::setw(15) << "rows/s" << std::setw(12) << "MB/s"
              << "\n";
    for (const auto& r : results) {
        const auto mb = r.mb_per_s();
        std::cout << std::left << std::setw(34) << r.name << std::right << std::setw(11) << r.rows << std::setw(13)
                  << (r.bytes ? std::to_string(*r.bytes) : std::string("-")) << std::setw(13)
                  << fixed_text(static_cast<double>(r.median_ns) / 1e6, 4) << std::setw(15)
                  << fixed_text(r.rows_per_s(), 0) << std::setw(12) << (mb ? fixed_text(*mb, 1) : std::string("-"))
                  << "\n";
    }
}

inline std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out + "\"";
}

/// Writes the header and the results as one JSON object: {"header": {...}, "results": [{name, rows, bytes, median_ns,
/// rows_per_s, mb_per_s}, ...]}. tools/compare_bench.py reads it.
inline bool write_json(const std::string& path, const RunInfo& info, const std::vector<Result>& results,
                       std::string& error) {
    std::ofstream out(path);
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    out << "{\n  \"header\": {\n"
        << "    \"program\": " << json_string(info.program) << ",\n"
        << "    \"git_commit\": " << json_string(info.git_commit) << ",\n"
        << "    \"git_dirty\": " << (info.git_dirty ? "true" : "false") << ",\n"
        << "    \"compiler\": " << json_string(info.compiler) << ",\n"
        << "    \"build_type\": " << json_string(info.build_type) << ",\n"
        << "    \"cores\": " << info.cores << ",\n"
        << "    \"quick\": " << (info.quick ? "true" : "false") << ",\n"
        << "    \"rows\": " << info.rows << ",\n"
        << "    \"runs\": " << info.runs << "\n  },\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        out << "    {\"name\": " << json_string(r.name) << ", \"rows\": " << r.rows << ", \"bytes\": "
            << (r.bytes ? std::to_string(*r.bytes) : std::string("null")) << ", \"median_ns\": " << r.median_ns
            << ", \"rows_per_s\": " << fixed_text(r.rows_per_s(), 1) << ", \"mb_per_s\": "
            << (r.mb_per_s() ? fixed_text(*r.mb_per_s(), 3) : std::string("null")) << "}"
            << (i + 1 < results.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    if (!out) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace nl_bench
