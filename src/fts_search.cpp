// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/fts_search.hpp"

#include "fts_fst.hpp"
#include "fts_json.hpp"
#include "fts_posting.hpp"
#include "index_files.hpp"
#include "nanolance/expr.hpp"
#include "nanolance/fts_tokenizer.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"

#include "lance_minimal.pb.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>

namespace nano_lance {

// ── the query ───────────────────────────────────────────────────────────────────────────────────

namespace {

using fts::json::Value;

bool number_field(const Value& obj, const char* key, double& out) {
    const Value* v = obj.get(key);
    if (v != nullptr && v->kind == Value::Number) {
        out = v->n;
    }
    return v == nullptr || v->kind == Value::Number || v->kind == Value::Null;
}

bool operator_field(const Value& obj, bool& and_operator, std::string& error) {
    const Value* v = obj.get("operator");
    if (v == nullptr || v->kind == Value::Null) {
        return true;
    }
    std::string s = v->kind == Value::String ? v->s : "";
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    if (s != "and" && s != "or") {
        error = "invalid operator: " + (v->kind == Value::String ? v->s : std::string("?"));
        return false;
    }
    and_operator = s == "and";
    return true;
}

bool query_from(const Value& v, FtsQuery& q, std::string& error, int depth);

bool query_list(const Value* v, std::vector<FtsQuery>& out, std::string& error, int depth) {
    if (v == nullptr || v->kind == Value::Null) {
        return true;
    }
    if (v->kind != Value::Array) {
        error = "a boolean query's clauses must be a list";
        return false;
    }
    for (const auto& item : v->items) {
        out.emplace_back();
        if (!query_from(item, out.back(), error, depth + 1)) {
            return false;
        }
    }
    return true;
}

bool match_from(const Value& m, FtsQuery& q, std::string& error) {
    q.kind = FtsQuery::Kind::Match;
    const Value* column = m.get("column");
    const Value* terms = m.get("terms");
    if (terms == nullptr) {
        terms = m.get("query");
    }
    if (terms == nullptr || terms->kind != Value::String) {
        error = "a match query needs its terms";
        return false;
    }
    q.text = terms->s;
    if (column != nullptr && column->kind == Value::String) {
        q.columns = {column->s};
    }
    double boost = 1.0;
    double max_expansions = 50;
    double prefix_length = 0;
    if (!number_field(m, "boost", boost) || !number_field(m, "max_expansions", max_expansions) ||
        !number_field(m, "prefix_length", prefix_length)) {
        error = "malformed match query";
        return false;
    }
    q.boost = static_cast<float>(boost);
    q.max_expansions = static_cast<std::uint32_t>(max_expansions);
    q.prefix_length = static_cast<std::uint32_t>(prefix_length);
    const Value* fuzz = m.get("fuzziness");
    if (fuzz != nullptr && fuzz->kind == Value::Null) {
        q.fuzziness.reset();
    } else if (fuzz != nullptr && fuzz->kind == Value::Number) {
        q.fuzziness = static_cast<std::uint32_t>(fuzz->n);
    }
    return operator_field(m, q.and_operator, error);
}

bool query_from(const Value& v, FtsQuery& q, std::string& error, int depth) {
    q = FtsQuery{};
    if (depth > 32) {
        error = "the full-text query is nested too deeply";
        return false;
    }
    if (v.kind == Value::String) {
        q.kind = FtsQuery::Kind::MultiMatch;
        q.text = v.s;
        return true;
    }
    if (v.kind != Value::Object || v.fields.empty()) {
        error = "a full-text query must be a string or an object";
        return false;
    }
    // {"query": text, "columns": [...]}: pylance's plain search.
    if (const Value* text = v.get("query"); text != nullptr && text->kind == Value::String) {
        q.kind = FtsQuery::Kind::MultiMatch;
        q.text = text->s;
        if (const Value* cols = v.get("columns"); cols != nullptr && cols->kind == Value::Array) {
            for (const auto& c : cols->items) {
                if (c.kind != Value::String) {
                    error = "columns must be strings";
                    return false;
                }
                q.columns.push_back(c.s);
            }
        } else if (cols != nullptr && cols->kind == Value::String) {
            q.columns.push_back(cols->s);
        }
        if (const Value* boosts = v.get("boost"); boosts != nullptr && boosts->kind == Value::Array) {
            for (const auto& b : boosts->items) {
                q.boosts.push_back(b.kind == Value::Number ? static_cast<float>(b.n) : 1.0F);
            }
        }
        return operator_field(v, q.and_operator, error);
    }
    const auto& [kind, body] = v.fields.front();
    if (body.kind != Value::Object) {
        error = "malformed " + kind + " query";
        return false;
    }
    if (kind == "match") {
        return match_from(body, q, error);
    }
    if (kind == "match_phrase" || kind == "phrase") {
        q.kind = FtsQuery::Kind::Phrase;
        const Value* column = body.get("column");
        const Value* terms = body.get("terms");
        if (terms == nullptr || terms->kind != Value::String) {
            error = "a phrase query needs its terms";
            return false;
        }
        q.text = terms->s;
        if (column != nullptr && column->kind == Value::String) {
            q.columns = {column->s};
        }
        double slop = 0;
        number_field(body, "slop", slop);
        q.slop = static_cast<std::uint32_t>(slop);
        return true;
    }
    if (kind == "multi_match") {
        q.kind = FtsQuery::Kind::MultiMatch;
        if (const Value* matches = body.get("match_queries"); matches != nullptr && matches->kind == Value::Array) {
            bool first = true;
            for (const auto& m : matches->items) {
                FtsQuery one;
                if (m.kind != Value::Object || !match_from(m.get("match") != nullptr ? *m.get("match") : m, one, error)) {
                    if (error.empty()) {
                        error = "malformed multi_match query";
                    }
                    return false;
                }
                if (!first && (one.text != q.text || one.and_operator != q.and_operator)) {
                    error = "a multi_match query's columns must share their terms and operator";
                    return false;
                }
                first = false;
                q.text = one.text;
                q.and_operator = one.and_operator;
                q.columns.insert(q.columns.end(), one.columns.begin(), one.columns.end());
                q.boosts.push_back(one.boost);
            }
            return true;
        }
        Value plain = body;
        return query_from(plain, q, error, depth + 1);
    }
    if (kind == "boost") {
        q.kind = FtsQuery::Kind::Boost;
        const Value* pos = body.get("positive");
        const Value* neg = body.get("negative");
        if (pos == nullptr || neg == nullptr) {
            error = "a boost query needs a positive and a negative query";
            return false;
        }
        q.positive.emplace_back();
        q.negative.emplace_back();
        double nb = 0.5;
        number_field(body, "negative_boost", nb);
        q.negative_boost = static_cast<float>(nb);
        return query_from(*pos, q.positive[0], error, depth + 1) && query_from(*neg, q.negative[0], error, depth + 1);
    }
    if (kind == "boolean") {
        q.kind = FtsQuery::Kind::Boolean;
        return query_list(body.get("must"), q.must, error, depth) &&
               query_list(body.get("should"), q.should, error, depth) &&
               query_list(body.get("must_not"), q.must_not, error, depth);
    }
    error = "unknown full-text query type '" + kind + "'";
    return false;
}

}  // namespace

bool parse_fts_query(std::string_view text, FtsQuery& out, std::string& error) {
    Value v;
    if (!fts::json::parse(text, v)) {
        error = "the full-text query is not valid JSON";
        return false;
    }
    return query_from(v, out, error, 0);
}

bool is_inverted_index_url(const std::string& url) {
    const std::string suffix = "InvertedIndexDetails";
    return url.size() >= suffix.size() && url.compare(url.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ── the index ───────────────────────────────────────────────────────────────────────────────────

namespace {

using index_files::FileTable;

struct Posting {
    std::vector<std::uint32_t> docs;
    std::vector<std::uint32_t> freqs;
};

struct Partition {
    std::filesystem::path invert;
    std::vector<std::uint8_t> fst_bytes;
    fts::FstMap fst;
    std::unordered_map<std::string, std::uint32_t> token_map;  // the "arrow" token set
    bool use_fst = true;
    std::vector<std::uint64_t> row_ids;
    std::vector<std::uint32_t> num_tokens;
    std::uint64_t total_tokens = 0;
    bool rows_sorted = true;  // documents in row order (as nanolance writes them)

    mutable std::mutex mutex;
    mutable std::uint32_t norms_key = 0;
    mutable std::shared_ptr<const std::vector<float>> norms;  // BM25's length norm by document, for norms_key

    /// Each document's K1 * (1 - B + B * length / avg).
    std::shared_ptr<const std::vector<float>> length_norms(float avg) const;
    mutable std::unordered_map<std::uint32_t, std::shared_ptr<const Posting>> postings;

    std::optional<std::uint32_t> token_id(const std::string& token) const {
        if (use_fst) {
            const auto v = fst.get(token);
            return v ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*v)) : std::nullopt;
        }
        const auto it = token_map.find(token);
        return it == token_map.end() ? std::nullopt : std::optional<std::uint32_t>(it->second);
    }

    /// The posting lists of `ids` (token ids), loaded on first use.
    bool load(const std::vector<std::uint32_t>& ids, fts::TailCodec codec,
              std::vector<std::shared_ptr<const Posting>>& out, std::string& error) const;
};

struct InvertedIndex {
    fts::Analyzer analyzer;
    fts::TailCodec codec = fts::TailCodec::VarintDelta;
    std::vector<std::shared_ptr<const Partition>> partitions;
};

bool Partition::load(const std::vector<std::uint32_t>& ids, fts::TailCodec codec,
                     std::vector<std::shared_ptr<const Posting>>& out, std::string& error) const {
    out.assign(ids.size(), nullptr);
    std::vector<std::uint64_t> missing;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto it = postings.find(ids[i]);
            if (it != postings.end()) {
                out[i] = it->second;
            } else {
                missing.push_back(ids[i]);
            }
        }
    }
    if (missing.empty()) {
        return true;
    }
    std::sort(missing.begin(), missing.end());
    missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
    FileTable table;
    const std::vector<std::string> columns = {"_posting", "_length"};
    if (!index_files::take_table(invert, &columns, missing, table, error)) {
        return false;
    }
    std::unordered_map<std::uint32_t, std::shared_ptr<const Posting>> loaded;
    std::size_t at = 0;
    for (const auto& batch : table.batches) {
        ArrowArrayView view{};
        ArrowError e{};
        if (ArrowArrayViewInitFromSchema(&view, &table.schema, &e) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &e) != NANOARROW_OK || view.n_children < 2) {
            ArrowArrayViewReset(&view);
            error = invert.filename().string() + ": unexpected columns";
            return false;
        }
        const ArrowArrayView* lists = view.children[0];
        const ArrowArrayView* lengths = view.children[1];
        bool ok = lists->n_children == 1;
        for (std::int64_t r = 0; ok && r < batch.length; ++r, ++at) {
            const std::int64_t row = view.offset + r;
            const std::int64_t begin = ArrowArrayViewListChildOffset(lists, row);
            const std::int64_t end = ArrowArrayViewListChildOffset(lists, row + 1);
            std::vector<fts::PostingBlockView> blocks;
            for (std::int64_t k = begin; k < end; ++k) {
                const ArrowBufferView b = ArrowArrayViewGetBytesUnsafe(lists->children[0], k);
                blocks.push_back({b.data.as_uint8, static_cast<std::size_t>(b.size_bytes)});
            }
            auto posting = std::make_shared<Posting>();
            const auto length = static_cast<std::uint32_t>(ArrowArrayViewGetUIntUnsafe(lengths, row));
            posting->docs.reserve(length);
            posting->freqs.reserve(length);
            ok = at < missing.size() &&
                 fts::decode_posting(blocks, length, codec, posting->docs, posting->freqs, error);
            for (const auto d : posting->docs) {
                if (ok && d >= row_ids.size()) {
                    error = "posting list: document " + std::to_string(d) + " out of range";
                    ok = false;
                }
            }
            if (ok) {
                loaded[static_cast<std::uint32_t>(missing[at])] = std::move(posting);
            }
        }
        ArrowArrayViewReset(&view);
        if (!ok) {
            if (error.empty()) {
                error = invert.filename().string() + ": unexpected columns";
            }
            error = invert.filename().string() + ": " + error;
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (postings.size() > 200000U) {
        postings.clear();
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (out[i] == nullptr) {
            const auto it = loaded.find(ids[i]);
            if (it == loaded.end()) {
                error = invert.filename().string() + ": no posting list for token " + std::to_string(ids[i]);
                return false;
            }
            out[i] = it->second;
            postings[ids[i]] = it->second;
        }
    }
    return true;
}

constexpr float kK1 = 1.2F;
constexpr float kB = 0.75F;

/// Lance's BM25 (lance-index scorer.rs), in its f32 operation order.
float length_norm(std::uint32_t doc_tokens, float avg_doc_length) {
    return kK1 * (1.0F - kB + kB * static_cast<float>(doc_tokens) / avg_doc_length);
}

float doc_weight_with_norm(std::uint32_t freq, float norm) {
    const auto f = static_cast<float>(freq);
    return (kK1 + 1.0F) * f / (f + norm);
}

std::shared_ptr<const std::vector<float>> Partition::length_norms(float avg) const {
    std::uint32_t key;
    std::memcpy(&key, &avg, sizeof(key));
    std::lock_guard<std::mutex> lock(mutex);
    if (norms == nullptr || norms_key != key) {
        auto v = std::make_shared<std::vector<float>>(num_tokens.size());
        for (std::size_t d = 0; d < num_tokens.size(); ++d) {
            (*v)[d] = length_norm(num_tokens[d], avg);
        }
        norms = std::move(v);
        norms_key = key;
    }
    return norms;
}

index_files::IndexCache<InvertedIndex>& index_cache() {
    static index_files::IndexCache<InvertedIndex> cache;
    return cache;
}

std::string metadata_value(const std::filesystem::path& path, const std::string& key, bool& found,
                           std::string& error) {
    std::string value;
    LanceDataFileFooterLayout layout;
    if (!index_files::schema_metadata(path, key, value, found, layout, error)) {
        return {};
    }
    return value;
}

bool load_partition(const std::filesystem::path& dir, std::uint64_t id, Partition& part, std::string& error) {
    const std::string prefix = "part_" + std::to_string(id) + "_";
    part.invert = dir / (prefix + "invert.lance");
    // Tokens: an fst map (token -> id), or token and id columns.
    {
        FileTable table;
        if (!index_files::read_table(dir / (prefix + "tokens.lance"), nullptr, LanceRowRange{}, table, error)) {
            return false;
        }
        int fst_col = -1;
        int token_col = -1;
        int id_col = -1;
        for (std::int64_t c = 0; c < table.schema.n_children; ++c) {
            const std::string name = table.schema.children[c]->name != nullptr ? table.schema.children[c]->name : "";
            if (name == "_token_fst_bytes") {
                fst_col = static_cast<int>(c);
            } else if (name == "_token") {
                token_col = static_cast<int>(c);
            } else if (name == "_token_id") {
                id_col = static_cast<int>(c);
            }
        }
        part.use_fst = fst_col >= 0;
        if (fst_col < 0 && (token_col < 0 || id_col < 0)) {
            error = prefix + "tokens.lance: unknown token set format";
            return false;
        }
        for (const auto& batch : table.batches) {
            ArrowArrayView view{};
            ArrowError e{};
            if (ArrowArrayViewInitFromSchema(&view, &table.schema, &e) != NANOARROW_OK ||
                ArrowArrayViewSetArray(&view, &batch, &e) != NANOARROW_OK) {
                ArrowArrayViewReset(&view);
                error = prefix + "tokens.lance: " + e.message;
                return false;
            }
            for (std::int64_t r = 0; r < batch.length; ++r) {
                const std::int64_t row = view.offset + r;
                if (part.use_fst) {
                    const ArrowBufferView b = ArrowArrayViewGetBytesUnsafe(view.children[fst_col], row);
                    part.fst_bytes.assign(b.data.as_uint8, b.data.as_uint8 + b.size_bytes);
                } else {
                    const ArrowStringView s = ArrowArrayViewGetStringUnsafe(view.children[token_col], row);
                    part.token_map.emplace(std::string(s.data, static_cast<std::size_t>(s.size_bytes)),
                                           static_cast<std::uint32_t>(
                                               ArrowArrayViewGetUIntUnsafe(view.children[id_col], row)));
                }
            }
            ArrowArrayViewReset(&view);
        }
        if (part.use_fst && !part.fst.open(part.fst_bytes.data(), part.fst_bytes.size(), error)) {
            error = prefix + "tokens.lance: " + error;
            return false;
        }
    }
    // Documents: row ids and token counts.
    {
        FileTable table;
        const std::vector<std::string> columns = {"_rowid", "_num_tokens"};
        if (!index_files::read_table(dir / (prefix + "docs.lance"), &columns, LanceRowRange{}, table, error)) {
            return false;
        }
        for (const auto& batch : table.batches) {
            ArrowArrayView view{};
            ArrowError e{};
            if (ArrowArrayViewInitFromSchema(&view, &table.schema, &e) != NANOARROW_OK ||
                ArrowArrayViewSetArray(&view, &batch, &e) != NANOARROW_OK || view.n_children < 2) {
                ArrowArrayViewReset(&view);
                error = prefix + "docs.lance: unexpected columns";
                return false;
            }
            for (std::int64_t r = 0; r < batch.length; ++r) {
                const std::int64_t row = view.offset + r;
                part.row_ids.push_back(ArrowArrayViewGetUIntUnsafe(view.children[0], row));
                const auto n = static_cast<std::uint32_t>(ArrowArrayViewGetUIntUnsafe(view.children[1], row));
                part.num_tokens.push_back(n);
                part.total_tokens += n;
                const auto size = part.row_ids.size();
                part.rows_sorted = part.rows_sorted && (size < 2 || part.row_ids[size - 2] < part.row_ids[size - 1]);
            }
            ArrowArrayViewReset(&view);
        }
    }
    return true;
}

bool load_inverted(const std::filesystem::path& dir, std::shared_ptr<const InvertedIndex>& out, std::string& error) {
    const auto meta = dir / "metadata.lance";
    const std::string key = index_files::file_key(meta);
    if (auto hit = index_cache().find(key)) {
        out = hit;
        return true;
    }
    auto index = std::make_shared<InvertedIndex>();
    bool found = false;
    const std::string format = metadata_value(meta, "format_version", found, error);
    if (!error.empty()) {
        return false;
    }
    if (!found || format != "2") {
        error = "INVERTED index format version " + (found ? format : std::string("1")) +
                " is not supported (2 is; rebuild the index with Lance 12 or nanolance)";
        return false;
    }
    const std::string block = metadata_value(meta, "posting_block_size", found, error);
    if (found && block != "128") {
        error = "INVERTED index posting blocks of " + block + " documents are not supported (128 are)";
        return false;
    }
    const std::string tail = metadata_value(meta, "posting_tail_codec", found, error);
    if (found && tail == "fixed32") {
        index->codec = fts::TailCodec::Fixed32;
    } else if (found && tail != "varint_delta_v1") {
        error = "INVERTED index posting tail codec '" + tail + "' is not supported";
        return false;
    }
    const std::string params_text = metadata_value(meta, "params", found, error);
    fts::AnalyzerParams params;
    if (found && !fts::parse_params(params_text, params, error)) {
        return false;
    }
    if (!index->analyzer.init(params, error)) {
        return false;
    }
    const std::string parts_text = metadata_value(meta, "partitions", found, error);
    fts::json::Value parts;
    if (!found || !fts::json::parse(parts_text, parts) || parts.kind != fts::json::Value::Array) {
        error = "INVERTED index metadata lists no partitions";
        return false;
    }
    for (const auto& p : parts.items) {
        if (p.kind != fts::json::Value::Number || p.n < 0) {
            error = "INVERTED index metadata: malformed partition list";
            return false;
        }
        auto part = std::make_shared<Partition>();
        if (!load_partition(dir, static_cast<std::uint64_t>(p.n), *part, error)) {
            return false;
        }
        index->partitions.push_back(std::move(part));
    }
    index_cache().put(key, index);
    out = index;
    return true;
}

// ── scoring ─────────────────────────────────────────────────────────────────────────────────────

float idf(std::uint64_t token_docs, std::uint64_t num_docs) {
    const auto n = static_cast<float>(num_docs);
    const auto t = static_cast<float>(token_docs);
    return std::log((n - t + 0.5F) / (t + 0.5F) + 1.0F);
}

float doc_weight(std::uint32_t freq, std::uint32_t doc_tokens, float avg_doc_length) {
    return doc_weight_with_norm(freq, length_norm(doc_tokens, avg_doc_length));
}

/// Hits, ascending by row id.
using Hits = std::vector<std::pair<std::uint64_t, float>>;

void sort_hits(Hits& h) {
    const auto by_row = [](const auto& a, const auto& b) { return a.first < b.first; };
    if (!std::is_sorted(h.begin(), h.end(), by_row)) {
        std::sort(h.begin(), h.end(), by_row);
    }
}

/// The better of two hits: the higher score, then the lower row id.
bool better_hit(const std::pair<std::uint64_t, float>& a, const std::pair<std::uint64_t, float>& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
}

/// Hits keeping only the best `k` (all when 0): a heap whose front is the worst kept.
class HitSink {
public:
    HitSink(Hits& out, std::size_t k) : out_(out), k_(k) {}
    void add(std::uint64_t row, float score) {
        if (k_ == 0U) {
            out_.emplace_back(row, score);
            return;
        }
        if (out_.size() < k_) {
            out_.emplace_back(row, score);
            std::push_heap(out_.begin(), out_.end(), better_hit);
            return;
        }
        const auto& worst = out_.front();
        if (score < worst.second || (score == worst.second && row > worst.first)) {
            return;
        }
        std::pop_heap(out_.begin(), out_.end(), better_hit);
        out_.back() = {row, score};
        std::push_heap(out_.begin(), out_.end(), better_hit);
    }

private:
    Hits& out_;
    std::size_t k_;
};

/// Which rows may be returned: the live rows of the dataset (and, with a prefilter, those passing
/// it). A fragment without a list has every row allowed; one absent from `fragments`, none.
struct RowMask {
    std::map<std::uint32_t, std::vector<std::uint8_t>> rows;
    std::set<std::uint32_t> fragments;

    bool allows(std::uint64_t addr) const {
        const auto frag = static_cast<std::uint32_t>(addr >> 32U);
        if (!cached_ || frag != cached_frag_) {
            cached_ = true;
            cached_frag_ = frag;
            cached_live_ = fragments.count(frag) != 0U;
            const auto it = rows.find(frag);
            cached_rows_ = it == rows.end() ? nullptr : &it->second;
        }
        if (!cached_live_) {
            return false;
        }
        if (cached_rows_ == nullptr) {
            return true;
        }
        const auto offset = static_cast<std::size_t>(addr & 0xFFFFFFFFULL);
        return offset < cached_rows_->size() && (*cached_rows_)[offset] != 0U;
    }

private:
    // The last fragment looked up: hits come grouped by fragment.
    mutable bool cached_ = false;
    mutable std::uint32_t cached_frag_ = 0;
    mutable bool cached_live_ = false;
    mutable const std::vector<std::uint8_t>* cached_rows_ = nullptr;
};

int child_index(const ArrowSchema& schema, const std::string& name) {
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children[i]->name != nullptr && name == schema.children[i]->name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/// One column's documents outside the index: their row addresses and tokens.
struct FlatDocs {
    std::vector<std::uint64_t> addrs;
    std::vector<std::vector<std::string>> tokens;  // only documents with tokens
    std::uint64_t total_tokens = 0;
};

struct ColumnIndex {
    std::string name;
    std::shared_ptr<const InvertedIndex> index;
    std::set<std::uint32_t> covered;
    std::uint64_t num_docs = 0;
    std::uint64_t total_tokens = 0;
    bool flat_loaded = false;
    FlatDocs flat;
};

struct Search {
    std::filesystem::path path;
    const FtsSearchRequest* request = nullptr;
    pb::Manifest manifest;
    std::map<std::string, ColumnIndex> columns;  // every column with an INVERTED index
    RowMask mask;
    std::vector<std::string>* plan = nullptr;
    // Scoring buffers, by document, kept zeroed between uses.
    std::vector<float> scores_;
    std::vector<std::uint32_t> seen_;
    std::vector<std::uint32_t> touched_;
    /// When only the best `top_k_` rows of a match can matter (a Match or MultiMatch at the root,
    /// with a limit), a match keeps just those.
    std::size_t top_k_ = 0;

    bool scan_flat(ColumnIndex& c, std::string& error);
    bool match(const FtsQuery& q, const std::string& column, float boost, Hits& out, std::string& error);
    bool run(const FtsQuery& q, Hits& out, std::string& error);
};

bool Search::scan_flat(ColumnIndex& c, std::string& error) {
    if (c.flat_loaded) {
        return true;
    }
    c.flat_loaded = true;
    std::vector<std::uint64_t> frags;
    if (!request->fast_search) {
        for (const auto& f : manifest.fragments) {
            if (c.covered.count(static_cast<std::uint32_t>(f.id)) == 0U) {
                frags.push_back(f.id);
            }
        }
    }
    if (frags.empty()) {
        return true;
    }
    const std::vector<std::string> cols = {c.name};
    LanceScanRequest scan;
    scan.columns = &cols;
    scan.has_version = request->has_version;
    scan.version = request->version;
    scan.fragment_ids = &frags;
    scan.with_row_address = true;
    const std::string* filter = request->filter && request->prefilter ? &*request->filter : nullptr;
    scan.filter = filter;
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    if (!lance_dataset_scan(path, scan, schema, batches, error)) {
        return false;
    }
    const int tc = child_index(schema, c.name);
    const int ac = child_index(schema, "_rowaddr");
    bool ok = tc >= 0 && ac >= 0;
    if (!ok) {
        error = "cannot read column " + c.name;
    }
    std::vector<fts::Token> tokens;
    for (auto& batch : batches) {
        if (ok) {
            ArrowArrayView view{};
            ArrowError e{};
            if (ArrowArrayViewInitFromSchema(&view, &schema, &e) != NANOARROW_OK ||
                ArrowArrayViewSetArray(&view, &batch, &e) != NANOARROW_OK) {
                error = std::string("cannot read a batch: ") + e.message;
                ok = false;
            }
            const ArrowArrayView* text = ok ? view.children[tc] : nullptr;
            if (ok && text->storage_type != NANOARROW_TYPE_STRING && text->storage_type != NANOARROW_TYPE_LARGE_STRING &&
                text->storage_type != NANOARROW_TYPE_STRING_VIEW) {
                error = "full-text search needs a string column; " + c.name + " is not one";
                ok = false;
            }
            for (std::int64_t r = 0; ok && r < batch.length; ++r) {
                const std::int64_t row = view.offset + r;
                if (ArrowArrayViewIsNull(text, row)) {
                    continue;
                }
                const ArrowStringView s = ArrowArrayViewGetStringUnsafe(text, row);
                tokens.clear();
                c.index->analyzer.tokenize(std::string_view(s.data, static_cast<std::size_t>(s.size_bytes)), tokens);
                if (tokens.empty()) {
                    continue;
                }
                c.flat.addrs.push_back(ArrowArrayViewGetUIntUnsafe(view.children[ac], row));
                std::vector<std::string> words;
                words.reserve(tokens.size());
                for (auto& t : tokens) {
                    words.push_back(std::move(t.text));
                }
                c.flat.total_tokens += words.size();
                c.flat.tokens.push_back(std::move(words));
            }
            ArrowArrayViewReset(&view);
        }
        batch.release(&batch);
    }
    schema.release(&schema);
    if (ok && plan != nullptr) {
        plan->push_back("FlatMatchQuery: column=" + c.name + " (" + std::to_string(frags.size()) +
                        " unindexed fragments, " + std::to_string(c.flat.addrs.size()) + " documents)");
    }
    return ok;
}

bool Search::match(const FtsQuery& q, const std::string& column, float boost, Hits& out, std::string& error) {
    const auto it = columns.find(column);
    if (it == columns.end()) {
        error = "Cannot perform full text search on column " + column + ": it has no INVERTED index";
        return false;
    }
    ColumnIndex& c = it->second;
    if (q.fuzziness.value_or(1U) != 0U) {
        error = "fuzzy full-text matching (fuzziness != 0) is not supported by nanolance";
        return false;
    }
    std::vector<fts::Token> query_tokens;
    c.index->analyzer.tokenize(q.text, query_tokens);
    if (query_tokens.empty()) {
        return true;
    }
    // Distinct tokens, and each query token's place among them (duplicates score again).
    std::vector<std::string> distinct;
    std::vector<std::size_t> order;
    for (const auto& t : query_tokens) {
        const auto at = std::find(distinct.begin(), distinct.end(), t.text);
        order.push_back(static_cast<std::size_t>(at - distinct.begin()));
        if (at == distinct.end()) {
            distinct.push_back(t.text);
        }
    }
    const std::size_t nt = distinct.size();
    // Posting lists by partition, and each token's document count.
    const auto& parts = c.index->partitions;
    std::vector<std::vector<std::shared_ptr<const Posting>>> postings(parts.size());
    std::vector<std::uint64_t> token_docs(nt, 0);
    for (std::size_t p = 0; p < parts.size(); ++p) {
        std::vector<std::uint32_t> ids;
        std::vector<std::size_t> which;
        for (std::size_t t = 0; t < nt; ++t) {
            if (const auto id = parts[p]->token_id(distinct[t])) {
                ids.push_back(*id);
                which.push_back(t);
            }
        }
        std::vector<std::shared_ptr<const Posting>> loaded;
        if (!parts[p]->load(ids, c.index->codec, loaded, error)) {
            return false;
        }
        postings[p].assign(nt, nullptr);
        for (std::size_t k = 0; k < ids.size(); ++k) {
            postings[p][which[k]] = loaded[k];
            token_docs[which[k]] += loaded[k]->docs.size();
        }
    }
    const float avg = static_cast<float>(c.total_tokens) / static_cast<float>(c.num_docs);
    std::vector<float> weights(nt, 0.0F);
    for (std::size_t t = 0; t < nt; ++t) {
        weights[t] = token_docs[t] == 0U ? 0.0F : idf(token_docs[t], c.num_docs);
    }
    Hits hits;
    // Only the best rows when the caller needs only those (a positive boost keeps the order).
    HitSink sink(hits, boost > 0.0F ? top_k_ : 0U);
    // Indexed documents: their scores from the index's statistics, accumulated in a dense array per
    // partition and read back in document (row) order.
    for (std::size_t p = 0; p < parts.size(); ++p) {
        const Partition& part = *parts[p];
        if (q.and_operator) {
            bool all = true;
            for (std::size_t t = 0; t < nt; ++t) {
                all = all && postings[p][t] != nullptr;
            }
            if (!all) {
                continue;
            }
        }
        const std::size_t n = part.row_ids.size();
        if (scores_.size() < n) {
            scores_.resize(n, 0.0F);
            seen_.resize(n, 0U);
        }
        touched_.clear();
        const auto norms = part.length_norms(avg);
        const float* norm = norms->data();
        for (const std::size_t t : order) {
            const Posting* posting = postings[p][t].get();
            if (posting == nullptr) {
                continue;
            }
            const float w = weights[t];
            const std::uint32_t* docs = posting->docs.data();
            const std::uint32_t* freqs = posting->freqs.data();
            for (std::size_t k = 0, m = posting->docs.size(); k < m; ++k) {
                const std::uint32_t d = docs[k];
                if (seen_[d] == 0U) {
                    touched_.push_back(d);
                }
                seen_[d] |= 1U;
                scores_[d] += w * doc_weight_with_norm(freqs[k], norm[d]);
            }
        }
        std::uint32_t need = 1U;
        if (q.and_operator) {
            // Count each distinct token's documents (above the "touched" bit).
            for (std::size_t t = 0; t < nt; ++t) {
                for (const std::uint32_t d : postings[p][t]->docs) {
                    seen_[d] += 2U;
                }
            }
            need = 1U + 2U * static_cast<std::uint32_t>(nt);
        }
        const auto emit = [&](std::uint32_t d) {
            if (seen_[d] == need) {
                const std::uint64_t row = part.row_ids[d];
                if (mask.allows(row)) {
                    sink.add(row, scores_[d]);
                }
            }
            scores_[d] = 0.0F;
            seen_[d] = 0U;
        };
        if (part.rows_sorted && touched_.size() * 8U > n) {
            for (std::uint32_t d = 0; d < n; ++d) {
                if (seen_[d] != 0U) {
                    emit(d);
                }
            }
        } else {
            if (part.rows_sorted) {
                std::sort(touched_.begin(), touched_.end());
            }
            for (const std::uint32_t d : touched_) {
                emit(d);
            }
        }
    }
    // Documents outside the index: their scores from the index's statistics and theirs.
    if (!scan_flat(c, error)) {
        return false;
    }
    if (!c.flat.addrs.empty()) {
        const std::uint64_t n = c.num_docs + c.flat.addrs.size();
        const float flat_avg = static_cast<float>(c.total_tokens + c.flat.total_tokens) / static_cast<float>(n);
        std::vector<std::uint64_t> docs = token_docs;
        std::vector<std::vector<std::uint32_t>> counts(c.flat.tokens.size(), std::vector<std::uint32_t>(nt, 0U));
        for (std::size_t d = 0; d < c.flat.tokens.size(); ++d) {
            for (const auto& w : c.flat.tokens[d]) {
                for (std::size_t t = 0; t < nt; ++t) {
                    if (w == distinct[t]) {
                        ++counts[d][t];
                    }
                }
            }
            for (std::size_t t = 0; t < nt; ++t) {
                docs[t] += counts[d][t] != 0U ? 1U : 0U;
            }
        }
        std::vector<float> flat_weights(nt, 0.0F);
        for (std::size_t t = 0; t < nt; ++t) {
            flat_weights[t] = docs[t] == 0U ? 0.0F : idf(docs[t], n);
        }
        for (std::size_t d = 0; d < c.flat.tokens.size(); ++d) {
            std::size_t found = 0;
            for (std::size_t t = 0; t < nt; ++t) {
                found += counts[d][t] != 0U ? 1U : 0U;
            }
            if (found == 0U || (q.and_operator && found != nt)) {
                continue;
            }
            float s = 0.0F;
            const auto dl = static_cast<std::uint32_t>(c.flat.tokens[d].size());
            for (const std::size_t t : order) {
                if (counts[d][t] != 0U) {
                    s += flat_weights[t] * doc_weight(counts[d][t], dl, flat_avg);
                }
            }
            sink.add(c.flat.addrs[d], s);
        }
    }
    if (boost != 1.0F) {
        for (auto& h : hits) {
            h.second *= boost;
        }
    }
    sort_hits(hits);
    out = std::move(hits);
    return true;
}

bool Search::run(const FtsQuery& q, Hits& out, std::string& error) {
    out.clear();
    switch (q.kind) {
        case FtsQuery::Kind::Match: {
            if (q.columns.size() != 1U) {
                error = "a match query needs one column";
                return false;
            }
            return match(q, q.columns[0], q.boost, out, error);
        }
        case FtsQuery::Kind::MultiMatch: {
            std::vector<std::string> cols = q.columns;
            if (cols.empty()) {
                for (const auto& [name, c] : columns) {
                    cols.push_back(name);
                }
            }
            if (cols.empty()) {
                error = "Cannot perform full text search unless an INVERTED index has been created on at least one "
                        "column";
                return false;
            }
            if (!q.boosts.empty() && q.boosts.size() != cols.size()) {
                error = "a multi_match query needs one boost per column";
                return false;
            }
            for (std::size_t i = 0; i < cols.size(); ++i) {
                Hits one;
                if (!match(q, cols[i], q.boosts.empty() ? 1.0F : q.boosts[i], one, error)) {
                    return false;
                }
                Hits merged;
                std::size_t a = 0;
                std::size_t b = 0;
                while (a < out.size() || b < one.size()) {
                    if (b == one.size() || (a < out.size() && out[a].first < one[b].first)) {
                        merged.push_back(out[a++]);
                    } else if (a == out.size() || one[b].first < out[a].first) {
                        merged.push_back(one[b++]);
                    } else {
                        merged.emplace_back(out[a].first, std::max(out[a].second, one[b].second));
                        ++a;
                        ++b;
                    }
                }
                out = std::move(merged);
            }
            return true;
        }
        case FtsQuery::Kind::Phrase: {
            if (q.columns.size() != 1U) {
                error = "a phrase query needs one column";
                return false;
            }
            const auto it = columns.find(q.columns[0]);
            if (it != columns.end() && !it->second.index->analyzer.params().with_position) {
                error = "position is not found but required for phrase queries, try recreating the index with "
                        "position";
            } else {
                error = "phrase queries are not supported by nanolance";
            }
            return false;
        }
        case FtsQuery::Kind::Boost: {
            Hits neg;
            if (q.positive.size() != 1U || q.negative.size() != 1U || !run(q.positive[0], out, error) ||
                !run(q.negative[0], neg, error)) {
                if (error.empty()) {
                    error = "a boost query needs a positive and a negative query";
                }
                return false;
            }
            std::size_t b = 0;
            for (auto& h : out) {
                while (b < neg.size() && neg[b].first < h.first) {
                    ++b;
                }
                if (b < neg.size() && neg[b].first == h.first) {
                    h.second -= q.negative_boost * neg[b].second;
                }
            }
            return true;
        }
        case FtsQuery::Kind::Boolean: {
            if (q.must.empty() && q.should.empty()) {
                error = "boolean query must have at least one should/must query";
                return false;
            }
            bool first = true;
            for (const auto& m : q.must) {
                Hits one;
                if (!run(m, one, error)) {
                    return false;
                }
                if (first) {
                    out = std::move(one);
                    first = false;
                    continue;
                }
                Hits kept;
                std::size_t b = 0;
                for (const auto& h : out) {
                    while (b < one.size() && one[b].first < h.first) {
                        ++b;
                    }
                    if (b < one.size() && one[b].first == h.first) {
                        kept.emplace_back(h.first, h.second + one[b].second);
                    }
                }
                out = std::move(kept);
            }
            const bool union_should = q.must.empty();
            for (const auto& s : q.should) {
                Hits one;
                if (!run(s, one, error)) {
                    return false;
                }
                Hits merged;
                std::size_t a = 0;
                std::size_t b = 0;
                while (a < out.size() || b < one.size()) {
                    if (b == one.size() || (a < out.size() && out[a].first < one[b].first)) {
                        merged.push_back(out[a++]);
                    } else if (a == out.size() || one[b].first < out[a].first) {
                        if (union_should) {
                            merged.push_back(one[b]);
                        }
                        ++b;
                    } else {
                        merged.emplace_back(out[a].first, out[a].second + one[b].second);
                        ++a;
                        ++b;
                    }
                }
                out = std::move(merged);
            }
            for (const auto& n : q.must_not) {
                Hits one;
                if (!run(n, one, error)) {
                    return false;
                }
                std::set<std::uint64_t> excluded;
                for (const auto& h : one) {
                    excluded.insert(h.first);
                }
                out.erase(std::remove_if(out.begin(), out.end(), [&](const auto& h) { return excluded.count(h.first) != 0U; }),
                          out.end());
            }
            return true;
        }
    }
    return false;
}

/// The addresses of the rows of `fragments` passing `filter` (live rows when null).
bool row_addresses(const std::filesystem::path& path, const FtsSearchRequest& q,
                   const std::vector<std::uint64_t>& fragments, const std::string* filter,
                   std::vector<std::uint64_t>& out, std::string& error) {
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
            ArrowArrayView view{};
            ArrowError e{};
            ok = ArrowArrayViewInitFromSchema(&view, &schema, &e) == NANOARROW_OK &&
                 ArrowArrayViewSetArray(&view, &batch, &e) == NANOARROW_OK;
            for (std::int64_t r = 0; ok && r < batch.length; ++r) {
                out.push_back(ArrowArrayViewGetUIntUnsafe(view.children[c], view.offset + r));
            }
            ArrowArrayViewReset(&view);
            if (!ok) {
                error = std::string("cannot read a batch: ") + e.message;
            }
        }
        batch.release(&batch);
    }
    schema.release(&schema);
    return ok;
}

/// Keep the hits passing `filter` (Lance's post-filter), in order.
bool post_filter(const std::filesystem::path& path, const FtsSearchRequest& q, Hits& hits, std::string& error) {
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
        addrs.push_back(h.first);
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
            ArrowArrayView view{};
            ArrowError e{};
            ok = ok && ArrowArrayViewInitFromSchema(&view, &schema, &e) == NANOARROW_OK &&
                 ArrowArrayViewSetArray(&view, &batch, &e) == NANOARROW_OK;
            for (std::int64_t r = 0; ok && r < batch.length; ++r) {
                if (keep[static_cast<std::size_t>(r)] != 0U) {
                    pass.insert(ArrowArrayViewGetUIntUnsafe(view.children[ic], view.offset + r));
                }
            }
            ArrowArrayViewReset(&view);
        }
        batch.release(&batch);
    }
    schema.release(&schema);
    if (!ok) {
        return false;
    }
    hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const auto& h) { return pass.count(h.first) == 0U; }),
               hits.end());
    return true;
}

std::string field_path(const pb::Manifest& manifest, std::int32_t id) {
    std::string path;
    while (id >= 0) {
        const pb::Field* found = nullptr;
        for (const auto& f : manifest.fields) {
            if (f.id == id) {
                found = &f;
                break;
            }
        }
        if (found == nullptr) {
            return {};
        }
        path = path.empty() ? found->name : found->name + "." + path;
        id = found->parent_id;
    }
    return path;
}

}  // namespace

bool dataset_full_text_search(const std::filesystem::path& dataset_path, const FtsSearchRequest& request,
                              FtsSearchResult& out, std::string& error) {
    out = FtsSearchResult{};
    error.clear();
    Search s;
    s.path = dataset_path;
    s.request = &request;
    s.plan = &out.plan;
    std::uint64_t version = request.version;
    if (request.has_version ? !load_manifest_version(dataset_path, request.version, s.manifest, error)
                            : !load_latest_manifest(dataset_path, s.manifest, version, error)) {
        return false;
    }
    // The INVERTED indexes, by column: the segments of the first index on each.
    std::map<std::string, std::string> index_names;
    std::map<std::string, std::vector<const pb::IndexMetadata*>> segments;
    for (const auto& index : s.manifest.indices) {
        if (index.fields.size() != 1U || !is_inverted_index_url(index.details_type_url)) {
            continue;
        }
        const std::string column = field_path(s.manifest, index.fields[0]);
        if (column.empty()) {
            continue;
        }
        auto [it, inserted] = index_names.emplace(column, index.name);
        if (it->second == index.name) {
            segments[column].push_back(&index);
        }
    }
    if (!segments.empty() && (s.manifest.reader_feature_flags & pb::kFlagStableRowIds) != 0U) {
        error = "full-text search over a dataset with stable row ids is not supported by nanolance";
        return false;
    }
    for (const auto& [column, segs] : segments) {
        ColumnIndex c;
        c.name = column;
        std::shared_ptr<InvertedIndex> merged;
        for (const auto* seg : segs) {
            std::shared_ptr<const InvertedIndex> index;
            if (!load_inverted(dataset_path / "_indices" / pb::uuid_string(seg->uuid), index, error)) {
                error = "index " + seg->name + ": " + error;
                return false;
            }
            c.covered.insert(seg->fragment_ids.begin(), seg->fragment_ids.end());
            for (const auto& p : index->partitions) {
                c.num_docs += p->row_ids.size();
                c.total_tokens += p->total_tokens;
            }
            if (segs.size() == 1U) {
                c.index = index;
                break;
            }
            // Several segments: one index of all their partitions, scored with their statistics together.
            if (merged == nullptr) {
                merged = std::make_shared<InvertedIndex>();
                merged->analyzer = index->analyzer;
                merged->codec = index->codec;
            }
            if (index->codec != merged->codec) {
                error = "INVERTED index " + seg->name + ": segments with different posting codecs";
                return false;
            }
            merged->partitions.insert(merged->partitions.end(), index->partitions.begin(), index->partitions.end());
        }
        if (merged != nullptr) {
            c.index = merged;
        }
        out.plan.push_back("MatchQuery: column=" + column + ", index=" + index_names[column] + " (" +
                           std::to_string(c.index->partitions.size()) + " partitions, " + std::to_string(c.num_docs) +
                           " documents)");
        s.columns.emplace(column, std::move(c));
    }
    if (s.columns.empty()) {
        error = "Cannot perform full text search unless an INVERTED index has been created on at least one column";
        return false;
    }
    // The rows that may be returned.
    {
        std::vector<std::uint64_t> narrowed;
        for (const auto& f : s.manifest.fragments) {
            s.mask.fragments.insert(static_cast<std::uint32_t>(f.id));
            if (f.deletion_file.present || (request.filter && request.prefilter)) {
                narrowed.push_back(f.id);
            }
        }
        if (!narrowed.empty()) {
            std::vector<std::uint64_t> addrs;
            const std::string* filter = request.filter && request.prefilter ? &*request.filter : nullptr;
            if (!row_addresses(dataset_path, request, narrowed, filter, addrs, error)) {
                return false;
            }
            for (const auto f : narrowed) {
                for (const auto& frag : s.manifest.fragments) {
                    if (frag.id == f) {
                        s.mask.rows[static_cast<std::uint32_t>(f)].assign(frag.physical_rows, 0U);
                    }
                }
            }
            for (const auto a : addrs) {
                auto& rows = s.mask.rows[static_cast<std::uint32_t>(a >> 32U)];
                const auto offset = static_cast<std::size_t>(a & 0xFFFFFFFFULL);
                if (offset < rows.size()) {
                    rows[offset] = 1U;
                }
            }
        }
    }
    if (request.limit && *request.limit > 0U &&
        (request.query.kind == FtsQuery::Kind::Match || request.query.kind == FtsQuery::Kind::MultiMatch)) {
        // A row outside a column's best `limit` cannot be among the best `limit` of the best of the
        // columns (ranked by score, then row id, as here).
        s.top_k_ = static_cast<std::size_t>(*request.limit);
    }
    Hits hits;
    if (!s.run(request.query, hits, error)) {
        return false;
    }
    if (request.limit && hits.size() > *request.limit) {
        const auto k = static_cast<std::ptrdiff_t>(*request.limit);
        std::nth_element(hits.begin(), hits.begin() + k, hits.end(), better_hit);
        hits.resize(static_cast<std::size_t>(k));
    }
    std::sort(hits.begin(), hits.end(), better_hit);
    if (request.filter && !request.prefilter) {
        out.plan.push_back("FilterExec: " + *request.filter + " (after the search)");
        if (!post_filter(dataset_path, request, hits, error)) {
            return false;
        }
    }
    for (const auto& h : hits) {
        out.row_ids.push_back(h.first);
        out.scores.push_back(h.second);
    }
    (void)version;
    return true;
}

}  // namespace nano_lance
