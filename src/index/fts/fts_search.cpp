// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/base_paths.hpp"
#include "nanolance/fts_search.hpp"
#include "nanolance/work_stats.hpp"
#include "nanolance/overlays.hpp"
#include "nanolance/row_ids.hpp"

#include "index/fts/fts_document_text.hpp"
#include "index/fts/fts_fst.hpp"
#include "base/fts_json.hpp"
#include "index/fts/fts_posting.hpp"
#include "index/index_build.hpp"
#include "index/index_files.hpp"
#include "base/shared_cache.hpp"
#include "table/index_location.hpp"
#include "nanolance/expr.hpp"
#include "nanolance/fts_tokenizer.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"

#include "lance_minimal.pb.hpp"
#include "base/proto_wire.hpp"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <string_view>
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

/// The "granularity" of a match or phrase query ("row" or "list_element"), when it has one.
bool granularity_field(const Value& obj, std::optional<DocGranularity>& out, std::string& error) {
    const Value* g = obj.get("granularity");
    if (g == nullptr || g->kind == Value::Null) {
        return true;
    }
    if (g->kind == Value::String && g->s == "row") {
        out = DocGranularity::Row;
        return true;
    }
    if (g->kind == Value::String && g->s == "list_element") {
        out = DocGranularity::ListElement;
        return true;
    }
    error = "document granularity must be \"row\" or \"list_element\"";
    return false;
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
    // Out-of-range numbers saturate (the bindings reject them first; a converted double must not overflow).
    const auto saturate = [](double v) {
        if (!(v > 0.0)) {
            return std::uint32_t{0};
        }
        return v >= 4294967295.0 ? std::uint32_t{4294967295U} : static_cast<std::uint32_t>(v);
    };
    q.boost = static_cast<float>(boost);
    q.max_expansions = saturate(max_expansions);
    q.prefix_length = saturate(prefix_length);
    const Value* fuzz = m.get("fuzziness");
    if (fuzz != nullptr && fuzz->kind == Value::Null) {
        q.fuzziness.reset();  // None: exact matching, as pylance's default
    } else if (fuzz != nullptr && fuzz->kind == Value::Number) {
        q.fuzziness = saturate(fuzz->n);
    }
    if (!granularity_field(m, q.granularity, error)) {
        return false;
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
        return granularity_field(body, q.granularity, error);
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

DocGranularity inverted_details_granularity(const std::vector<std::uint8_t>& details) {
    // InvertedIndexDetails.document_granularity (field 14): ROW = 0 (the default), LIST_ELEMENT = 1.
    proto_wire::Reader reader(details.data(), details.size());
    proto_wire::Field f;
    DocGranularity out = DocGranularity::Row;
    while (reader.next(f)) {
        if (f.number == 14U && f.wire == 0U && f.value == 1U) {
            out = DocGranularity::ListElement;
        }
    }
    return out;
}

// ── the index ───────────────────────────────────────────────────────────────────────────────────

namespace {

using index_files::FileTable;

constexpr std::size_t kBoundBlock = 128;

/// A posting list's best BM25 document weight in each run of kBoundBlock entries, and the runs
/// best first: what lets a search for the best few rows of one word stop early.
struct BlockBounds {
    std::vector<float> max_weight;
    std::vector<std::uint32_t> order;
    bool ascending = true;  // the posting's documents strictly ascending (what a pruned OR walks)
};

struct Posting {
    std::vector<std::uint32_t> docs;
    std::vector<std::uint32_t> freqs;
    std::size_t block = fts::kPostingBlock;  // the posting blocks' size (128 or 256) it was read with

    mutable std::mutex mutex;
    mutable std::uint32_t bounds_key = 0;
    mutable std::shared_ptr<const BlockBounds> bounds;

    /// The bounds for document length norms `norm` (cached under `key`, the average length's bits).
    std::shared_ptr<const BlockBounds> block_bounds(const float* norm, std::uint32_t key) const;
};

/// One posting list's positions (an index built with_position): posting entry k's are
/// values[start[k] .. start[k + 1]), ascending.
struct Positions {
    std::vector<std::uint32_t> values;
    std::vector<std::uint32_t> start;
};

/// How an index stores its positions: Lance's shared stream (packed or varint deltas, format 2 and
/// 3), or format 1's legacy per-document `_position` lists.
enum class PositionsLayout { PackedShared, VarintShared, Legacy };

struct Partition {
    std::filesystem::path invert;
    std::vector<std::uint8_t> fst_bytes;
    fts::FstMap fst;
    std::unordered_map<std::string, std::uint32_t> token_map;  // the "arrow" token set
    bool use_fst = true;
    std::vector<std::uint64_t> row_ids;
    std::vector<std::uint32_t> num_tokens;
    std::vector<std::uint32_t> elements;  // list-element documents: each one's position in its list (else empty)
    std::uint32_t element_of(std::size_t d) const { return elements.empty() ? 0U : elements[d]; }
    bool quantized_lengths = false;  // 256-document blocks: scoring sees quantized_doc_length(num_tokens)
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
    bool load(const std::vector<std::uint32_t>& ids, fts::TailCodec codec, std::size_t block,
              std::vector<std::shared_ptr<const Posting>>& out, std::string& error) const;

    /// The positions of token `id`, whose posting list is `posting`, loaded on first use.
    mutable std::unordered_map<std::uint32_t, std::shared_ptr<const Positions>> positions;
    bool load_positions(std::uint32_t id, const Posting& posting, PositionsLayout layout,
                        std::shared_ptr<const Positions>& out, std::string& error) const;
};

struct InvertedIndex {
    fts::Analyzer analyzer;
    fts::TailCodec codec = fts::TailCodec::VarintDelta;
    std::size_t block_size = fts::kPostingBlock;  // posting blocks: 128 or 256 documents
    bool with_positions = false;   // positions nanolance reads (see positions_layout)
    PositionsLayout positions_layout = PositionsLayout::PackedShared;
    std::string positions_error;   // with positions nanolance cannot read: why (matches still work)
    std::vector<std::shared_ptr<const Partition>> partitions;
};

/// A u32 varint of `src` at `at`.
bool read_varint(const std::uint8_t* src, std::size_t size, std::size_t& at, std::uint32_t& out) {
    out = 0;
    for (std::uint32_t shift = 0; at < size; shift += 7U) {
        const std::uint8_t b = src[at++];
        if (shift >= 35U) {
            return false;
        }
        out |= static_cast<std::uint32_t>(b & 0x7FU) << shift;
        if ((b & 0x80U) == 0U) {
            return true;
        }
    }
    return false;
}

/// One posting block's position stream (lance-index encoding.rs): every document's positions as
/// deltas, the first absolute. PackedDelta: groups of 128 deltas bit-packed (BitPacker4x, a bit
/// width byte first), the rest varints; VarintDocDelta: all varints.
bool decode_position_block(const std::uint8_t* src, std::size_t size, const std::uint32_t* freqs, std::size_t docs,
                           bool packed, std::vector<std::uint32_t>& out) {
    std::size_t total = 0;
    for (std::size_t d = 0; d < docs; ++d) {
        total += freqs[d];
    }
    std::vector<std::uint32_t> deltas;
    deltas.reserve(total);
    std::size_t at = 0;
    if (packed) {
        std::uint32_t group[fts::kPostingBlock];
        for (std::size_t g = 0; g < total / fts::kPostingBlock; ++g) {
            if (at >= size || src[at] > 32U) {
                return false;
            }
            const std::uint8_t bits = src[at++];
            const std::size_t bytes = static_cast<std::size_t>(bits) * fts::kPostingBlock / 8U;
            if (bytes > size - at) {
                return false;
            }
            fts::bitpack4x_unpack(src + at, bits, group);
            at += bytes;
            deltas.insert(deltas.end(), group, group + fts::kPostingBlock);
        }
    }
    while (deltas.size() < total) {
        std::uint32_t v = 0;
        if (!read_varint(src, size, at, v)) {
            return false;
        }
        deltas.push_back(v);
    }
    if (at != size) {
        return false;
    }
    std::size_t k = 0;
    for (std::size_t d = 0; d < docs; ++d) {
        std::uint32_t previous = 0;
        for (std::uint32_t i = 0; i < freqs[d]; ++i, ++k) {
            previous = i == 0U ? deltas[k] : previous + deltas[k];
            out.push_back(previous);
        }
    }
    return true;
}

/// A byte range of a `_position` element.
struct Piece {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

std::uint32_t u32_le(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

/// One document's positions in format 1's layout (lance-index compress_positions): the count, a
/// sorted block per full 128 positions ([u32 first][u8 bits][packed deltas]), then the remainder's
/// plain u32s. `expect` is the document's frequency.
bool decode_legacy_document(const std::vector<Piece>& e, std::uint32_t expect, std::vector<std::uint32_t>& out) {
    if (e.empty() || e[0].size != 4U) {
        return false;
    }
    const std::uint32_t count = u32_le(e[0].data);
    const std::size_t full = count / fts::kPostingBlock;
    const std::size_t rest = count % fts::kPostingBlock;
    if (count != expect || e.size() != 1U + full + (rest > 0 ? 1U : 0U)) {
        return false;
    }
    std::uint32_t deltas[fts::kPostingBlock];
    for (std::size_t b = 0; b < full; ++b) {
        const Piece& block = e[1U + b];
        if (block.size < 5U || block.data[4] > 32U || block.size != 5U + 16U * block.data[4]) {
            return false;
        }
        fts::bitpack4x_unpack(block.data + 5, block.data[4], deltas);
        std::uint32_t value = u32_le(block.data);
        out.push_back(value);
        for (std::size_t i = 1; i < fts::kPostingBlock; ++i) {
            value += deltas[i];
            out.push_back(value);
        }
    }
    if (rest > 0) {
        const Piece& tail = e.back();
        if (tail.size != 4U * rest) {
            return false;
        }
        for (std::size_t i = 0; i < rest; ++i) {
            out.push_back(u32_le(tail.data + 4 * i));
        }
    }
    return true;
}

/// A format 1 token's positions: its one `_position` row, a list per document of element lists.
bool decode_legacy_positions_row(const ArrowArrayView& view, const Posting& posting,
                                 std::vector<std::uint32_t>& values) {
    const ArrowArrayView* documents = view.children[0];
    if (documents->n_children != 1 || documents->children[0]->n_children != 1) {
        return false;
    }
    const ArrowArrayView* elements = documents->children[0];
    const ArrowArrayView* leaf = elements->children[0];
    const std::int64_t d0 = ArrowArrayViewListChildOffset(documents, 0);
    if (ArrowArrayViewListChildOffset(documents, 1) - d0 != static_cast<std::int64_t>(posting.docs.size())) {
        return false;
    }
    std::vector<Piece> pieces;
    for (std::size_t k = 0; k < posting.docs.size(); ++k) {
        const std::int64_t document = d0 + static_cast<std::int64_t>(k);
        pieces.clear();
        for (std::int64_t j = ArrowArrayViewListChildOffset(elements, document);
             j < ArrowArrayViewListChildOffset(elements, document + 1); ++j) {
            const ArrowBufferView bytes = ArrowArrayViewGetBytesUnsafe(leaf, j);
            pieces.push_back({bytes.data.as_uint8, static_cast<std::size_t>(bytes.size_bytes)});
        }
        if (!decode_legacy_document(pieces, posting.freqs[k], values)) {
            return false;
        }
    }
    return true;
}

bool Partition::load_positions(std::uint32_t id, const Posting& posting, PositionsLayout layout,
                               std::shared_ptr<const Positions>& out, std::string& error) const {
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = positions.find(id);
        if (it != positions.end()) {
            out = it->second;
            return true;
        }
    }
    FileTable table;
    const bool legacy = layout == PositionsLayout::Legacy;
    const std::vector<std::string> columns = legacy ? std::vector<std::string>{"_position"}
                                                    : std::vector<std::string>{"_compressed_position",
                                                                               "_position_block_offset"};
    if (!index_files::take_table(invert, &columns, {id}, table, error)) {
        error = invert.filename().string() + ": " + error;
        return false;
    }
    auto result = std::make_shared<Positions>();
    bool ok = false;
    for (const auto& batch : table.batches) {
        if (batch.length == 0) {
            continue;
        }
        ArrowArrayView view{};
        ArrowError e{};
        if (ArrowArrayViewInitFromSchema(&view, &table.schema, &e) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &e) != NANOARROW_OK ||
            (legacy ? view.n_children < 1 : view.n_children < 2 || view.children[1]->n_children != 1)) {
            ArrowArrayViewReset(&view);
            break;
        }
        if (legacy) {
            ok = decode_legacy_positions_row(view, posting, result->values);
            ArrowArrayViewReset(&view);
            break;
        }
        const std::int64_t row = view.offset;
        const ArrowBufferView stream = ArrowArrayViewGetBytesUnsafe(view.children[0], row);
        const ArrowArrayView* offsets = view.children[1];
        const std::int64_t first = ArrowArrayViewListChildOffset(offsets, row);
        const std::int64_t last = ArrowArrayViewListChildOffset(offsets, row + 1);
        const std::size_t block = posting.block;
        const std::size_t blocks = (posting.docs.size() + block - 1U) / block;
        const auto size = static_cast<std::size_t>(stream.size_bytes);
        ok = static_cast<std::size_t>(last - first) == blocks;
        result->values.reserve(posting.docs.size());
        for (std::size_t b = 0; ok && b < blocks; ++b) {
            const auto begin = static_cast<std::size_t>(ArrowArrayViewGetUIntUnsafe(offsets->children[0], first + static_cast<std::int64_t>(b)));
            const auto end = b + 1U < blocks
                                 ? static_cast<std::size_t>(ArrowArrayViewGetUIntUnsafe(
                                       offsets->children[0], first + static_cast<std::int64_t>(b) + 1))
                                 : size;
            const std::size_t d0 = b * block;
            const std::size_t nd = std::min(block, posting.docs.size() - d0);
            ok = begin <= end && end <= size &&
                 decode_position_block(stream.data.as_uint8 + begin, end - begin, posting.freqs.data() + d0, nd,
                                       layout == PositionsLayout::PackedShared, result->values);
        }
        ArrowArrayViewReset(&view);
        break;
    }
    if (!ok) {
        error = invert.filename().string() + ": malformed positions of token " + std::to_string(id);
        return false;
    }
    result->start.resize(posting.docs.size() + 1U, 0U);
    for (std::size_t k = 0; k < posting.docs.size(); ++k) {
        result->start[k + 1U] = result->start[k] + posting.freqs[k];
    }
    if (result->start.back() != result->values.size()) {
        error = invert.filename().string() + ": positions of token " + std::to_string(id) +
                " do not match its frequencies";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    out = positions.emplace(id, std::move(result)).first->second;
    return true;
}

bool Partition::load(const std::vector<std::uint32_t>& ids, fts::TailCodec codec, std::size_t block,
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
            posting->block = block;
            const auto length = static_cast<std::uint32_t>(ArrowArrayViewGetUIntUnsafe(lengths, row));
            posting->docs.reserve(length);
            posting->freqs.reserve(length);
            ok = at < missing.size() &&
                 fts::decode_posting(blocks, length, codec, block, posting->docs, posting->freqs, error);
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

/// The document length a 256-document-block partition scores (Lance's quantize_doc_length and
/// dequantize_doc_length, index/doc_set.rs): exact below 8, else the top four significant bits,
/// decoded to the bucket floor (the floor only ever shortens a document).
std::uint32_t quantized_doc_length(std::uint32_t value) {
    const std::uint8_t num_bits = fts::bit_width(value);
    std::uint32_t code = value;
    if (num_bits >= 4U) {
        const std::uint32_t shift = num_bits - 4U;
        code = ((value >> shift) & 0x07U) | ((shift + 1U) << 3U);
    }
    const std::uint32_t bits = code & 0x07U;
    const auto shift = static_cast<std::int32_t>(code >> 3U) - 1;
    return shift < 0 ? bits : (bits | 0x08U) << static_cast<std::uint32_t>(shift);
}

float doc_weight_with_norm(std::uint32_t freq, float norm) {
    const auto f = static_cast<float>(freq);
    return (kK1 + 1.0F) * f / (f + norm);
}

/// Off with NANOLANCE_FTS_EXHAUSTIVE=1: every OR then scores every document (what tests compare against).
bool pruning_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("NANOLANCE_FTS_EXHAUSTIVE");
        return v == nullptr || std::string_view(v) != "1";
    }();
    return enabled;
}

std::shared_ptr<const BlockBounds> Posting::block_bounds(const float* norm, std::uint32_t key) const {
    std::lock_guard<std::mutex> lock(mutex);
    if (bounds == nullptr || bounds_key != key) {
        auto b = std::make_shared<BlockBounds>();
        const std::size_t n = docs.size();
        b->max_weight.resize((n + kBoundBlock - 1) / kBoundBlock, 0.0F);
        for (std::size_t k = 0; k < n; ++k) {
            float& m = b->max_weight[k / kBoundBlock];
            m = std::max(m, doc_weight_with_norm(freqs[k], norm[docs[k]]));
        }
        for (std::size_t k = 1; k < n && b->ascending; ++k) {
            b->ascending = docs[k - 1] < docs[k];
        }
        b->order.resize(b->max_weight.size());
        for (std::uint32_t i = 0; i < b->order.size(); ++i) {
            b->order[i] = i;
        }
        std::stable_sort(b->order.begin(), b->order.end(),
                         [&](std::uint32_t x, std::uint32_t y) { return b->max_weight[x] > b->max_weight[y]; });
        bounds = std::move(b);
        bounds_key = key;
    }
    return bounds;
}

std::shared_ptr<const std::vector<float>> Partition::length_norms(float avg) const {
    std::uint32_t key;
    std::memcpy(&key, &avg, sizeof(key));
    std::lock_guard<std::mutex> lock(mutex);
    if (norms == nullptr || norms_key != key) {
        auto v = std::make_shared<std::vector<float>>(num_tokens.size());
        for (std::size_t d = 0; d < num_tokens.size(); ++d) {
            (*v)[d] = length_norm(quantized_lengths ? quantized_doc_length(num_tokens[d]) : num_tokens[d], avg);
        }
        norms = std::move(v);
        norms_key = key;
    }
    return norms;
}

SharedCache<InvertedIndex>& index_cache() {
    static SharedCache<InvertedIndex> cache;
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

bool load_partition(const std::filesystem::path& dir, std::uint64_t id, bool elements, Partition& part,
                    std::string& error) {
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
        std::vector<std::string> columns = {"_rowid", "_num_tokens"};
        if (elements) {
            columns.push_back("_doc_index_0");  // Lance's v3 layout: the element's position in its list
        }
        if (!index_files::read_table(dir / (prefix + "docs.lance"), &columns, LanceRowRange{}, table, error)) {
            return false;
        }
        for (const auto& batch : table.batches) {
            ArrowArrayView view{};
            ArrowError e{};
            if (ArrowArrayViewInitFromSchema(&view, &table.schema, &e) != NANOARROW_OK ||
                ArrowArrayViewSetArray(&view, &batch, &e) != NANOARROW_OK || view.n_children < (elements ? 3 : 2)) {
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
                if (elements) {
                    part.elements.push_back(static_cast<std::uint32_t>(ArrowArrayViewGetUIntUnsafe(view.children[2], row)));
                }
                const auto size = part.row_ids.size();
                part.rows_sorted = part.rows_sorted && (size < 2 || part.row_ids[size - 2] < part.row_ids[size - 1]);
            }
            ArrowArrayViewReset(&view);
        }
    }
    return true;
}

bool load_inverted(const std::filesystem::path& dir, bool elements, std::shared_ptr<const InvertedIndex>& out,
                   std::string& error) {
    const auto meta = dir / "metadata.lance";
    const std::string key = index_files::file_key(meta);
    if (auto hit = index_cache().find(key)) {
        work_stats::count_index_cache(true);
        out = hit;
        return true;
    }
    work_stats::count_index_cache(false);
    auto index = std::make_shared<InvertedIndex>();
    bool found = false;
    const std::string format = metadata_value(meta, "format_version", found, error);
    if (!error.empty()) {
        return false;
    }
    if (!found || (format != "1" && format != "2" && format != "3")) {
        error = "INVERTED index format version " + (found ? format : std::string("1")) +
                " is not supported (1, 2 and 3 are; rebuild the index with Lance 12 or nanolance)";
        return false;
    }
    const std::string block = metadata_value(meta, "posting_block_size", found, error);
    if (found && block != "128" && block != "256") {
        error = "INVERTED index posting blocks of " + block + " documents are not supported (128 and 256 are)";
        return false;
    }
    index->block_size = found && block == "256" ? fts::kLargePostingBlock : fts::kPostingBlock;
    const std::string tail = metadata_value(meta, "posting_tail_codec", found, error);
    if (found && tail == "fixed32_v1") {
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
    bool found_codec = false;
    if (params.with_position && format == "1") {
        // Format 1: no positions_layout; each posting's positions are its legacy _position column.
        index->with_positions = true;
        index->positions_layout = PositionsLayout::Legacy;
    } else if (params.with_position) {
        const std::string layout = metadata_value(meta, "positions_layout", found, error);
        const std::string codec = metadata_value(meta, "positions_codec", found_codec, error);
        if (!found || layout != "shared_stream_v2") {
            index->positions_error = "INVERTED index positions in layout '" +
                                     (found ? layout : std::string("legacy per-document")) +
                                     "' are not supported by nanolance (shared_stream_v2 is)";
        } else if (found_codec && codec != "packed_delta_v1" && codec != "varint_doc_delta_v2") {
            index->positions_error = "INVERTED index positions codec '" + codec + "' is not supported by nanolance";
        } else {
            index->with_positions = true;
            index->positions_layout = found_codec && codec == "varint_doc_delta_v2" ? PositionsLayout::VarintShared
                                                                                   : PositionsLayout::PackedShared;
        }
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
        if (!load_partition(dir, static_cast<std::uint64_t>(p.n), elements, *part, error)) {
            return false;
        }
        part->quantized_lengths = index->block_size == fts::kLargePostingBlock;
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

/// A hit: a row (its id or its address), the element of a list-element document (0 for a row), and the
/// score. Hits are ordered by (row, element).
struct Hit {
    std::uint64_t row = 0;
    float score = 0.0F;
    std::uint32_t element = 0;
};
using Hits = std::vector<Hit>;

bool hit_before(const Hit& a, const Hit& b) { return a.row != b.row ? a.row < b.row : a.element < b.element; }
bool same_doc(const Hit& a, const Hit& b) { return a.row == b.row && a.element == b.element; }

void sort_hits(Hits& h) {
    if (!std::is_sorted(h.begin(), h.end(), hit_before)) {
        std::sort(h.begin(), h.end(), hit_before);
    }
}

/// The better of two hits: the higher score, then the lower row id (and element).
bool better_hit(const Hit& a, const Hit& b) { return a.score != b.score ? a.score > b.score : hit_before(a, b); }

/// Hits keeping only the best `k` (all when 0): a heap whose front is the worst kept.
class HitSink {
public:
    HitSink(Hits& out, std::size_t k) : out_(out), k_(k) {}
    bool limited() const { return k_ != 0U; }
    bool full() const { return k_ != 0U && out_.size() == k_; }
    float worst() const { return out_.front().score; }
    void add(std::uint64_t row, std::uint32_t element, float score) {
        const Hit h{row, score, element};
        if (k_ != 0U && out_.size() == k_ && score < out_.front().score) {
            return;
        }
        if (k_ == 0U) {
            out_.push_back(h);
            return;
        }
        if (out_.size() < k_) {
            out_.push_back(h);
            std::push_heap(out_.begin(), out_.end(), better_hit);
            return;
        }
        const auto& worst = out_.front();
        if (score < worst.score || (score == worst.score && hit_before(worst, h))) {
            return;
        }
        std::pop_heap(out_.begin(), out_.end(), better_hit);
        out_.back() = h;
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
    /// With stable row ids the rows this is asked about are ids, which this turns into addresses.
    const RowIdToAddress* resolver = nullptr;
    /// The caller's row-address masks, which every row must pass as well (null: none).
    const RowAddrMask* caller = nullptr;

    /// `cover`: the fragments the index segment holding the row covers -- with stable row ids a
    /// row that was updated or compacted since has an entry in the old segment too, which is stale.
    bool allows(std::uint64_t addr, const std::set<std::uint32_t>* cover = nullptr) const {
        if (resolver != nullptr && resolver->stable()) {
            if (!resolver->find(addr, addr) || (cover != nullptr && cover->count(static_cast<std::uint32_t>(addr >> 32U)) == 0U)) {
                return false;  // an id whose row is gone, or moved out of the segment's fragments
            }
        }
        if (caller != nullptr && !caller->allows(addr)) {
            return false;  // the caller's mask (`addr` is an address by now)
        }
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
    std::vector<std::uint32_t> elements;  // list-element documents: each one's position in its list
    std::vector<std::vector<std::string>> tokens;  // only documents with tokens
    std::vector<std::vector<std::uint32_t>> positions;  // each token's position
    std::uint64_t total_tokens = 0;
};

struct ColumnIndex {
    std::string name;   // the column
    std::string index_name;  // its index's name (empty when unindexed)
    bool element = false;    // the list-element documents (else the row documents)
    std::shared_ptr<const InvertedIndex> index;
    /// A search of some segments only: all of them, for the token document counts.
    std::shared_ptr<const InvertedIndex> stats;
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
    std::map<std::string, ColumnIndex> columns;    // every column with an INVERTED index
    std::map<std::string, ColumnIndex> unindexed;  // columns searched without one
    RowMask mask;
    AddressToRowId to_id;      // stable row ids: hits are ids until they are returned
    RowIdToAddress resolver;
    /// With stable row ids: each index partition's segment's fragments.
    std::unordered_map<const Partition*, std::shared_ptr<const std::set<std::uint32_t>>> cover;
    const std::set<std::uint32_t>* cover_of(const Partition& part) const {
        const auto it = cover.find(&part);
        return it == cover.end() ? nullptr : it->second.get();
    }
    std::vector<std::string>* plan = nullptr;
    // Scoring buffers, by document, kept zeroed between uses.
    std::vector<float> scores_;
    std::vector<std::uint32_t> seen_;
    std::vector<std::uint32_t> touched_;
    std::vector<std::uint32_t> group_mark_;  // AND: the last position group that counted a document
    /// When only the best `top_k_` rows of a match can matter (a Match or MultiMatch at the root,
    /// with a limit), a match keeps just those.
    std::size_t top_k_ = 0;
    std::size_t indexed_used_ = 0;  // matches answered from an index
    bool element_used_ = false;     // some match was answered by list-element documents

    bool scan_flat(ColumnIndex& c, std::string& error);
    /// Which index (or unindexed documents) of `column` answers `q`: the list-element documents when
    /// `element` is set. Unset granularity: the one index on the column decides.
    bool resolve(const FtsQuery& q, const std::string& column, bool& element, std::string& error) const;
    bool match(const FtsQuery& q, const std::string& column, float boost, Hits& out, std::string& error);
    /// A phrase query in progress (match() with every term required, and their positions checked).
    bool phrase_ = false;
    bool flat_pass_ = false;  // a fuzzy query's pass over the unindexed fragments alone (exact tokens)
    std::uint32_t slop_ = 0;
    /// A partition's scored documents with `need` marks, in row order, into `sink`; the buffers
    /// left zeroed.
    [[gnu::noinline]] void emit(const Partition& part, std::uint32_t need, HitSink& sink);
    /// An OR of several words over a partition, the best rows only, with block-max MaxScore pruning;
    /// false (nothing added) when it cannot be used, for the exhaustive pass to run instead.
    [[gnu::noinline]] bool match_pruned(const Partition& part,
                                        const std::vector<std::shared_ptr<const Posting>>& postings,
                                        const std::vector<float>& weights, const std::vector<std::size_t>& order,
                                        const float* norm, std::uint32_t key, HitSink& sink);
    /// A phrase over a partition: unmark the documents holding every term but not as the phrase
    /// does, so the plain match's emit loop drops them.
    [[gnu::noinline]] bool drop_non_phrases(const Partition& part,
                                            const std::vector<std::shared_ptr<const Posting>>& postings,
                                            const std::vector<std::string>& distinct,
                                            const std::vector<std::size_t>& order,
                                            const std::vector<std::int32_t>& query_positions, PositionsLayout layout,
                                            std::uint32_t need, std::string& error);
    bool run(const FtsQuery& q, Hits& out, std::string& error);
};

/// One query term's positions in a document, and the term's place in the query.
struct PositionCursor {
    const std::uint32_t* values = nullptr;
    std::size_t n = 0;
    std::int32_t query_position = 0;
    std::size_t i = 0;
    std::optional<std::int32_t> relative() const {
        return i < n ? std::optional<std::int32_t>(static_cast<std::int32_t>(values[i]) - query_position) : std::nullopt;
    }
    void advance_to_relative(std::int32_t least) {
        if (i >= n) {
            return;
        }
        const auto target = static_cast<std::uint32_t>(std::max(least + query_position, 0));
        i = static_cast<std::size_t>(std::lower_bound(values + i, values + n, target) - values);
    }
};

/// Whether the terms stand as the phrase does, within `slop` (lance-index wand.rs check_positions:
/// exact -- some base puts every term at base + its query position -- or, with slop, the terms in
/// query order each at most `slop` past the one before, moving every cursor up to the furthest
/// position a pair needs until all pairs fit).
bool phrase_matches(std::vector<PositionCursor> cursors, std::int32_t slop) {
    if (cursors.empty()) {
        return false;
    }
    if (slop == 0) {
        std::size_t lead = 0;
        for (std::size_t k = 1; k < cursors.size(); ++k) {
            if (cursors[k].n < cursors[lead].n) {
                lead = k;
            }
        }
        const auto& a = cursors[lead];
        for (std::size_t j = 0; j < a.n; ++j) {
            if (a.values[j] < static_cast<std::uint32_t>(a.query_position)) {
                continue;
            }
            const std::uint32_t base = a.values[j] - static_cast<std::uint32_t>(a.query_position);
            bool all = true;
            for (std::size_t k = 0; all && k < cursors.size(); ++k) {
                const auto& c = cursors[k];
                all = std::binary_search(c.values, c.values + c.n, base + static_cast<std::uint32_t>(c.query_position));
            }
            if (all) {
                return true;
            }
        }
        return false;
    }
    std::sort(cursors.begin(), cursors.end(),
              [](const PositionCursor& x, const PositionCursor& y) { return x.query_position < y.query_position; });
    for (;;) {
        std::optional<std::int32_t> furthest;
        bool all_same = true;
        for (std::size_t k = 0; k + 1U < cursors.size(); ++k) {
            const auto last = cursors[k].relative();
            const auto next = cursors[k + 1U].relative();
            if (!last || !next) {
                return false;
            }
            const std::int32_t move_to = *last > *next ? *last : std::max(*last + 1, *next - slop);
            furthest = std::max(furthest.value_or(move_to), move_to);
            if (!(*last <= *next && *next <= *last + slop)) {
                all_same = false;
                break;
            }
        }
        if (all_same) {
            return true;
        }
        for (auto& c : cursors) {
            c.advance_to_relative(*furthest);
        }
    }
}

/// The same for a document outside the index, as Lance's flat search decides it
/// (flat_search.rs phrase_matches_positions): the first term's positions are the candidates, and
/// each next term keeps its positions within [candidate + gap, candidate + gap + slop] of one.
bool flat_phrase_matches(const std::vector<std::int32_t>& query_positions,
                         const std::vector<const std::vector<std::uint32_t>*>& positions, std::uint32_t slop) {
    if (positions.empty() || positions.front()->empty()) {
        return false;
    }
    std::vector<std::uint64_t> candidates(positions.front()->begin(), positions.front()->end());
    std::vector<std::uint64_t> next;
    for (std::size_t i = 1; i < positions.size(); ++i) {
        if (query_positions[i] < query_positions[i - 1U]) {
            return false;
        }
        const auto gap = static_cast<std::uint64_t>(query_positions[i] - query_positions[i - 1U]);
        next.clear();
        for (const std::uint32_t p : *positions[i]) {
            const bool fits = std::any_of(candidates.begin(), candidates.end(), [&](std::uint64_t c) {
                return c + gap <= p && p <= c + gap + slop;
            });
            if (fits) {
                next.push_back(p);
            }
        }
        if (next.empty()) {
            return false;
        }
        candidates.swap(next);
    }
    return true;
}

/// The same over an unindexed document's tokens, at their positions.
[[gnu::noinline]] bool flat_phrase_matches(const std::vector<std::string>& tokens,
                                           const std::vector<std::uint32_t>& token_positions,
                                           const std::vector<std::string>& distinct,
                                           const std::vector<std::size_t>& order,
                                           const std::vector<std::int32_t>& query_positions, std::uint32_t slop) {
    std::vector<std::vector<std::uint32_t>> at(distinct.size());
    for (std::size_t j = 0; j < tokens.size(); ++j) {
        for (std::size_t t = 0; t < distinct.size(); ++t) {
            if (tokens[j] == distinct[t]) {
                at[t].push_back(token_positions[j]);
            }
        }
    }
    std::vector<const std::vector<std::uint32_t>*> by_query;
    for (const std::size_t t : order) {
        by_query.push_back(&at[t]);
    }
    return flat_phrase_matches(query_positions, by_query, slop);
}

bool Search::scan_flat(ColumnIndex& c, std::string& error) {
    if (c.flat_loaded) {
        return true;
    }
    c.flat_loaded = true;
    std::vector<std::uint64_t> frags;
    if (!request->fast_search && request->segments.empty()) {
        for (const auto& f : manifest.fragments) {
            if (c.covered.count(static_cast<std::uint32_t>(f.id)) == 0U) {
                frags.push_back(f.id);
            }
        }
    }
    if (frags.empty()) {
        return true;
    }
    // The column's documents: the text leaf below the struct fields of the top-level column the scan reads.
    std::vector<std::string> parts;
    if (index_location::find_field(manifest, c.name, parts) == nullptr) {
        error = "Column " + c.name + " not found in the dataset";
        return false;
    }
    const std::vector<std::string> cols = {parts.front()};
    const std::vector<std::string> below(parts.begin() + 1, parts.end());
    LanceScanRequest scan;
    scan.row_mask = request->row_mask;
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
    const int tc = child_index(schema, parts.front());
    const int ac = child_index(schema, "_rowaddr");
    bool ok = tc >= 0 && ac >= 0;
    if (!ok) {
        error = "cannot read column " + c.name;
    }
    std::vector<std::size_t> path;
    if (ok && !fts::text_child_path(schema.children[tc], below, path)) {
        error = "cannot read column " + c.name;
        ok = false;
    }
    std::vector<fts::Token> tokens;
    std::string joined;  // a list row's text, joined as Lance joins it
    for (auto& batch : batches) {
        if (ok) {
            ArrowArrayView view{};
            ArrowError e{};
            if (ArrowArrayViewInitFromSchema(&view, &schema, &e) != NANOARROW_OK ||
                ArrowArrayViewSetArray(&view, &batch, &e) != NANOARROW_OK) {
                error = std::string("cannot read a batch: ") + e.message;
                ok = false;
            }
            std::vector<const ArrowArrayView*> above;  // the structs above the text, null where they are
            const ArrowArrayView* text = ok ? fts::text_view_at(view.children[tc], path, above) : nullptr;
            if (ok && text == nullptr) {
                error = "cannot read column " + c.name;
                ok = false;
            }
            const fts::TextColumn kind = fts::text_column_kind(text);
            if (ok && kind == fts::TextColumn::kNone) {
                error = "full-text search needs a string column or a list of strings column; " + c.name + " is not one";
                ok = false;
            }
            if (ok && c.element && kind != fts::TextColumn::kStringList) {
                error = "Invalid user input: FTS field path '" + c.name + "' has no List layer and cannot use ListElement document granularity";
                ok = false;
            }
            const auto append = [&](std::uint64_t addr, std::uint32_t element) {
                std::vector<std::string> words;
                std::vector<std::uint32_t> at;
                words.reserve(tokens.size());
                at.reserve(tokens.size());
                for (auto& t : tokens) {
                    words.push_back(std::move(t.text));
                    at.push_back(t.position);
                }
                c.flat.addrs.push_back(addr);
                if (c.element) {
                    c.flat.elements.push_back(element);
                }
                c.flat.positions.push_back(std::move(at));
                c.flat.total_tokens += words.size();
                c.flat.tokens.push_back(std::move(words));
            };
            for (std::int64_t r = 0; ok && r < batch.length; ++r) {
                const std::int64_t row = view.offset + r;
                if (c.element) {
                    // One document per element of a non-null row, empty and null ones included (no tokens).
                    if (ArrowArrayViewIsNull(text, row) || fts::null_above(above, row)) {
                        continue;
                    }
                    const ArrowArrayView* values = text->children[0];
                    const std::int64_t first = ArrowArrayViewListChildOffset(text, text->offset + row);
                    const std::int64_t last = ArrowArrayViewListChildOffset(text, text->offset + row + 1);
                    const std::uint64_t addr = to_id(ArrowArrayViewGetUIntUnsafe(view.children[ac], row));
                    for (std::int64_t k = first; k < last; ++k) {
                        tokens.clear();
                        if (!ArrowArrayViewIsNull(values, k)) {
                            const ArrowStringView sv = ArrowArrayViewGetStringUnsafe(values, k);
                            if (sv.size_bytes > 0) {
                                c.index->analyzer.tokenize(std::string_view(sv.data, static_cast<std::size_t>(sv.size_bytes)), tokens);
                            }
                        }
                        append(addr, static_cast<std::uint32_t>(k - first));
                    }
                    continue;
                }
                std::string_view document;
                if (fts::null_above(above, row) || !fts::row_text(text, kind, row, joined, document)) {
                    continue;
                }
                tokens.clear();
                c.index->analyzer.tokenize(document, tokens);
                if (tokens.empty()) {
                    continue;
                }
                append(to_id(ArrowArrayViewGetUIntUnsafe(view.children[ac], row)), 0U);
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

// Fuzzy matching (Match with fuzziness k > 0): a query token expands to the dictionary terms within k
// edits of it, where an insertion, deletion or substitution is one edit and a transposition is two
// (measured against pylance 12.0.0). Only terms that start with the token's first prefix characters
// qualify, and the first of them in key order are kept (pylance's order, not distance or document
// frequency). max_expansions is one budget for the whole query: the query's tokens spend it in order.

// The code points of a UTF-8 string; a byte that does not start a valid sequence stands for itself.
std::vector<char32_t> code_points(const std::string& s) {
    std::vector<char32_t> out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        char32_t cp = b0;
        if ((b0 & 0xE0U) == 0xC0U) {
            extra = 1;
            cp = b0 & 0x1FU;
        } else if ((b0 & 0xF0U) == 0xE0U) {
            extra = 2;
            cp = b0 & 0x0FU;
        } else if ((b0 & 0xF8U) == 0xF0U) {
            extra = 3;
            cp = b0 & 0x07U;
        }
        bool valid = extra > 0 && i + extra < s.size();
        for (std::size_t j = 1; valid && j <= extra; ++j) {
            const auto bj = static_cast<unsigned char>(s[i + j]);
            valid = (bj & 0xC0U) == 0x80U;
            cp = (cp << 6U) | (bj & 0x3FU);
        }
        if (!valid) {
            cp = b0;
            extra = 0;
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

// Whether the edit distance between a and b is at most k (Levenshtein, with rows that cannot get back
// under k abandoned early). prev and cur are scratch rows, reused across terms.
bool within_edits(const std::vector<char32_t>& a, const std::vector<char32_t>& b, std::size_t k,
                  std::vector<std::size_t>& prev, std::vector<std::size_t>& cur) {
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    if ((n > m ? n - m : m - n) > k) {
        return false;
    }
    prev.resize(m + 1);
    cur.resize(m + 1);
    for (std::size_t j = 0; j <= m; ++j) {
        prev[j] = j;
    }
    for (std::size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        std::size_t row_min = cur[0];
        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t substitute = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0U : 1U);
            cur[j] = std::min(substitute, std::min(prev[j] + 1, cur[j - 1] + 1));
            row_min = std::min(row_min, cur[j]);
        }
        if (row_min > k) {
            return false;
        }
        std::swap(prev, cur);
    }
    return prev[m] <= k;
}

// The dictionary terms `token` expands to, in key order, at most max_expansions of them.
bool expand_token(const InvertedIndex& index, const std::string& token, std::uint32_t k, std::uint32_t prefix,
                  std::uint32_t max_expansions, std::vector<std::string>& out, std::string& error) {
    const std::vector<char32_t> query = code_points(token);
    const std::size_t keep = std::min<std::size_t>(prefix, query.size());
    std::set<std::string> found;
    std::vector<std::size_t> prev;
    std::vector<std::size_t> cur;
    const auto consider = [&](std::string_view key) {
        const std::vector<char32_t> term = code_points(std::string(key));
        if (term.size() < keep || !std::equal(query.begin(), query.begin() + static_cast<std::ptrdiff_t>(keep),
                                              term.begin())) {
            return;
        }
        if (within_edits(query, term, k, prev, cur)) {
            found.emplace(key);
        }
    };
    for (const auto& part : index.partitions) {
        if (part->use_fst) {
            const bool ok = part->fst.for_each(
                [&](std::string_view key, std::uint64_t) { consider(key); });
            if (!ok) {
                error = "the full-text index's token dictionary is malformed";
                return false;
            }
        } else {
            for (const auto& entry : part->token_map) {
                consider(entry.first);
            }
        }
    }
    out.clear();
    for (const auto& term : found) {
        if (out.size() >= max_expansions) {
            break;
        }
        out.push_back(term);
    }
    return true;
}

/// The key of a column's index (or unindexed documents) in Search::columns / unindexed: the column, and
/// for list-element documents the column with a separator no column name can carry.
std::string column_key(const std::string& column, bool element) {
    return element ? column + '\x1f' + "list_element" : column;
}

bool Search::resolve(const FtsQuery& q, const std::string& column, bool& element, std::string& error) const {
    const auto row = columns.find(column_key(column, false));
    const auto list = columns.find(column_key(column, true));
    const bool has_row = row != columns.end();
    const bool has_list = list != columns.end();
    if (q.granularity == DocGranularity::Row) {
        if (!has_row && has_list) {
            error = "Invalid user input: FTS query for field '" + column + "' requested Row document granularity, but the existing FTS "
                    "index uses a different granularity: '" + list->second.index_name + "' (ListElement)";
            return false;
        }
        element = false;
        return true;
    }
    if (q.granularity == DocGranularity::ListElement) {
        if (!has_list && has_row) {
            error = "Invalid user input: FTS query for field '" + column + "' requested ListElement document granularity, but the existing "
                    "FTS index uses a different granularity: '" + row->second.index_name + "' (Row)";
            return false;
        }
        element = true;
        return true;
    }
    if (has_row && has_list) {
        error = "Invalid user input: FTS query for field '" + column + "' is ambiguous because Row and ListElement indexes coexist: '" +
                row->second.index_name + "' (Row), '" + list->second.index_name + "' (ListElement); specify "
                "document_granularity";
        return false;
    }
    element = has_list;
    return true;
}

bool Search::match(const FtsQuery& q, const std::string& column, float boost, Hits& out, std::string& error) {
    bool element = false;
    if (!resolve(q, column, element, error)) {
        return false;
    }
    const std::string key = column_key(column, element);
    element_used_ = element_used_ || element;
    auto it = columns.find(key);
    if (it == columns.end()) {
        // No index: every document is searched as an unindexed one, split as Lance's flat search
        // splits it -- the simple tokenizer alone, no lower-casing, stemming, stop words, folding or
        // length limit.
        it = unindexed.find(key);
        if (it == unindexed.end()) {
            std::vector<std::string> parts;
            if (index_location::find_field(manifest, column, parts) == nullptr) {
                error = "Column " + column + " not found in the dataset";
                return false;
            }
            auto index = std::make_shared<InvertedIndex>();
            fts::AnalyzerParams raw;
            raw.lower_case = false;
            raw.stem = false;
            raw.remove_stop_words = false;
            raw.ascii_folding = false;
            raw.max_token_length.reset();
            raw.with_position = true;  // a phrase query's positions, as flat search reads them
            if (!index->analyzer.init(raw, error)) {
                return false;
            }
            ColumnIndex c;
            c.name = column;
            c.element = element;
            c.index = std::move(index);
            it = unindexed.emplace(key, std::move(c)).first;
        }
    }
    ColumnIndex& c = it->second;
    if (columns.count(key) != 0U) {
        ++indexed_used_;
    }
    std::vector<fts::Token> query_tokens;
    c.index->analyzer.tokenize(q.text, query_tokens);
    // Fuzzy (pylance's rule, measured): the indexed rows match each token's expansions, one budget for
    // the query; the unindexed fragments match the query's exact tokens, scored as an exact query would.
    // The fragments are a pass of their own (exact tokens, no indexed rows), merged into this one's hits.
    const std::uint32_t fuzziness = q.fuzziness.value_or(0U);
    const bool fuzzy = fuzziness > 0U && !phrase_ && !flat_pass_;
    Hits fragment_hits;
    if (fuzzy) {
        // Each query token becomes its expansions, all at the token's position: they are one group there
        // (AND needs one of them in a document) and each scores as its own term, as an OR of terms does.
        std::vector<fts::Token> expanded;
        std::vector<std::string> terms;
        bool some_token_has_no_term = false;
        // One budget for the whole query, spent in query-token order (a term two tokens share uses two).
        std::uint32_t budget = q.max_expansions;
        for (const auto& t : query_tokens) {
            if (!expand_token(*c.index, t.text, fuzziness, q.prefix_length, budget, terms, error)) {
                return false;
            }
            budget -= static_cast<std::uint32_t>(terms.size());
            some_token_has_no_term = some_token_has_no_term || terms.empty();
            for (auto& term : terms) {
                fts::Token token;
                token.text = std::move(term);
                token.position = t.position;
                expanded.push_back(std::move(token));
            }
        }
        FtsQuery exact = q;
        exact.fuzziness = 0;
        flat_pass_ = true;
        const bool ok = match(exact, column, 1.0F, fragment_hits, error);
        flat_pass_ = false;
        if (!ok) {
            return false;
        }
        // AND with a token that has no indexed term: no indexed row can match, only the fragments can.
        if ((some_token_has_no_term && q.and_operator) || expanded.empty()) {
            if (boost != 1.0F) {
                for (auto& h : fragment_hits) {
                    h.score *= boost;
                }
            }
            sort_hits(fragment_hits);
            out = std::move(fragment_hits);
            return true;
        }
        query_tokens = std::move(expanded);
    }
    if (query_tokens.empty()) {
        return true;
    }
    // Distinct tokens, and each query token's place among them (duplicates score again). Outside a
    // phrase, Lance keeps one of each (token, position) pair (load_posting_lists' dedup), so a
    // token repeated at one position (an ngram tokenizer's) scores once.
    std::vector<std::string> distinct;
    std::vector<std::size_t> order;
    std::vector<std::uint32_t> order_positions;
    for (const auto& t : query_tokens) {
        const auto at = std::find(distinct.begin(), distinct.end(), t.text);
        const auto idx = static_cast<std::size_t>(at - distinct.begin());
        if (!phrase_ && at != distinct.end()) {
            bool repeated = false;
            for (std::size_t k = 0; k < order.size(); ++k) {
                repeated = repeated || (order[k] == idx && order_positions[k] == t.position);
            }
            if (repeated) {
                continue;
            }
        }
        order.push_back(idx);
        order_positions.push_back(t.position);
        if (at == distinct.end()) {
            distinct.push_back(t.text);
        }
    }
    const std::size_t nt = distinct.size();
    // AND outside a phrase: Lance unions the tokens sharing a query position (an ngram tokenizer
    // puts every gram at position 0; the code analyzer's preserved original shares its parts'), so
    // a document needs one token of every position's group (summarize_position_matches).
    std::vector<std::vector<std::size_t>> groups;
    {
        std::map<std::uint32_t, std::vector<std::size_t>> by_position;
        for (std::size_t k = 0; k < order.size(); ++k) {
            auto& g = by_position[order_positions[k]];
            if (std::find(g.begin(), g.end(), order[k]) == g.end()) {
                g.push_back(order[k]);
            }
        }
        for (auto& [position, g] : by_position) {
            groups.push_back(std::move(g));
        }
    }
    const bool singles = std::all_of(groups.begin(), groups.end(), [](const auto& g) { return g.size() == 1U; });
    // A phrase: each query term's place, counted from the first term kept (Lance's
    // collect_query_tokens), and every distinct term required.
    std::vector<std::int32_t> query_positions;
    if (phrase_) {
        for (const auto& t : query_tokens) {
            query_positions.push_back(static_cast<std::int32_t>(t.position - query_tokens.front().position));
        }
    }
    const bool and_terms = q.and_operator || phrase_;
    const bool check_phrase = phrase_ && order.size() > 1U;  // positions to check
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
        if (!parts[p]->load(ids, c.index->codec, c.index->block_size, loaded, error)) {
            return false;
        }
        postings[p].assign(nt, nullptr);
        for (std::size_t k = 0; k < ids.size(); ++k) {
            postings[p][which[k]] = loaded[k];
            token_docs[which[k]] += loaded[k]->docs.size();
        }
    }
    if (c.stats != nullptr) {
        // Some segments only: the document counts are all segments'.
        std::fill(token_docs.begin(), token_docs.end(), 0U);
        for (const auto& part : c.stats->partitions) {
            std::vector<std::uint32_t> ids;
            std::vector<std::size_t> which;
            for (std::size_t t = 0; t < nt; ++t) {
                if (const auto id = part->token_id(distinct[t])) {
                    ids.push_back(*id);
                    which.push_back(t);
                }
            }
            std::vector<std::shared_ptr<const Posting>> loaded;
            if (!part->load(ids, c.stats->codec, c.stats->block_size, loaded, error)) {
                return false;
            }
            for (std::size_t k = 0; k < ids.size(); ++k) {
                token_docs[which[k]] += loaded[k]->docs.size();
            }
        }
    }
    // The statistics the indexed documents are scored with. Row documents: the index's own. List-element
    // documents with unindexed ones beside them: every document's, the unindexed ones counted in (Lance
    // scores them so, measured against pylance 12.0.0).
    std::uint64_t scoring_docs_total = c.num_docs;
    std::uint64_t scoring_tokens = c.total_tokens;
    std::vector<std::uint64_t> scoring_docs = token_docs;
    if (c.element && !fuzzy) {
        if (!scan_flat(c, error)) {
            return false;
        }
        if (!c.flat.addrs.empty()) {
            for (const auto& words : c.flat.tokens) {
                for (std::size_t t = 0; t < nt; ++t) {
                    if (std::find(words.begin(), words.end(), distinct[t]) != words.end()) {
                        ++scoring_docs[t];
                    }
                }
            }
            scoring_docs_total += c.flat.addrs.size();
            scoring_tokens += c.flat.total_tokens;
        }
    }
    const float avg = static_cast<float>(scoring_tokens) / static_cast<float>(scoring_docs_total);
    std::vector<float> weights(nt, 0.0F);
    for (std::size_t t = 0; t < nt; ++t) {
        weights[t] = scoring_docs[t] == 0U ? 0.0F : idf(scoring_docs[t], scoring_docs_total);
    }
    Hits hits;
    // Only the best rows when the caller needs only those (a positive boost keeps the order).
    HitSink sink(hits, boost > 0.0F ? top_k_ : 0U);
    // Indexed documents: their scores from the index's statistics, accumulated in a dense array per
    // partition and read back in document (row) order.
    for (std::size_t p = 0; p < parts.size(); ++p) {
        const Partition& part = *parts[p];
        if (and_terms) {
            bool all = true;
            if (phrase_) {
                for (std::size_t t = 0; t < nt; ++t) {
                    all = all && postings[p][t] != nullptr;
                }
            } else {
                for (const auto& g : groups) {
                    bool any = false;
                    for (const std::size_t t : g) {
                        any = any || postings[p][t] != nullptr;
                    }
                    all = all && any;
                }
            }
            if (!all) {
                continue;
            }
        }
        const std::size_t n = part.row_ids.size();
        const auto norms = part.length_norms(avg);
        const float* norm = norms->data();
        if (!flat_pass_ && nt > 1U && sink.limited() && !and_terms && !fuzzy && pruning_enabled()) {
            std::uint32_t key;
            std::memcpy(&key, &avg, sizeof(key));
            if (match_pruned(part, postings[p], weights, order, norm, key, sink)) {
                continue;
            }
        }
        if (!flat_pass_ && nt == 1U && sink.limited() && !check_phrase) {
            // One word, the best rows only: its runs best first, until no run can beat the rows
            // kept. (A word repeated in the query scores its weight that many times over, which
            // keeps the order.)
            const Posting* posting = postings[p][0].get();
            if (posting == nullptr) {
                continue;
            }
            const auto* part_cover = cover_of(part);
            std::uint32_t key;
            std::memcpy(&key, &avg, sizeof(key));
            const auto bounds = posting->block_bounds(norm, key);
            const float w = weights[0];
            const std::size_t reps = order.size();
            const auto score_of = [&](float dw) {
                float total = 0.0F;
                for (std::size_t r = 0; r < reps; ++r) {
                    total += w * dw;
                }
                return total;
            };
            for (const std::uint32_t b : bounds->order) {
                if (sink.full() && score_of(bounds->max_weight[b]) < sink.worst()) {
                    break;
                }
                const std::size_t end = std::min(posting->docs.size(), (b + 1U) * kBoundBlock);
                for (std::size_t k = b * kBoundBlock; k < end; ++k) {
                    const std::uint32_t d = posting->docs[k];
                    const std::uint64_t row = part.row_ids[d];
                    if (mask.allows(row, part_cover)) {
                        sink.add(row, part.element_of(d), score_of(doc_weight_with_norm(posting->freqs[k], norm[d])));
                    }
                }
            }
            continue;
        }
        if (scores_.size() < n) {
            scores_.resize(n, 0.0F);
            seen_.resize(n, 0U);
        }
        touched_.clear();
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
        if (and_terms && !phrase_ && singles) {
            // One token per position (the usual query): each token's documents count once.
            for (const auto& g : groups) {
                if (postings[p][g.front()] != nullptr) {
                    for (const std::uint32_t d : postings[p][g.front()]->docs) {
                        seen_[d] += 2U;
                    }
                }
            }
            need = 1U + 2U * static_cast<std::uint32_t>(groups.size());
        } else if (and_terms && phrase_) {
            // Count each distinct token's documents (above the "touched" bit).
            for (std::size_t t = 0; t < nt; ++t) {
                for (const std::uint32_t d : postings[p][t]->docs) {
                    seen_[d] += 2U;
                }
            }
            need = 1U + 2U * static_cast<std::uint32_t>(nt);
        } else if (and_terms) {
            // Count each position group's documents once (above the "touched" bit).
            if (group_mark_.size() < n) {
                group_mark_.resize(n, 0U);
            }
            for (std::size_t g = 0; g < groups.size(); ++g) {
                const auto stamp = static_cast<std::uint32_t>(g + 1U);
                for (const std::size_t t : groups[g]) {
                    if (postings[p][t] == nullptr) {
                        continue;
                    }
                    for (const std::uint32_t d : postings[p][t]->docs) {
                        if (group_mark_[d] != stamp) {
                            group_mark_[d] = stamp;
                            seen_[d] += 2U;
                        }
                    }
                }
            }
            for (const std::uint32_t d : touched_) {
                group_mark_[d] = 0U;
            }
            need = 1U + 2U * static_cast<std::uint32_t>(groups.size());
        }
        if (check_phrase &&
            !drop_non_phrases(part, postings[p], distinct, order, query_positions, c.index->positions_layout, need,
                              error)) {
            return false;
        }
        if (flat_pass_) {
            // The indexed rows belong to the other pass; only the scoring buffers are cleared here.
            for (const std::uint32_t d : touched_) {
                scores_[d] = 0.0F;
                seen_[d] = 0U;
            }
            touched_.clear();
        } else {
            emit(part, need, sink);
        }
    }
    // Documents outside the index: their scores from the index's statistics and theirs.
    if (!scan_flat(c, error)) {
        return false;
    }
    for (const auto& h : fragment_hits) {
        sink.add(h.row, h.element, h.score);
    }
    if (!fuzzy && !c.flat.addrs.empty()) {
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
            bool every_group = true;
            for (const auto& g : groups) {
                bool any = false;
                for (const std::size_t t : g) {
                    any = any || counts[d][t] != 0U;
                }
                every_group = every_group && any;
            }
            if (found == 0U || (and_terms && (phrase_ ? found != nt : !every_group))) {
                continue;
            }
            if (check_phrase &&
                !flat_phrase_matches(c.flat.tokens[d], c.flat.positions[d], distinct, order, query_positions,
                                     slop_)) {
                continue;
            }
            float s = 0.0F;
            const auto dl = static_cast<std::uint32_t>(c.flat.tokens[d].size());
            for (const std::size_t t : order) {
                if (counts[d][t] != 0U) {
                    s += flat_weights[t] * doc_weight(counts[d][t], dl, flat_avg);
                }
            }
            sink.add(c.flat.addrs[d], c.flat.elements.empty() ? 0U : c.flat.elements[d], s);
        }
    }
    if (boost != 1.0F) {
        for (auto& h : hits) {
            h.score *= boost;
        }
    }
    sort_hits(hits);
    out = std::move(hits);
    return true;
}

bool Search::match_pruned(const Partition& part, const std::vector<std::shared_ptr<const Posting>>& postings,
                          const std::vector<float>& weights, const std::vector<std::size_t>& order,
                          const float* norm, std::uint32_t key, HitSink& sink) {
    // Lance answers these with block-max WAND; this is block-max MaxScore over the same bounds. Documents
    // are visited in order. A word's bound is its weight times its best document weight (overall, or in
    // the run of kBoundBlock entries a cursor is in), times its repeats in the query. Words whose bounds
    // together cannot reach the worst row kept are "non-essential": only documents of the others are
    // candidates, and those words are looked up for a candidate whose bound still can. A candidate is
    // scored as the exhaustive pass scores it (its words in `order`, summed in float from 0), so rows and
    // scores are the same; bounds are summed in double with a margin over float rounding, and a document
    // is skipped only when its bound is below the worst kept, never equal (ties go to the lower row).
    constexpr double kMargin = 1.0 + 1e-5;
    constexpr std::size_t kMaxTerms = 64;
    struct Term {
        const Posting* posting = nullptr;
        std::shared_ptr<const BlockBounds> bounds;
        double scale = 0.0;  // weight times repeats
        double bound = 0.0;  // scale times the best document weight
        std::size_t cursor = 0;
        std::size_t index = 0;  // into `weights`
    };
    std::vector<Term> terms;
    std::vector<std::uint32_t> repeats(weights.size(), 0U);
    for (const std::size_t t : order) {
        ++repeats[t];
    }
    for (std::size_t t = 0; t < postings.size(); ++t) {
        if (postings[t] == nullptr || postings[t]->docs.empty() || repeats[t] == 0U) {
            continue;
        }
        Term term;
        term.posting = postings[t].get();
        term.bounds = postings[t]->block_bounds(norm, key);
        if (!term.bounds->ascending || weights[t] < 0.0F) {
            return false;
        }
        term.scale = static_cast<double>(weights[t]) * repeats[t];
        const float best = *std::max_element(term.bounds->max_weight.begin(), term.bounds->max_weight.end());
        term.bound = term.scale * best;
        term.index = t;
        terms.push_back(std::move(term));
    }
    if (terms.size() < 2U || terms.size() > kMaxTerms) {
        return false;
    }
    // Ascending bounds: a prefix of them is the non-essential set.
    std::sort(terms.begin(), terms.end(), [](const Term& a, const Term& b) { return a.bound < b.bound; });
    const std::size_t n_terms = terms.size();
    std::vector<double> prefix(n_terms + 1U, 0.0);
    for (std::size_t i = 0; i < n_terms; ++i) {
        prefix[i + 1U] = prefix[i] + terms[i].bound;
    }
    double threshold = -1.0;  // the worst row kept, once the sink is full
    std::size_t first_essential = 0;
    const auto refresh = [&] {
        if (!sink.full()) {
            return;
        }
        threshold = static_cast<double>(sink.worst());
        while (first_essential < n_terms && prefix[first_essential + 1U] * kMargin < threshold) {
            ++first_essential;
        }
    };
    const auto at = [](const Term& t) { return t.posting->docs[t.cursor]; };
    const auto live = [](const Term& t) { return t.cursor < t.posting->docs.size(); };
    // Moves `t` to its first document at or after `d`.
    const auto seek = [](Term& t, std::uint32_t d) {
        const auto& docs = t.posting->docs;
        if (t.cursor < docs.size() && docs[t.cursor] < d) {
            t.cursor = static_cast<std::size_t>(
                std::lower_bound(docs.begin() + static_cast<std::ptrdiff_t>(t.cursor), docs.end(), d) - docs.begin());
        }
    };
    const auto* part_cover = cover_of(part);
    std::vector<float> contribution(weights.size(), 0.0F);
    std::vector<std::uint8_t> present(weights.size(), 0U);
    refresh();
    while (first_essential < n_terms) {
        std::uint32_t d = std::numeric_limits<std::uint32_t>::max();
        for (std::size_t i = first_essential; i < n_terms; ++i) {
            if (live(terms[i])) {
                d = std::min(d, at(terms[i]));
            }
        }
        if (d == std::numeric_limits<std::uint32_t>::max()) {
            break;  // every essential word is exhausted: no other document can reach the rows kept
        }
        if (threshold >= 0.0) {
            // The run each essential cursor is in bounds every document up to the run's last one.
            double bound = prefix[first_essential];
            std::uint32_t window_end = std::numeric_limits<std::uint32_t>::max();
            for (std::size_t i = first_essential; i < n_terms; ++i) {
                const Term& t = terms[i];
                if (!live(t)) {
                    continue;
                }
                const std::size_t run = t.cursor / kBoundBlock;
                bound += t.scale * t.bounds->max_weight[run];
                const std::size_t last = std::min(t.posting->docs.size(), (run + 1U) * kBoundBlock) - 1U;
                window_end = std::min(window_end, t.posting->docs[last]);
            }
            if (bound * kMargin < threshold) {
                // No document up to window_end can beat the rows kept.
                const std::uint32_t next = window_end == std::numeric_limits<std::uint32_t>::max() ? window_end
                                                                                                   : window_end + 1U;
                for (std::size_t i = first_essential; i < n_terms; ++i) {
                    if (next == std::numeric_limits<std::uint32_t>::max()) {
                        terms[i].cursor = terms[i].posting->docs.size();
                    } else {
                        seek(terms[i], next);
                    }
                }
                continue;
            }
        }
        // The candidate's essential words, then the others while they can still lift it past the worst.
        double partial = 0.0;
        for (std::size_t i = first_essential; i < n_terms; ++i) {
            Term& t = terms[i];
            if (live(t) && at(t) == d) {
                const float dw = doc_weight_with_norm(t.posting->freqs[t.cursor], norm[d]);
                contribution[t.index] = dw;
                present[t.index] = 1U;
                partial += t.scale * dw;
            }
        }
        bool skip = false;
        for (std::size_t i = first_essential; i-- > 0;) {
            if (threshold >= 0.0 && (partial + prefix[i + 1U]) * kMargin < threshold) {
                skip = true;
                break;
            }
            Term& t = terms[i];
            seek(t, d);
            if (live(t) && at(t) == d) {
                const float dw = doc_weight_with_norm(t.posting->freqs[t.cursor], norm[d]);
                contribution[t.index] = dw;
                present[t.index] = 1U;
                partial += t.scale * dw;
            }
        }
        if (!skip) {
            float score = 0.0F;
            for (const std::size_t t : order) {
                if (present[t] != 0U) {
                    score += weights[t] * contribution[t];
                }
            }
            const std::uint64_t row = part.row_ids[d];
            if (mask.allows(row, part_cover)) {
                sink.add(row, part.element_of(d), score);
            }
        }
        for (std::size_t t = 0; t < present.size(); ++t) {
            present[t] = 0U;
        }
        for (std::size_t i = first_essential; i < n_terms; ++i) {
            if (live(terms[i]) && at(terms[i]) == d) {
                ++terms[i].cursor;
            }
        }
        refresh();
    }
    return true;
}

void Search::emit(const Partition& part, std::uint32_t need, HitSink& sink) {
    const auto* part_cover = cover_of(part);
    const auto one = [&](std::uint32_t d) {
        if (seen_[d] == need) {
            const std::uint64_t row = part.row_ids[d];
            if (mask.allows(row, part_cover)) {
                sink.add(row, part.element_of(d), scores_[d]);
            }
        }
        scores_[d] = 0.0F;
        seen_[d] = 0U;
    };
    const std::size_t n = part.row_ids.size();
    if (part.rows_sorted && touched_.size() * 8U > n) {
        for (std::uint32_t d = 0; d < n; ++d) {
            if (seen_[d] != 0U) {
                one(d);
            }
        }
    } else {
        if (part.rows_sorted) {
            std::sort(touched_.begin(), touched_.end());
        }
        for (const std::uint32_t d : touched_) {
            one(d);
        }
    }
}

bool Search::drop_non_phrases(const Partition& part, const std::vector<std::shared_ptr<const Posting>>& postings,
                              const std::vector<std::string>& distinct, const std::vector<std::size_t>& order,
                              const std::vector<std::int32_t>& query_positions, PositionsLayout layout,
                              std::uint32_t need, std::string& error) {
    // The distinct terms' positions.
    std::vector<std::shared_ptr<const Positions>> positions(distinct.size());
    for (std::size_t t = 0; t < distinct.size(); ++t) {
        const auto id = part.token_id(distinct[t]);
        if (!id || !part.load_positions(*id, *postings[t], layout, positions[t], error)) {
            if (error.empty()) {
                error = "positions of a phrase term are missing";
            }
            return false;
        }
    }
    std::vector<PositionCursor> cursors(order.size());
    for (const std::uint32_t d : touched_) {
        if (seen_[d] != need) {
            continue;
        }
        for (std::size_t i = 0; i < order.size(); ++i) {
            const std::size_t t = order[i];
            const auto& docs = postings[t]->docs;
            const auto k = static_cast<std::size_t>(std::lower_bound(docs.begin(), docs.end(), d) - docs.begin());
            const auto& pos = *positions[t];
            cursors[i] = PositionCursor{pos.values.data() + pos.start[k], pos.start[k + 1U] - pos.start[k],
                                        query_positions[i], 0};
        }
        if (!phrase_matches(cursors, static_cast<std::int32_t>(slop_))) {
            seen_[d] = 1U;  // touched, not kept
        }
    }
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
            // A string query asks for row documents, as Lance's full_text_query does.
            FtsQuery per = q;
            if (!per.granularity) {
                per.granularity = DocGranularity::Row;
            }
            std::vector<std::string> cols = q.columns;
            if (cols.empty()) {
                for (const auto& [key, c] : columns) {
                    if (!c.element) {
                        cols.push_back(c.name);
                    }
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
                if (!match(per, cols[i], q.boosts.empty() ? 1.0F : q.boosts[i], one, error)) {
                    return false;
                }
                Hits merged;
                std::size_t a = 0;
                std::size_t b = 0;
                while (a < out.size() || b < one.size()) {
                    if (b == one.size() || (a < out.size() && hit_before(out[a], one[b]))) {
                        merged.push_back(out[a++]);
                    } else if (a == out.size() || hit_before(one[b], out[a])) {
                        merged.push_back(one[b++]);
                    } else {
                        merged.push_back(Hit{out[a].row, std::max(out[a].score, one[b].score), out[a].element});
                        ++a;
                        ++b;
                    }
                }
                out = std::move(merged);
            }
            return true;
        }
        case FtsQuery::Kind::Phrase: {
            if (q.columns.empty()) {
                // No column: the one column with an index (Lance's fill_fts_query_column).
                if (columns.size() != 1U) {
                    error = columns.empty() ? "Cannot perform full text search unless an INVERTED index has been "
                                              "created on at least one column"
                                            : "the column must be specified in the query";
                    return false;
                }
                FtsQuery filled = q;
                filled.columns = {columns.begin()->second.name};
                return run(filled, out, error);
            }
            if (q.columns.size() != 1U) {
                error = "a phrase query needs one column";
                return false;
            }
            // An unindexed column is searched as flat search does, with its positions.
            bool element = false;
            if (!resolve(q, q.columns[0], element, error)) {
                return false;
            }
            const auto it = columns.find(column_key(q.columns[0], element));
            if (it != columns.end() && !it->second.index->analyzer.params().with_position) {
                error = "position is not found but required for phrase queries, try recreating the index with "
                        "position";
                return false;
            }
            if (it != columns.end() && !it->second.index->with_positions) {
                error = it->second.index->positions_error;
                return false;
            }
            // Every term, at its place in the phrase (within the slop), scored as their match.
            FtsQuery terms = q;
            terms.kind = FtsQuery::Kind::Match;
            terms.and_operator = true;
            terms.fuzziness = 0;
            phrase_ = true;
            slop_ = q.slop;
            const bool ok = match(terms, q.columns[0], 1.0F, out, error);
            phrase_ = false;
            return ok;
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
                while (b < neg.size() && hit_before(neg[b], h)) {
                    ++b;
                }
                if (b < neg.size() && same_doc(neg[b], h)) {
                    h.score -= q.negative_boost * neg[b].score;
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
                    while (b < one.size() && hit_before(one[b], h)) {
                        ++b;
                    }
                    if (b < one.size() && same_doc(one[b], h)) {
                        kept.push_back(Hit{h.row, h.score + one[b].score, h.element});
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
                    if (b == one.size() || (a < out.size() && hit_before(out[a], one[b]))) {
                        merged.push_back(out[a++]);
                    } else if (a == out.size() || hit_before(one[b], out[a])) {
                        if (union_should) {
                            merged.push_back(one[b]);
                        }
                        ++b;
                    } else {
                        merged.push_back(Hit{out[a].row, out[a].score + one[b].score, out[a].element});
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
                std::set<std::pair<std::uint64_t, std::uint32_t>> excluded;
                for (const auto& h : one) {
                    excluded.insert({h.row, h.element});
                }
                out.erase(std::remove_if(out.begin(), out.end(), [&](const auto& h) { return excluded.count({h.row, h.element}) != 0U; }),
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
        addrs.push_back(h.row);
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
    hits.erase(std::remove_if(hits.begin(), hits.end(), [&](const auto& h) { return pass.count(h.row) == 0U; }),
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
    // The INVERTED indexes, by column and granularity: the segments of the first index on each.
    struct Group {
        std::string column;
        bool element = false;
        std::string name;
        std::vector<const pb::IndexMetadata*> segs;
    };
    std::map<std::string, Group> groups;  // by column_key
    for (const auto& index : s.manifest.indices) {
        if (index.fields.size() != 1U || !is_inverted_index_url(index.details_type_url)) {
            continue;
        }
        const std::string column = field_path(s.manifest, index.fields[0]);
        if (column.empty()) {
            continue;
        }
        const bool element = inverted_details_granularity(index.details_value) == DocGranularity::ListElement;
        const std::string key = column_key(column, element);
        auto [it, inserted] = groups.emplace(key, Group{column, element, index.name, {}});
        if (it->second.name == index.name) {
            it->second.segs.push_back(&index);
        }
    }
    if (!AddressToRowId::build(s.manifest, s.to_id, error) ||
        !RowIdToAddress::build(dataset_path, s.manifest, s.resolver, error)) {
        return false;
    }
    s.mask.resolver = &s.resolver;
    s.mask.caller = request.row_mask;
    const std::set<std::array<std::uint8_t, 16>> chosen(request.segments.begin(), request.segments.end());
    for (const auto& uuid : chosen) {
        bool known = false;
        for (const auto& [key, group] : groups) {
            for (const auto* seg : group.segs) {
                known = known || seg->uuid == uuid;
            }
        }
        if (!known) {
            error = "FTS segment UUID " + pb::uuid_string(uuid) + " is not present in the attached query context for "
                    "dataset version " + std::to_string(version);
            return false;
        }
    }
    for (const auto& [key, group] : groups) {
        const auto& segs = group.segs;
        ColumnIndex c;
        c.name = group.column;
        c.element = group.element;
        c.index_name = group.name;
        std::shared_ptr<InvertedIndex> merged;
        std::shared_ptr<InvertedIndex> picked;  // the chosen segments, when some are
        for (const auto* seg : segs) {
            std::shared_ptr<const InvertedIndex> index;
            std::filesystem::path seg_dir;
            if (!index_dir_of(dataset_path, s.manifest, *seg, seg_dir, error) ||
                !load_inverted(seg_dir, group.element, index, error)) {
                error = "index " + seg->name + ": " + error;
                return false;
            }
            if (s.resolver.stable()) {
                auto fragments = std::make_shared<std::set<std::uint32_t>>(seg->fragment_ids.begin(),
                                                                          seg->fragment_ids.end());
                for (const auto& p : index->partitions) {
                    s.cover[p.get()] = fragments;
                }
            }
            if (!chosen.empty()) {
                if (picked == nullptr) {
                    picked = std::make_shared<InvertedIndex>();
                    picked->analyzer = index->analyzer;
                    picked->codec = index->codec;
                    picked->block_size = index->block_size;
                }
                if (chosen.count(seg->uuid) != 0U) {
                    c.covered.insert(seg->fragment_ids.begin(), seg->fragment_ids.end());
                    picked->partitions.insert(picked->partitions.end(), index->partitions.begin(),
                                              index->partitions.end());
                }
            } else {
                c.covered.insert(seg->fragment_ids.begin(), seg->fragment_ids.end());
            }
            for (const auto stale : fragments_stale_for_index(s.manifest, seg->fields, seg->dataset_version)) {
                c.covered.erase(stale);  // overlaid since the index was built: searched as unindexed
            }
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
                merged->block_size = index->block_size;
            }
            if (index->codec != merged->codec || index->block_size != merged->block_size) {
                error = "INVERTED index " + seg->name + ": segments with different posting codecs";
                return false;
            }
            merged->partitions.insert(merged->partitions.end(), index->partitions.begin(), index->partitions.end());
        }
        if (merged != nullptr) {
            c.index = merged;
        }
        if (picked != nullptr) {
            c.stats = c.index;
            c.index = picked;
        }
        out.plan.push_back("MatchQuery: column=" + group.column + ", index=" + group.name + " (" +
                           std::to_string(c.index->partitions.size()) + " partitions, " + std::to_string(c.num_docs) +
                           " documents)");
        s.columns.emplace(key, std::move(c));
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
    if (!request.limit && !s.unindexed.empty() && s.indexed_used_ == 0U) {
        // Only columns without an index, and no limit: rows come in scan order, as Lance's flat
        // search returns them.
        sort_hits(hits);
    } else {
        std::sort(hits.begin(), hits.end(), better_hit);
    }
    if (s.resolver.stable()) {  // hits are ids; what is returned and filtered are addresses
        for (auto& hit : hits) {
            s.resolver.find(hit.row, hit.row);
        }
    }
    if (request.filter && !request.prefilter) {
        out.plan.push_back("FilterExec: " + *request.filter + " (after the search)");
        if (!post_filter(dataset_path, request, hits, error)) {
            return false;
        }
    }
    out.has_doc_indices = s.element_used_;
    for (const auto& h : hits) {
        out.row_ids.push_back(h.row);
        out.scores.push_back(h.score);
        if (out.has_doc_indices) {
            out.doc_indices.push_back(h.element);
        }
    }
    (void)version;
    return true;
}

bool index_build::load_inverted_params(const std::filesystem::path& dir, fts::AnalyzerParams& out,
                                       std::string& error) {
    bool found = false;
    const std::string text = metadata_value(dir / "metadata.lance", "params", found, error);
    if (!error.empty()) {
        return false;
    }
    out = fts::AnalyzerParams{};
    if (!found || !fts::parse_params(text, out, error)) {
        return !found;
    }
    // The format is metadata.lance's, not params': an index rebuilt from these settings keeps it.
    const std::string format = metadata_value(dir / "metadata.lance", "format_version", found, error);
    if (!error.empty()) {
        return false;
    }
    out.format_version = format == "1" ? 1U : format == "3" ? 3U : format == "2" ? 2U : 0U;
    return true;
}

bool index_build::load_inverted_rows(const std::filesystem::path& dir, std::vector<std::uint64_t>& out,
                                     std::string& error) {
    out.clear();
    bool found = false;
    const std::string text = metadata_value(dir / "metadata.lance", "partitions", found, error);
    fts::json::Value parts;
    if (!error.empty()) {
        return false;
    }
    if (!found || !fts::json::parse(text, parts) || parts.kind != fts::json::Value::Array) {
        error = "INVERTED index metadata lists no partitions";
        return false;
    }
    for (const auto& p : parts.items) {
        if (p.kind != fts::json::Value::Number || p.n < 0) {
            error = "INVERTED index metadata: malformed partition list";
            return false;
        }
        FileTable table;
        const std::vector<std::string> columns = {"_rowid"};
        const auto file = dir / ("part_" + std::to_string(static_cast<std::uint64_t>(p.n)) + "_docs.lance");
        if (!index_files::read_table(file, &columns, LanceRowRange{}, table, error)) {
            return false;
        }
        for (const auto& batch : table.batches) {
            ArrowArrayView view{};
            ArrowError e{};
            if (ArrowArrayViewInitFromSchema(&view, &table.schema, &e) != NANOARROW_OK ||
                ArrowArrayViewSetArray(&view, &batch, &e) != NANOARROW_OK || view.n_children < 1) {
                ArrowArrayViewReset(&view);
                error = file.filename().string() + ": unexpected columns";
                return false;
            }
            for (std::int64_t r = 0; r < batch.length; ++r) {
                out.push_back(ArrowArrayViewGetUIntUnsafe(view.children[0], view.offset + r));
            }
            ArrowArrayViewReset(&view);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return true;
}

}  // namespace nano_lance
