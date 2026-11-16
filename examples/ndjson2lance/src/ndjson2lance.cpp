// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// ndjson2lance: a mixed stream of JSON-lines records in, one clean Lance table per record shape out.
//
//   stdin --Reader--> blocks of whole lines --Router--> one Heap per record shape
//   Heap: learn a schema from its first rows (simdjson_arrow infers it), lock it, write the first
//         dump, then stream the later rows through the locked schema into the table.
//   Lines that cannot be read go to _rejects.lance. Every table is one row of _catalog.lance.
//
// Every non-blank input line ends up in exactly one table, in _rejects.lance, or as unwritable
// (its table could not be written). The summary checks that count.

#include "nanolance/nano_lance_writer.h"
#include "nanolance/writer.hpp"
#include "simdjson_arrow.h"

#include <nanoarrow/nanoarrow.h>
#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kMiB = size_t{1} << 20;
// Reader block size. The reader buffer grows past this only to hold one long line.
constexpr size_t kBlockBytes = 16 * kMiB;
// Table names are file names, and the catalog and rejects files take these.
constexpr size_t kMaxNameBytes = 64;
constexpr std::string_view kRejectsName = "_rejects";
constexpr std::string_view kCatalogName = "_catalog";
// Between the keys of a fingerprint. A byte no JSON key can hold unescaped in practice.
constexpr char kKeySeparator = '\x1f';
// Read-time reads cost a copy each; below this, one thread is faster than starting the others.
constexpr size_t kThreadedReadBytes = kMiB;

struct Options {
    fs::path out_dir;
    std::string input;      // empty: stdin
    std::string route_key;  // empty: route by the set of top-level keys
    size_t learn_rows = 100;
    size_t learn_max_rows = 10000;
    size_t flush_rows = 32768;
    size_t flush_bytes = 8 * kMiB;
    size_t table_bytes = 32 * kMiB;
    size_t max_line_bytes = 16 * kMiB;
    size_t threads = 1;
    bool strict = false;
    bool one_version = false;
    bool compress = false;
};

// Record batches read by simdjson_arrow. Each one is a struct ArrowArray that the caller releases
// (simdjson_arrow.h); this holder does it on scope exit.
struct Batches {
    std::vector<ArrowArray> items;
    Batches() = default;
    Batches(const Batches&) = delete;
    Batches& operator=(const Batches&) = delete;
    ~Batches() {
        for (ArrowArray& b : items) {
            if (b.release != nullptr) b.release(&b);
        }
    }
};

// One struct record batch of the given columns, built with nanoarrow (the way nanolance's own
// builders do). Fill a row column by column, then end_row(). A failed append makes finish() fail.
class BatchBuilder {
  public:
    struct Column {
        const char* name;
        ArrowType type;
    };

    explicit BatchBuilder(std::initializer_list<Column> columns) {
        ArrowSchemaInit(&schema_);
        check(ArrowSchemaSetTypeStruct(&schema_, static_cast<int64_t>(columns.size())));
        int64_t i = 0;
        for (const Column& c : columns) {
            ArrowSchema* child = schema_.children[i++];
            check(ArrowSchemaSetType(child, c.type));
            check(ArrowSchemaSetName(child, c.name));
        }
        check(ArrowArrayInitFromSchema(&array_, &schema_, nullptr));
        check(ArrowArrayStartAppending(&array_));
    }
    ~BatchBuilder() {
        ArrowArrayRelease(&array_);
        ArrowSchemaRelease(&schema_);
    }
    BatchBuilder(const BatchBuilder&) = delete;
    BatchBuilder& operator=(const BatchBuilder&) = delete;

    void u64(int64_t column, uint64_t value) {
        check(ArrowArrayAppendUInt(array_.children[column], value));
    }
    void str(int64_t column, std::string_view value) {
        const ArrowStringView view{value.data(), static_cast<int64_t>(value.size())};
        check(ArrowArrayAppendString(array_.children[column], view));
    }
    void end_row() { check(ArrowArrayFinishElement(&array_)); }
    // The batch is complete; array() and schema() are then ready for nanolance's write_batch.
    bool finish() {
        check(ArrowArrayFinishBuildingDefault(&array_, nullptr));
        return ok_;
    }
    ArrowArray* array() { return &array_; }
    ArrowSchema* schema() { return &schema_; }

  private:
    void check(int code) {
        if (code != NANOARROW_OK) ok_ = false;
    }

    ArrowSchema schema_{};
    ArrowArray array_{};
    bool ok_ = true;
};

// One Lance dataset. Unstaged (the default): each publish() is one visible version, and the first
// one creates the dataset. Staged (--one-version): fragments wait for finish(), which publishes them
// as one version.
class Sink {
  public:
    Sink(fs::path path, const Options& options, bool staged)
        : path_(std::move(path)), options_(options), staged_(staged) {}

    bool open() {
        NanoLanceWriteOptions c;
        nano_lance_write_options_init(&c);
        c.compression = options_.compress;
        c.stage_fragments = staged_;
        // Unstaged, the budget starts at 0 so the create commit holds exactly the first dump; the
        // budget is set once the dataset exists (see publish()).
        c.max_pending_bytes = staged_ ? options_.table_bytes : 0;
        // nano_lance::Writer's handle() is the documented way to reach options it does not wrap.
        const std::string path = path_.string();
        return nano_lance_writer_open(writer_.handle(), path.c_str(), &c) == NANO_LANCE_OK;
    }

    bool write(ArrowArray* batch, ArrowSchema* schema) { return writer_.write_batch(batch, schema); }

    bool publish() {
        if (staged_) return true;
        if (created_) return writer_.commit(true);
        if (!writer_.commit(false)) return false;
        created_ = true;
        return nano_lance_writer_set_max_pending_bytes(writer_.handle(), options_.table_bytes) ==
               NANO_LANCE_OK;
    }

    bool finish() {
        if (!staged_) return true;
        uint64_t version = 0;
        return nano_lance_writer_finish(writer_.handle(), NANO_LANCE_COMMIT_CREATE, &version) ==
               NANO_LANCE_OK;
    }

    std::string error() const { return writer_.error(); }

  private:
    fs::path path_;
    const Options& options_;
    bool staged_;
    bool created_ = false;
    nano_lance::Writer writer_;
};

// Lines that were not written, with the reason. Rows are kept in input order within a table; the
// rejects file is in the order the problems were found, and each row names its line.
class Rejects {
  public:
    explicit Rejects(const Options& options) : sink_(options.out_dir / (std::string(kRejectsName) + ".lance"), options, options.one_version), flush_bytes_(options.flush_bytes) {}

    void add(uint64_t line, std::string_view table, std::string_view reason, std::string_view raw) {
        lines_.push_back(line);
        tables_.emplace_back(table);
        reasons_.emplace_back(reason);
        raws_.emplace_back(raw);
        pending_bytes_ += table.size() + reason.size() + raw.size();
        ++count_;
        if (pending_bytes_ >= flush_bytes_) flush();
    }

    // Writes what is pending and publishes. False if the rejects file could not be written.
    bool finish() {
        flush();
        if (opened_ && !sink_.finish()) ok_ = false;
        return ok_;
    }

    uint64_t count() const { return count_; }
    bool ok() const { return ok_; }
    std::string error() const { return error_; }

  private:
    void flush() {
        if (lines_.empty()) return;
        if (!opened_) {
            opened_ = true;
            if (!sink_.open()) return fail("cannot open " + std::string(kRejectsName) + ".lance: " + sink_.error());
        }
        BatchBuilder batch({{"line", NANOARROW_TYPE_UINT64},
                            {"table", NANOARROW_TYPE_STRING},
                            {"reason", NANOARROW_TYPE_STRING},
                            {"raw", NANOARROW_TYPE_STRING}});
        for (size_t i = 0; i < lines_.size(); ++i) {
            batch.u64(0, lines_[i]);
            batch.str(1, tables_[i]);
            batch.str(2, reasons_[i]);
            batch.str(3, raws_[i]);
            batch.end_row();
        }
        if (!batch.finish() || !sink_.write(batch.array(), batch.schema()) || !sink_.publish()) {
            fail("cannot write " + std::string(kRejectsName) + ".lance: " + sink_.error());
        }
        lines_.clear();
        tables_.clear();
        reasons_.clear();
        raws_.clear();
        pending_bytes_ = 0;
    }

    void fail(std::string message) {
        ok_ = false;
        error_ = std::move(message);
    }

    Sink sink_;
    size_t flush_bytes_;
    bool opened_ = false;
    bool ok_ = true;
    std::string error_;
    uint64_t count_ = 0;
    size_t pending_bytes_ = 0;
    std::vector<uint64_t> lines_;
    std::vector<std::string> tables_;
    std::vector<std::string> reasons_;
    std::vector<std::string> raws_;
};

bool has_null(const simdjson_arrow::field& f) {
    if (f.type == simdjson_arrow::type::null) return true;
    return std::any_of(f.children.begin(), f.children.end(), [](const simdjson_arrow::field& c) {
        return has_null(c);
    });
}

// One table: the lines of one record shape, from its first line to its last flush.
//
// Learning: lines are staged in memory. At each learn_rows step (and at learn_max_rows) the staged
// lines are inferred again, so a field that was null in the first rows gets its type once a later
// row has one. The schema is locked when it has no null-typed leaves, or at learn_max_rows, or at
// the end of input.
// Locked: lines are staged and read through the locked schema in flushes.
class Heap {
  public:
    enum class State { learning, locked, failed };

    Heap(std::string name, std::string route, const Options& options, Rejects* rejects)
        : name_(std::move(name)),
          route_(std::move(route)),
          options_(options),
          rejects_(rejects),
          sink_(options.out_dir / (name_ + ".lance"), options, options.one_version) {}

    ~Heap() {
        if (arrow_schema_.release != nullptr) arrow_schema_.release(&arrow_schema_);
    }
    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    // A parsed line of this table (p, n: its text without the newline). keys are its top-level
    // keys, n_keys of them.
    void accept(const char* p, size_t n, uint64_t number, const std::vector<std::string>& keys,
                size_t n_keys) {
        if (first_line == 0) first_line = number;
        last_line = number;
        if (state_ == State::locked) {
            // A key not in the locked schema is skipped on read, so count it here.
            for (size_t i = 0; i < n_keys; ++i) {
                if (known_.count(keys[i]) != 0) continue;
                ++new_key_lines;
                if (options_.strict) {
                    reject_line(number, std::string_view(p, n), "new key after lock: " + keys[i]);
                    return;
                }
                break;
            }
        }
        if (state_ == State::failed) {
            ++unwritable;
            return;
        }
        stage(p, n, number);
        if (state_ == State::learning) {
            const size_t count = lines_.size();
            if (count >= options_.learn_rows &&
                (count % options_.learn_rows == 0 || count == options_.learn_max_rows)) {
                learn_step(false);
            }
        } else if (lines_.size() >= options_.flush_rows || bytes_.size() >= options_.flush_bytes) {
            flush();
        }
    }

    // End of input: a table still learning is locked with what it has; the rest is flushed and
    // published.
    void finish() {
        if (state_ == State::learning) {
            if (lines_.empty()) {
                mark_failed("no line of this table could be read as a record");
            } else {
                learn_step(true);
            }
        }
        if (state_ == State::locked && !lines_.empty()) flush();
        if (state_ == State::locked && !sink_.finish()) {
            // One-version: nothing was published, so every row counted so far is unwritable.
            unwritable += rows;
            rows = 0;
            mark_failed("cannot publish the table: " + sink_.error());
        }
    }

    const std::string& name() const { return name_; }
    const std::string& route() const { return route_; }
    State state() const { return state_; }
    // "ok", or why the table is not there.
    std::string status() const { return state_ == State::failed ? error_ : "ok"; }
    // The table's schema as compact JSON, or "" when it was never locked.
    std::string schema_json() const {
        if (!has_schema_) return "";
        std::string out;
        append_field_json(&out, schema_);
        return out;
    }

    uint64_t rows = 0;
    uint64_t rejects = 0;
    uint64_t new_key_lines = 0;
    uint64_t unwritable = 0;
    uint64_t first_line = 0;
    uint64_t last_line = 0;

  private:
    struct Line {
        size_t offset;  // in bytes_
        size_t length;  // without the newline that follows it in bytes_
        uint64_t number;
    };
    struct Flush {
        uint64_t written = 0;
        uint64_t rejected = 0;
    };

    void stage(const char* p, size_t n, uint64_t number) {
        lines_.push_back(Line{bytes_.size(), n, number});
        bytes_.append(p, n);
        bytes_.push_back('\n');
    }

    void clear_staged() {
        lines_.clear();
        bytes_.clear();
    }

    void reject_line(uint64_t number, std::string_view raw, const std::string& reason) {
        ++rejects;
        rejects_->add(number, name_, reason, raw);
    }

    std::string_view staged_text(size_t index) const {
        return std::string_view(bytes_).substr(lines_[index].offset, lines_[index].length);
    }

    void mark_failed(std::string message) {
        state_ = State::failed;
        error_ = std::move(message);
    }

    // Infers the schema of the first `count` staged lines. On success, schema and batches hold the
    // result; on failure, reason says why.
    bool infer(size_t count, simdjson_arrow::field* schema, Batches* batches, std::string* reason) {
        const Line& last = lines_[count - 1];
        const simdjson::padded_string text(bytes_.data(), last.offset + last.length + 1);
        *schema = simdjson_arrow::field();
        simdjson_arrow::ndjson_options options = read_options(0, count);
        const simdjson_arrow::status s = simdjson_arrow::read_ndjson(text, schema, &batches->items, options);
        if (!s.ok()) {
            *reason = s.message;
            return false;
        }
        return true;
    }

    // Infers from every staged line. A line that makes the set uninferable (a key that is a number
    // in one line and a string in another, say) is rejected, and the rest is inferred. The shortest
    // failing prefix ends at the culprit, found by bisection. False when no line is left.
    bool learn_set(simdjson_arrow::field* schema, Batches* batches) {
        while (!lines_.empty()) {
            std::string reason;
            if (infer(lines_.size(), schema, batches, &reason)) return true;
            size_t lo = 1;
            size_t hi = lines_.size();  // the whole set fails
            while (lo < hi) {
                const size_t mid = lo + (hi - lo) / 2;
                Batches probe;
                simdjson_arrow::field probe_schema;
                std::string ignored;
                if (infer(mid, &probe_schema, &probe, &ignored)) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            Batches probe;
            simdjson_arrow::field probe_schema;
            infer(hi, &probe_schema, &probe, &reason);
            const Line culprit = lines_[hi - 1];
            reject_line(culprit.number, staged_text(hi - 1), reason);
            remove_staged(hi - 1);
        }
        return false;
    }

    void remove_staged(size_t index) {
        std::string kept;
        std::vector<Line> kept_lines;
        kept.reserve(bytes_.size());
        kept_lines.reserve(lines_.size() - 1);
        for (size_t i = 0; i < lines_.size(); ++i) {
            if (i == index) continue;
            const Line& l = lines_[i];
            kept_lines.push_back(Line{kept.size(), l.length, l.number});
            kept.append(bytes_, l.offset, l.length);
            kept.push_back('\n');
        }
        bytes_ = std::move(kept);
        lines_ = std::move(kept_lines);
    }

    // One learning step over the staged lines. Locks when the schema is complete, or at the end of
    // input, or at learn_max_rows; otherwise keeps learning.
    void learn_step(bool at_end) {
        simdjson_arrow::field schema;
        Batches batches;
        if (!learn_set(&schema, &batches)) {
            if (at_end) mark_failed("no line of this table could be read as a record");
            return;
        }
        if (!at_end && lines_.size() < options_.learn_max_rows && has_null(schema)) return;
        lock(schema, batches);
    }

    // Exports the schema, creates the table, and writes the first dump: the batches learning inferred
    // from every staged line.
    void lock(const simdjson_arrow::field& schema, Batches& batches) {
        const simdjson_arrow::status exported = simdjson_arrow::export_schema(schema, &arrow_schema_);
        if (!exported.ok()) {
            drop_staged_as_unwritable("cannot export the schema: " + exported.message);
            return;
        }
        schema_ = schema;
        has_schema_ = true;
        for (const simdjson_arrow::field& c : schema_.children) known_.insert(c.name);
        if (!sink_.open()) {
            drop_staged_as_unwritable("cannot create the table: " + sink_.error());
            return;
        }
        const uint64_t dump = lines_.size();
        bool ok = true;
        for (ArrowArray& b : batches.items) {
            if (ok && !sink_.write(&b, &arrow_schema_)) ok = false;
        }
        if (!ok || !sink_.publish()) {
            drop_staged_as_unwritable("cannot write the table: " + sink_.error());
            return;
        }
        rows += dump;
        clear_staged();
        state_ = State::locked;
    }

    void drop_staged_as_unwritable(std::string message) {
        unwritable += lines_.size();
        clear_staged();
        mark_failed(std::move(message));
    }

    // Writes the staged lines through the locked schema, then publishes them as one version.
    void flush() {
        Flush f;
        const uint64_t total = lines_.size();
        write_range(0, lines_.size(), &f);
        if (state_ == State::locked && !sink_.publish()) {
            mark_failed("cannot publish the table: " + sink_.error());
        }
        if (state_ == State::locked) {
            rows += f.written;
        } else {
            unwritable += total - f.rejected;
        }
        clear_staged();
    }

    // Writes lines [begin, end). A block that does not read is split in halves until its bad
    // lines stand alone and are rejected; the other lines are written in input order.
    void write_range(size_t begin, size_t end, Flush* f) {
        if (begin == end || state_ != State::locked) return;
        Batches batches;
        const simdjson_arrow::status read = read_range(begin, end, &batches);
        if (read.ok()) {
            for (ArrowArray& b : batches.items) {
                if (!sink_.write(&b, &arrow_schema_)) {
                    mark_failed("cannot write the table: " + sink_.error());
                    return;
                }
            }
            f->written += end - begin;
            return;
        }
        if (end - begin == 1) {
            reject_line(lines_[begin].number, staged_text(begin), read.message);
            ++f->rejected;
            return;
        }
        const size_t mid = begin + (end - begin) / 2;
        write_range(begin, mid, f);
        write_range(mid, end, f);
    }

    // simdjson_arrow parses a record within one window of block_size bytes, and a batch is cut at
    // block_size, so the window must hold the longest line of the read (1 MB is only the default).
    simdjson_arrow::ndjson_options read_options(size_t begin, size_t end) const {
        size_t longest = 0;
        for (size_t i = begin; i < end; ++i) longest = std::max(longest, lines_[i].length + 1);
        simdjson_arrow::ndjson_options options;
        options.block_size = std::max(options.block_size, longest);
        return options;
    }

    simdjson_arrow::status read_range(size_t begin, size_t end, Batches* batches) {
        const Line& first = lines_[begin];
        const Line& last = lines_[end - 1];
        const size_t from = first.offset;
        const size_t to = last.offset + last.length + 1;
        const simdjson::padded_string text(bytes_.data() + from, to - from);
        simdjson_arrow::ndjson_options options = read_options(begin, end);
        options.threads = (to - from) >= kThreadedReadBytes ? options_.threads : 1;
        return simdjson_arrow::read_ndjson(schema_, text, &batches->items, options);
    }

    // The schema as JSON: {"name","type","nullable"} and "children" for struct and list fields.
    static void append_field_json(std::string* out, const simdjson_arrow::field& f) {
        *out += "{\"name\":";
        append_json_string(out, f.name);
        *out += ",\"type\":\"";
        *out += type_name(f.type);
        *out += f.nullable ? "\",\"nullable\":true" : "\",\"nullable\":false";
        if (!f.children.empty()) {
            *out += ",\"children\":[";
            for (size_t i = 0; i < f.children.size(); ++i) {
                if (i != 0) *out += ',';
                append_field_json(out, f.children[i]);
            }
            *out += ']';
        }
        *out += '}';
    }

    static const char* type_name(simdjson_arrow::type t) {
        switch (t) {
            case simdjson_arrow::type::null: return "null";
            case simdjson_arrow::type::boolean: return "bool";
            case simdjson_arrow::type::int64: return "int64";
            case simdjson_arrow::type::uint64: return "uint64";
            case simdjson_arrow::type::float64: return "float64";
            case simdjson_arrow::type::utf8: return "utf8";
            case simdjson_arrow::type::struct_: return "struct";
            case simdjson_arrow::type::list: return "list";
            case simdjson_arrow::type::timestamp: return "timestamp";
        }
        return "unknown";
    }

    static void append_json_string(std::string* out, std::string_view s) {
        *out += '"';
        for (const unsigned char c : s) {
            if (c == '"' || c == '\\') {
                *out += '\\';
                *out += static_cast<char>(c);
            } else if (c < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
                *out += escaped;
            } else {
                *out += static_cast<char>(c);
            }
        }
        *out += '"';
    }

    std::string name_;
    std::string route_;
    const Options& options_;
    Rejects* rejects_;
    Sink sink_;
    State state_ = State::learning;
    std::string error_;
    std::string bytes_;       // staged lines, each followed by '\n'
    std::vector<Line> lines_;  // staged lines, in input order
    simdjson_arrow::field schema_;
    bool has_schema_ = false;
    ArrowSchema arrow_schema_{};
    std::unordered_set<std::string> known_;  // top-level keys of the locked schema
};

// Splits the input into blocks of whole lines (the tail after the last newline is carried into the
// next block). A line that cannot fit in the buffer is an error.
class Reader {
  public:
    struct Block {
        const char* data;        // whole lines; the last one may lack its newline at the end of input
        size_t size;
        const char* buffer_end;  // readable from any byte of the block to here (padding included)
    };

    Reader(std::FILE* in, size_t max_line_bytes)
        : in_(in),
          capacity_(std::max(kBlockBytes, max_line_bytes + 1)),
          buffer_(std::make_unique<char[]>(capacity_ + simdjson::SIMDJSON_PADDING)) {}

    // False at the end of the input, or on an error (see error()).
    bool next(Block* out) {
        if (done_) return false;
        // The previous block is no longer in use: move its unfinished tail to the front.
        std::memmove(buffer_.get(), buffer_.get() + consumed_, tail_);
        const size_t want = capacity_ - tail_;
        const size_t got = std::fread(buffer_.get() + tail_, 1, want, in_);
        if (std::ferror(in_)) return fail("cannot read the input");
        bytes_ += got;
        const size_t filled = tail_ + got;
        const bool at_end = got < want;
        size_t cut = filled;
        if (!at_end) {
            const char* last_newline = nullptr;
            for (const char* p = buffer_.get() + filled; p != buffer_.get();) {
                if (*--p == '\n') {
                    last_newline = p;
                    break;
                }
            }
            if (last_newline == nullptr) {
                too_long_ = true;
                return fail("a line is longer than the buffer");
            }
            cut = static_cast<size_t>(last_newline - buffer_.get()) + 1;
        }
        if (at_end) done_ = true;
        if (at_end && filled == 0) return false;
        consumed_ = cut;
        tail_ = filled - cut;
        *out = Block{buffer_.get(), cut, buffer_.get() + capacity_ + simdjson::SIMDJSON_PADDING};
        return true;
    }

    bool too_long() const { return too_long_; }
    std::string error() const { return error_; }
    uint64_t bytes() const { return bytes_; }

  private:
    bool fail(std::string message) {
        error_ = std::move(message);
        done_ = true;
        return false;
    }

    std::FILE* in_;
    size_t capacity_;
    std::unique_ptr<char[]> buffer_;
    size_t consumed_ = 0;
    size_t tail_ = 0;
    bool done_ = false;
    bool too_long_ = false;
    std::string error_;
    uint64_t bytes_ = 0;
};

// Parses each line and sends it to its heap. A line that is not a JSON object, or does not parse, or
// repeats a top-level key, goes to the rejects.
class Router {
  public:
    Router(const Options& options, Rejects* rejects) : options_(options), rejects_(rejects) {
        used_names_.insert(std::string(kRejectsName));
        used_names_.insert(std::string(kCatalogName));
    }

    // False on a fatal input error (see fatal()); the lines before it are routed.
    bool feed(const Reader::Block& block) {
        const char* p = block.data;
        const char* end = block.data + block.size;
        while (p < end) {
            const char* newline = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
            const char* stop = newline != nullptr ? newline : end;
            line(p, static_cast<size_t>(stop - p), block.buffer_end);
            if (!fatal_.empty()) return false;
            p = newline != nullptr ? newline + 1 : end;
        }
        return true;
    }

    void finish() {
        for (const auto& h : heaps_) h->finish();
    }

    const std::vector<std::unique_ptr<Heap>>& heaps() const { return heaps_; }
    uint64_t line_count() const { return line_count_; }
    uint64_t blank_lines() const { return blank_lines_; }
    uint64_t records() const { return records_; }
    std::string fatal() const { return fatal_; }

  private:
    void line(const char* p, size_t n, const char* buffer_end) {
        const uint64_t number = ++line_count_;
        if (number == 1 && n >= 3 && std::memcmp(p, "\xEF\xBB\xBF", 3) == 0) {
            p += 3;
            n -= 3;
        }
        if (n > 0 && p[n - 1] == '\r') --n;
        if (n > options_.max_line_bytes) {
            fatal_ = "line " + std::to_string(number) + " is longer than --max-line-mb (" +
                     std::to_string(options_.max_line_bytes / kMiB) + " MB)";
            return;
        }
        if (std::all_of(p, p + n, [](char c) { return c == ' ' || c == '\t' || c == '\r'; })) {
            ++blank_lines_;
            return;
        }
        ++records_;
        route(p, n, static_cast<size_t>(buffer_end - p), number);
    }

    void route(const char* p, size_t n, size_t capacity, uint64_t number) {
        simdjson::ondemand::document doc;
        if (auto e = parser_.iterate(simdjson::padded_string_view(p, n, capacity)).get(doc)) {
            return reject_route(number, p, n, simdjson::error_message(e));
        }
        simdjson::ondemand::object object;
        if (auto e = doc.get_object().get(object)) {
            return reject_route(number, p, n,
                                e == simdjson::INCORRECT_TYPE ? "a record must be a JSON object"
                                                              : simdjson::error_message(e));
        }
        n_keys_ = 0;
        bool route_found = false;
        const bool by_route = !options_.route_key.empty();
        for (auto entry : object) {
            simdjson::ondemand::field field;
            if (auto e = std::move(entry).get(field)) {
                return reject_route(number, p, n, simdjson::error_message(e));
            }
            std::string_view key;
            if (auto e = field.unescaped_key().get(key)) {
                return reject_route(number, p, n, simdjson::error_message(e));
            }
            if (n_keys_ == keys_.size()) keys_.emplace_back();
            keys_[n_keys_++].assign(key.data(), key.size());
            if (by_route && key == options_.route_key) {
                route_found = true;
                if (auto e = route_text(field.value(), &route_text_)) {
                    return reject_route(number, p, n, simdjson::error_message(e));
                }
            }
        }
        // at_end() is false while the document has trailing content after the object.
        if (!doc.at_end()) return reject_route(number, p, n, "trailing content after the record");
        std::sort(keys_.begin(), keys_.begin() + static_cast<std::ptrdiff_t>(n_keys_));
        for (size_t i = 1; i < n_keys_; ++i) {
            if (keys_[i] == keys_[i - 1]) {
                return reject_route(number, p, n, "duplicate top-level key: " + keys_[i]);
            }
        }

        Heap* heap = nullptr;
        if (!by_route) {
            fingerprint_.clear();
            for (size_t i = 0; i < n_keys_; ++i) {
                if (i != 0) fingerprint_.push_back(kKeySeparator);
                fingerprint_ += keys_[i];
            }
            auto it = by_keys_.find(fingerprint_);
            if (it == by_keys_.end()) {
                heap = add_heap(key_table_name(), fingerprint_);
                by_keys_.emplace(fingerprint_, heap);
            } else {
                heap = it->second;
            }
        } else if (!route_found) {
            const std::string missing = "_no_" + options_.route_key;
            if (missing_heap_ == nullptr) missing_heap_ = add_heap(missing, missing);
            heap = missing_heap_;
        } else {
            auto it = by_route_.find(route_text_);
            if (it == by_route_.end()) {
                heap = add_heap(route_text_, route_text_);
                by_route_.emplace(route_text_, heap);
            } else {
                heap = it->second;
            }
        }
        heap->accept(p, n, number, keys_, n_keys_);
    }

    // The text a route value stands for: a string's contents, or the JSON text of any other value.
    static simdjson::error_code route_text(simdjson::ondemand::value value, std::string* out) {
        std::string_view text;
        if (!value.get_string().get(text)) {
            out->assign(text.data(), text.size());
            return simdjson::SUCCESS;
        }
        std::string_view raw;
        if (auto e = value.raw_json().get(raw)) return e;
        out->assign(raw.data(), raw.size());
        return simdjson::SUCCESS;
    }

    // t<NN>_<first three keys>, NN counting tables in the order they first appear.
    std::string key_table_name() const {
        char number[16];
        std::snprintf(number, sizeof number, "t%02zu", heaps_.size() + 1);
        std::string name = number;
        for (size_t i = 0; i < n_keys_ && i < 3; ++i) {
            name += '_';
            name += keys_[i];
        }
        return name;
    }

    // A table name that is safe as a file name and not already taken.
    std::string unique_name(std::string_view base) {
        std::string name;
        for (const char c : base) {
            if (name.size() == kMaxNameBytes) break;
            const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '-';
            name += safe ? c : '_';
        }
        if (name.empty()) name = "_empty";
        std::string candidate = name;
        for (int k = 2; used_names_.count(candidate) != 0; ++k) {
            const std::string suffix = "_" + std::to_string(k);
            candidate = name.substr(0, kMaxNameBytes - suffix.size()) + suffix;
        }
        used_names_.insert(candidate);
        return candidate;
    }

    Heap* add_heap(const std::string& base, const std::string& route) {
        heaps_.push_back(std::make_unique<Heap>(unique_name(base), route, options_, rejects_));
        return heaps_.back().get();
    }

    void reject_route(uint64_t number, const char* p, size_t n, std::string_view reason) {
        rejects_->add(number, "", reason, std::string_view(p, n));
    }

    const Options& options_;
    Rejects* rejects_;
    simdjson::ondemand::parser parser_;
    std::vector<std::string> keys_;  // reused: keys_[0..n_keys_) are the current line's keys
    size_t n_keys_ = 0;
    std::string fingerprint_;
    std::string route_text_;
    std::unordered_map<std::string, Heap*> by_keys_;
    std::unordered_map<std::string, Heap*> by_route_;
    Heap* missing_heap_ = nullptr;
    std::vector<std::unique_ptr<Heap>> heaps_;
    std::unordered_set<std::string> used_names_;
    uint64_t line_count_ = 0;
    uint64_t blank_lines_ = 0;
    uint64_t records_ = 0;
    std::string fatal_;
};

bool prepare_out_dir(const fs::path& dir, std::string* error) {
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        if (!fs::is_directory(dir, ec)) {
            *error = "'" + dir.string() + "' exists and is not a directory";
            return false;
        }
        if (!fs::is_empty(dir, ec)) {
            *error = "'" + dir.string() + "' already exists and is not empty; refusing to write into it";
            return false;
        }
        return true;
    }
    fs::create_directories(dir, ec);
    if (ec) {
        *error = "cannot create '" + dir.string() + "': " + ec.message();
        return false;
    }
    return true;
}

// One row per table: its name, route, counts, first and last input line, schema, and status.
// Returns "" on success, else why the catalog was not written.
std::string write_catalog(const Options& options, const std::vector<std::unique_ptr<Heap>>& heaps) {
    BatchBuilder batch({{"table", NANOARROW_TYPE_STRING},
                        {"route", NANOARROW_TYPE_STRING},
                        {"rows", NANOARROW_TYPE_UINT64},
                        {"rejects", NANOARROW_TYPE_UINT64},
                        {"new_key_lines", NANOARROW_TYPE_UINT64},
                        {"first_line", NANOARROW_TYPE_UINT64},
                        {"last_line", NANOARROW_TYPE_UINT64},
                        {"unwritable", NANOARROW_TYPE_UINT64},
                        {"schema", NANOARROW_TYPE_STRING},
                        {"status", NANOARROW_TYPE_STRING}});
    for (const auto& h : heaps) {
        batch.str(0, h->name());
        batch.str(1, h->route());
        batch.u64(2, h->rows);
        batch.u64(3, h->rejects);
        batch.u64(4, h->new_key_lines);
        batch.u64(5, h->first_line);
        batch.u64(6, h->last_line);
        batch.u64(7, h->unwritable);
        batch.str(8, h->schema_json());
        batch.str(9, h->status());
        batch.end_row();
    }
    const std::string name = std::string(kCatalogName) + ".lance";
    if (!batch.finish()) return "cannot build the catalog batch";
    // Staged, so that finish() publishes it: an unstaged create commit refuses to run with no rows,
    // and a catalog of an empty input has none.
    Sink sink(options.out_dir / name, options, true);
    if (!sink.open()) return "cannot open " + name + ": " + sink.error();
    if (!sink.write(batch.array(), batch.schema())) return "cannot write " + name + ": " + sink.error();
    if (!sink.finish()) return "cannot publish " + name + ": " + sink.error();
    return "";
}

int run(const Options& options) {
    std::string error;
    if (!prepare_out_dir(options.out_dir, &error)) {
        std::fprintf(stderr, "ndjson2lance: %s\n", error.c_str());
        return 1;
    }
    std::FILE* in = stdin;
    if (!options.input.empty()) {
        in = std::fopen(options.input.c_str(), "rb");
        if (in == nullptr) {
            std::fprintf(stderr, "ndjson2lance: cannot open '%s'\n", options.input.c_str());
            return 1;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    Rejects rejects(options);
    Router router(options, &rejects);
    Reader reader(in, options.max_line_bytes);
    Reader::Block block{};
    std::string fatal;
    while (reader.next(&block)) {
        if (!router.feed(block)) break;
    }
    fatal = router.fatal();
    if (fatal.empty() && reader.too_long()) {
        fatal = "line " + std::to_string(router.line_count() + 1) + " is longer than --max-line-mb (" +
                std::to_string(options.max_line_bytes / kMiB) + " MB)";
    } else if (fatal.empty() && !reader.error().empty()) {
        fatal = reader.error();
    }
    if (in != stdin) std::fclose(in);

    router.finish();
    const bool rejects_ok = rejects.finish();
    const std::string catalog_error = write_catalog(options, router.heaps());
    const bool catalog_ok = catalog_error.empty();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    uint64_t rows = 0;
    uint64_t unwritable = 0;
    uint64_t new_keys = 0;
    bool tables_ok = true;
    std::fprintf(stderr, "%-28s %12s %10s %10s %12s  %s\n", "table", "rows", "rejects", "new_keys",
                 "unwritable", "status");
    for (const auto& h : router.heaps()) {
        rows += h->rows;
        unwritable += h->unwritable;
        new_keys += h->new_key_lines;
        if (h->status() != "ok") tables_ok = false;
        std::fprintf(stderr, "%-28s %12llu %10llu %10llu %12llu  %s\n", h->name().c_str(),
                     static_cast<unsigned long long>(h->rows),
                     static_cast<unsigned long long>(h->rejects),
                     static_cast<unsigned long long>(h->new_key_lines),
                     static_cast<unsigned long long>(h->unwritable), h->status().c_str());
    }
    const uint64_t non_blank = router.records();
    const bool balanced = non_blank == rows + rejects.count() + unwritable;
    const double mb = static_cast<double>(reader.bytes()) / static_cast<double>(kMiB);
    std::fprintf(stderr,
                 "ndjson2lance: %llu input lines (%llu blank), %zu tables, %llu rows, %llu rejects, "
                 "%llu new-key lines, %llu unwritable\n",
                 static_cast<unsigned long long>(router.line_count()),
                 static_cast<unsigned long long>(router.blank_lines()), router.heaps().size(),
                 static_cast<unsigned long long>(rows), static_cast<unsigned long long>(rejects.count()),
                 static_cast<unsigned long long>(new_keys), static_cast<unsigned long long>(unwritable));
    std::fprintf(stderr, "ndjson2lance: %.2f s, %.1f MB/s, %.0f rows/s\n", seconds,
                 seconds > 0 ? mb / seconds : 0.0,
                 seconds > 0 ? static_cast<double>(rows) / seconds : 0.0);
    if (!fatal.empty()) std::fprintf(stderr, "ndjson2lance: stopped: %s\n", fatal.c_str());
    if (!rejects_ok) std::fprintf(stderr, "ndjson2lance: %s\n", rejects.error().c_str());
    if (!catalog_ok) std::fprintf(stderr, "ndjson2lance: %s\n", catalog_error.c_str());

    if (!balanced) {
        std::fprintf(stderr,
                     "ndjson2lance: ACCOUNTING MISMATCH: %llu records != %llu rows + %llu rejects + %llu unwritable\n",
                     static_cast<unsigned long long>(non_blank), static_cast<unsigned long long>(rows),
                     static_cast<unsigned long long>(rejects.count()),
                     static_cast<unsigned long long>(unwritable));
        return 2;
    }
    return (fatal.empty() && tables_ok && rejects_ok && catalog_ok) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    size_t flush_mb = 8;
    size_t table_mb = 32;
    size_t max_line_mb = 16;

    CLI::App app{"Mixed JSON-lines logs in, one clean Lance table per record shape out."};
    std::string out_dir;
    app.add_option("out_dir", out_dir, "Output directory; must not exist or must be empty")->required();
    app.add_option("-i,--input", options.input, "Input file (default: stdin)");
    app.add_option("--route-key", options.route_key,
                   "Route by this top-level field's value instead of by the set of keys");
    app.add_option("--learn-rows", options.learn_rows, "Rows a schema is learned from")
        ->check(CLI::PositiveNumber);
    app.add_option("--learn-max-rows", options.learn_max_rows,
                   "Most rows a schema is learned from before it is locked as it is")
        ->check(CLI::PositiveNumber);
    app.add_option("--flush-rows", options.flush_rows, "Locked tables: rows per flush")
        ->check(CLI::PositiveNumber);
    app.add_option("--flush-mb", flush_mb, "Locked tables: MB of staged text per flush")
        ->check(CLI::PositiveNumber);
    app.add_option("--table-mb", table_mb, "Per table: max_pending_bytes of its writer, in MB")
        ->check(CLI::PositiveNumber);
    app.add_option("--threads", options.threads, "Threads for reading large flushes")
        ->check(CLI::PositiveNumber);
    app.add_option("--max-line-mb", max_line_mb, "Longest line accepted, in MB")
        ->check(CLI::PositiveNumber);
    app.add_flag("--strict", options.strict, "Reject a line with a key the locked schema lacks");
    app.add_flag("--one-version", options.one_version,
                 "Publish each table as one version at the end, not one version per flush");
    app.add_flag("--compress", options.compress, "zstd-compress string columns");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }
    if (options.learn_max_rows < options.learn_rows) {
        std::fprintf(stderr, "ndjson2lance: --learn-max-rows must be at least --learn-rows\n");
        return 1;
    }
    options.out_dir = out_dir;
    options.flush_bytes = flush_mb * kMiB;
    options.table_bytes = table_mb * kMiB;
    options.max_line_bytes = max_line_mb * kMiB;
    return run(options);
}
