// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Building an INVERTED (full-text) index in Lance 12's format (docs/FTS_INDEX.md): the column's
// documents are tokenized as the index's analyzer does, token ids given in order of first
// appearance, and the four files and manifest entry written as Lance's InvertedIndexBuilder writes
// them, one partition holding every document.

#include "index_build.hpp"
#include "nanolance/fts_search.hpp"

#include "fts_document_text.hpp"
#include "fts_fst.hpp"
#include "fts_json.hpp"
#include "fts_posting.hpp"
#include "index_files.hpp"
#include "nanolance/dataset_commit.hpp"
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/manifest_reader.hpp"
#include "nanolance/parallel.hpp"
#include "nanolance/row_ids.hpp"

#include "lance_minimal.pb.hpp"

#include <bit>
#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>
#include <unordered_map>

namespace nano_lance {
namespace {

using index_files::OwnedArray;
using index_files::OwnedSchema;
using index_files::WrittenFile;

constexpr float kK1 = 1.2F;
constexpr float kB = 0.75F;
constexpr std::size_t kLevel1Blocks = 32;  // Lance's IMPACT_LEVEL1_BLOCKS

float idf(std::uint64_t token_docs, std::uint64_t num_docs) {
    const auto n = static_cast<float>(num_docs);
    const auto t = static_cast<float>(token_docs);
    return std::log((n - t + 0.5F) / (t + 0.5F) + 1.0F);
}

// ── protobuf ────────────────────────────────────────────────────────────────────────────────────

void put_key(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint32_t wire) {
    fts::put_varint(out, (static_cast<std::uint64_t>(field) << 3U) | wire);
}

void put_uint(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint64_t v) {
    put_key(out, field, 0);
    fts::put_varint(out, v);
}

void put_string(std::vector<std::uint8_t>& out, std::uint32_t field, const std::string& s) {
    put_key(out, field, 2);
    fts::put_varint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

/// InvertedIndexDetails, as Lance 12 fills it in (the language is kept as a JSON string).
/// The posting tail codec of format `format_version` (metadata.lance's posting_tail_codec).
std::string posting_tail_codec(std::uint32_t format_version) {
    return format_version == 1U ? "fixed32_v1" : "varint_delta_v1";
}

std::vector<std::uint8_t> index_details(const fts::AnalyzerParams& p, DocGranularity granularity) {
    std::vector<std::uint8_t> out;
    put_string(out, 1, p.base_tokenizer);
    put_string(out, 2, fts::json::quote(p.language));
    if (p.with_position) {
        put_uint(out, 3, 1);
    }
    if (p.max_token_length) {
        put_uint(out, 4, *p.max_token_length);
    }
    const auto flag = [&](std::uint32_t field, bool v) {
        if (v) {
            put_uint(out, field, 1);
        }
    };
    flag(5, p.lower_case);
    flag(6, p.stem);
    flag(7, p.remove_stop_words);
    flag(8, p.ascii_folding);
    if (p.min_ngram_length != 0U) {
        put_uint(out, 9, p.min_ngram_length);
    }
    if (p.max_ngram_length != 0U) {
        put_uint(out, 10, p.max_ngram_length);
    }
    flag(11, p.prefix_only);
    put_uint(out, 12, p.block_size);
    if (p.base_tokenizer == "code") {
        // code_config (CodeTokenizerConfig, index_old.proto). The two optional flags are written even when
        // false, as an explicit index-time choice; proto3 leaves out the other false ones.
        std::vector<std::uint8_t> code;
        if (p.split_identifiers) {
            put_uint(code, 1, 1);
        }
        put_uint(code, 2, p.split_on_numerics ? 1U : 0U);
        put_uint(code, 3, p.preserve_original ? 1U : 0U);
        if (p.index_operators) {
            put_uint(code, 4, 1);
        }
        put_string(out, 13, std::string(code.begin(), code.end()));
    }
    put_uint(out, 15, fts::resolved_format_version(p));  // posting_format_version
    if (granularity == DocGranularity::ListElement) {
        put_uint(out, 14, 1);  // document_granularity: LIST_ELEMENT (ROW is the default, left out)
    }
    return out;
}

// ── impacts (lance-index impact.rs) ─────────────────────────────────────────────────────────────

struct Impact {
    std::uint32_t doc;
    std::uint32_t freq;
    std::uint32_t doc_len;
};

std::uint8_t quantize_doc_length(std::uint32_t value) {
    const std::uint32_t bits = fts::bit_width(value);
    if (bits < 4U) {
        return static_cast<std::uint8_t>(value);
    }
    const std::uint32_t shift = bits - 4U;
    return static_cast<std::uint8_t>(((value >> shift) & 0x07U) | ((shift + 1U) << 3U));
}

/// One impact entry: the last document, then the (frequency, quantized length) frontier -- for
/// each frequency the shortest document, keeping only pairs no other pair dominates.
void encode_impact_entry(const std::vector<Impact>& docs, std::vector<std::pair<std::uint32_t, std::uint32_t>>& scratch,
                         std::vector<std::uint8_t>& out) {
    out.clear();
    auto& min_lens = scratch;
    min_lens.clear();
    for (const auto& d : docs) {
        min_lens.emplace_back(d.freq, d.doc_len);
    }
    std::sort(min_lens.begin(), min_lens.end());
    std::size_t w = 0;
    for (std::size_t r = 0; r < min_lens.size(); ++r) {
        if (w > 0 && min_lens[w - 1].first == min_lens[r].first) {
            continue;  // sorted: the first of a frequency is its shortest document
        }
        min_lens[w++] = min_lens[r];
    }
    min_lens.resize(w);
    std::vector<std::pair<std::uint32_t, std::uint8_t>> frontier;
    std::uint32_t best = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t i = min_lens.size(); i-- > 0;) {
        if (min_lens[i].second < best) {
            frontier.emplace_back(min_lens[i].first, 0);
            frontier.back().second = quantize_doc_length(min_lens[i].second);
            best = min_lens[i].second;
        }
    }
    std::reverse(frontier.begin(), frontier.end());
    std::vector<std::pair<std::uint32_t, std::uint8_t>> quantized;
    for (const auto& [freq, norm] : frontier) {
        if (!quantized.empty() && quantized.back().second == norm) {
            quantized.back().first = freq;
        } else {
            quantized.emplace_back(freq, norm);
        }
    }
    fts::put_varint(out, docs.back().doc);
    fts::put_varint(out, quantized.size());
    std::uint32_t prev_freq = 0;
    std::uint8_t prev_norm = 0;
    for (const auto& [freq, norm] : quantized) {
        const auto norm_delta = static_cast<std::uint8_t>(norm - prev_norm);
        const bool explicit_norm = norm_delta != 1U;
        fts::put_varint(out, (static_cast<std::uint64_t>(freq - prev_freq - 1U) << 1U) | (explicit_norm ? 1U : 0U));
        if (explicit_norm) {
            out.push_back(norm_delta);
        }
        prev_freq = freq;
        prev_norm = norm;
    }
}

// ── Arrow ───────────────────────────────────────────────────────────────────────────────────────

bool field(ArrowSchema* s, const char* name, ArrowType type, bool nullable) {
    if (ArrowSchemaInitFromType(s, type) != NANOARROW_OK || ArrowSchemaSetName(s, name) != NANOARROW_OK) {
        return false;
    }
    if (!nullable) {
        s->flags &= ~ARROW_FLAG_NULLABLE;
    }
    return true;
}

bool list_field(ArrowSchema* s, const char* name) {
    if (ArrowSchemaInitFromType(s, NANOARROW_TYPE_LIST) != NANOARROW_OK || ArrowSchemaSetName(s, name) != NANOARROW_OK ||
        ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_LARGE_BINARY) != NANOARROW_OK ||
        ArrowSchemaSetName(s->children[0], "item") != NANOARROW_OK) {
        return false;
    }
    s->flags &= ~ARROW_FLAG_NULLABLE;
    return true;
}

/// `_position`: list<list<large_binary>>, a posting's documents, each a list of compressed blocks.
bool legacy_position_field(ArrowSchema* s) {
    if (ArrowSchemaInitFromType(s, NANOARROW_TYPE_LIST) != NANOARROW_OK ||
        ArrowSchemaSetName(s, "_position") != NANOARROW_OK ||
        ArrowSchemaInitFromType(s->children[0], NANOARROW_TYPE_LIST) != NANOARROW_OK ||
        ArrowSchemaSetName(s->children[0], "item") != NANOARROW_OK ||
        ArrowSchemaSetType(s->children[0]->children[0], NANOARROW_TYPE_LARGE_BINARY) != NANOARROW_OK ||
        ArrowSchemaSetName(s->children[0]->children[0], "item") != NANOARROW_OK) {
        return false;
    }
    s->flags &= ~ARROW_FLAG_NULLABLE;
    return true;
}

bool struct_schema(ArrowSchema& out, std::size_t n) {
    ArrowSchemaInit(&out);
    return ArrowSchemaSetTypeStruct(&out, static_cast<std::int64_t>(n)) == NANOARROW_OK;
}

bool start(const ArrowSchema& schema, ArrowArray& out) {
    return ArrowArrayInitFromSchema(&out, &schema, nullptr) == NANOARROW_OK &&
           ArrowArrayStartAppending(&out) == NANOARROW_OK;
}

bool finish(ArrowArray& batch, std::int64_t rows, std::string& error) {
    batch.length = rows;  // every child holds `rows` values; no nulls
    batch.null_count = 0;
    ArrowError e{};
    if (ArrowArrayFinishBuildingDefault(&batch, &e) != NANOARROW_OK) {
        error = std::string("cannot assemble a batch: ") + e.message;
        return false;
    }
    return true;
}

bool append_bytes(ArrowArray* a, const std::uint8_t* data, std::size_t size) {
    ArrowBufferView v;
    v.data.as_uint8 = data;
    v.size_bytes = static_cast<std::int64_t>(size);
    return ArrowArrayAppendBytes(a, v) == NANOARROW_OK;
}

LanceFileExtras bitpacked() {
    LanceFileExtras extras;
    extras.bitpack_integers = true;
    return extras;
}

// ── the index ───────────────────────────────────────────────────────────────────────────────────

struct Built {
    std::vector<std::uint64_t> row_ids;
    std::vector<std::uint32_t> num_tokens;
    std::uint64_t total_tokens = 0;
    std::vector<std::string> tokens;  // by id
    std::unordered_map<std::string, std::uint32_t> ids;
    std::vector<std::vector<std::uint32_t>> docs;   // by token id
    std::vector<std::vector<std::uint32_t>> freqs;  // by token id
    /// with_position: by token id, every posting's positions in order (freqs[t][k] for posting k).
    bool with_positions = false;
    std::vector<std::vector<std::uint32_t>> positions;
    std::uint32_t format_version = 2;  // 1: fixed32_v1 tails and the legacy per-document _position
    std::size_t block_size = 128;      // posting blocks: 128 (BitPacker4x) or 256 (BitPacker8x)
    /// ListElement: one document per list element (`doc_index` is its position in the list), null and
    /// empty elements included; Row: one per row (no `doc_index`).
    bool elements = false;
    std::vector<std::uint32_t> doc_index;
};

/// One document: its tokens (already positioned) given their row address and, for a list-element
/// index, the element's position. Ids and postings follow in document order.
void append_document(Built& b, std::vector<fts::Token>& tokens, std::uint64_t row_id, std::uint32_t element,
                     std::vector<std::pair<std::uint32_t, std::uint32_t>>& counts) {
    const auto doc = static_cast<std::uint32_t>(b.row_ids.size());
    b.row_ids.push_back(row_id);
    b.num_tokens.push_back(static_cast<std::uint32_t>(tokens.size()));
    b.total_tokens += tokens.size();
    if (b.elements) {
        b.doc_index.push_back(element);
    }
    counts.clear();
    for (auto& t : tokens) {
        auto [it, inserted] = b.ids.emplace(t.text, static_cast<std::uint32_t>(b.tokens.size()));
        if (inserted) {
            b.tokens.push_back(std::move(t.text));
            b.docs.emplace_back();
            b.freqs.emplace_back();
            if (b.with_positions) {
                b.positions.emplace_back();
            }
        }
        counts.emplace_back(it->second, b.with_positions ? t.position : 0U);
    }
    std::sort(counts.begin(), counts.end());
    for (std::size_t i = 0; i < counts.size();) {
        std::size_t j = i;
        while (j < counts.size() && counts[j].first == counts[i].first) {
            ++j;
        }
        b.docs[counts[i].first].push_back(doc);
        b.freqs[counts[i].first].push_back(static_cast<std::uint32_t>(j - i));
        if (b.with_positions) {
            auto& p = b.positions[counts[i].first];
            for (std::size_t k = i; k < j; ++k) {
                p.push_back(counts[k].second);
            }
        }
        i = j;
    }
}

/// A list-element batch's documents: one per element of each non-null row, in row order then element
/// order. Each element is tokenized alone, so its positions start at 0 (Lance's element documents).
bool add_element_documents(const ArrowArrayView* text, const ArrowArrayView* addr, const ArrowArrayView& view,
                           std::int64_t length, const fts::Analyzer& analyzer, const AddressToRowId& to_row_id,
                           Built& b, std::string& error) {
    struct Element {
        std::int64_t row;  // the row, as the column's accessors take it
        std::uint32_t k;   // the element's position in its list
        std::string_view text;
        bool present;
    };
    std::vector<Element> elements;
    const ArrowArrayView* values = text->children[0];
    for (std::int64_t r = 0; r < length; ++r) {
        const std::int64_t row = view.offset + r;
        if (ArrowArrayViewIsNull(text, row)) {
            continue;  // a null row has no documents
        }
        const std::int64_t first = ArrowArrayViewListChildOffset(text, text->offset + row);
        const std::int64_t last = ArrowArrayViewListChildOffset(text, text->offset + row + 1);
        for (std::int64_t e = first; e < last; ++e) {
            Element el{row, static_cast<std::uint32_t>(e - first), {}, !ArrowArrayViewIsNull(values, e)};
            if (el.present) {
                const ArrowStringView sv = ArrowArrayViewGetStringUnsafe(values, e);
                el.text = std::string_view(sv.data, static_cast<std::size_t>(sv.size_bytes));
            }
            elements.push_back(el);
        }
    }
    std::vector<std::vector<fts::Token>> tokens(elements.size());
    const std::size_t chunk = 1024;
    parallel::for_each((elements.size() + chunk - 1) / chunk, [&](std::size_t c) {
        for (std::size_t i = c * chunk; i < std::min(elements.size(), (c + 1) * chunk); ++i) {
            if (elements[i].present && !elements[i].text.empty()) {
                analyzer.tokenize(elements[i].text, tokens[i]);
            }
        }
    });
    std::vector<std::pair<std::uint32_t, std::uint32_t>> counts;  // token id, position
    for (std::size_t i = 0; i < elements.size(); ++i) {
        if (b.row_ids.size() >= std::numeric_limits<std::uint32_t>::max()) {
            error = "more than 2^32 documents";
            return false;
        }
        const std::uint64_t row_id = to_row_id(ArrowArrayViewGetUIntUnsafe(addr, elements[i].row));
        append_document(b, tokens[i], row_id, elements[i].k, counts);
    }
    return true;
}

/// The documents of `batches` (column 0 text, column 1 row address), added in order.
bool add_documents(const ArrowSchema& schema, std::vector<ArrowArray>& batches, const fts::Analyzer& analyzer,
                   const AddressToRowId& to_row_id, Built& b, std::string& error) {
    for (auto& batch : batches) {
        ArrowArrayView view{};
        ArrowError e{};
        if (ArrowArrayViewInitFromSchema(&view, &schema, &e) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &e) != NANOARROW_OK) {
            ArrowArrayViewReset(&view);
            error = std::string("cannot read a batch: ") + e.message;
            return false;
        }
        const ArrowArrayView* text = view.children[0];
        const ArrowArrayView* addr = view.children[1];
        if (b.elements) {
            const bool ok = add_element_documents(text, addr, view, batch.length, analyzer, to_row_id, b, error);
            ArrowArrayViewReset(&view);
            if (!ok) {
                return false;
            }
            continue;
        }
        const fts::TextColumn kind = fts::text_column_kind(text);
        const auto rows = static_cast<std::size_t>(batch.length);
        // Tokenize side by side; then give ids and postings in row order.
        std::vector<std::vector<fts::Token>> tokens(rows);
        const std::size_t chunk = 1024;
        parallel::for_each((rows + chunk - 1) / chunk, [&](std::size_t c) {
            std::string joined;  // a list row's text, joined as Lance joins it
            for (std::size_t r = c * chunk; r < std::min(rows, (c + 1) * chunk); ++r) {
                const std::int64_t row = view.offset + static_cast<std::int64_t>(r);
                std::string_view document;
                if (fts::row_text(text, kind, row, joined, document)) {
                    analyzer.tokenize(document, tokens[r]);
                }
            }
        });
        std::vector<std::pair<std::uint32_t, std::uint32_t>> counts;  // token id, position
        for (std::size_t r = 0; r < rows; ++r) {
            if (tokens[r].empty()) {
                continue;
            }
            if (b.row_ids.size() >= std::numeric_limits<std::uint32_t>::max()) {
                ArrowArrayViewReset(&view);
                error = "more than 2^32 documents";
                return false;
            }
            append_document(b, tokens[r], to_row_id(ArrowArrayViewGetUIntUnsafe(addr, view.offset + static_cast<std::int64_t>(r))),
                            0U, counts);
        }
        ArrowArrayViewReset(&view);
    }
    return true;
}

bool write_tokens(const std::filesystem::path& dir, const Built& b, std::vector<WrittenFile>& files,
                  std::string& error) {
    std::vector<std::uint32_t> order(b.tokens.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t x, std::uint32_t y) { return b.tokens[x] < b.tokens[y]; });
    fts::FstBuilder fst;
    std::uint64_t total_length = 0;
    for (const auto id : order) {
        if (!fst.insert(b.tokens[id], id, error)) {
            return false;
        }
        total_length += b.tokens[id].size();
    }
    const auto bytes = fst.finish();
    OwnedSchema schema;
    OwnedArray batch;
    const auto next_id = static_cast<std::uint32_t>(b.tokens.size());
    if (!struct_schema(schema.s, 3) || !field(schema.s.children[0], "_token_fst_bytes", NANOARROW_TYPE_LARGE_BINARY, false) ||
        !field(schema.s.children[1], "_token_next_id", NANOARROW_TYPE_UINT32, false) ||
        !field(schema.s.children[2], "_token_total_length", NANOARROW_TYPE_UINT64, false) || !start(schema.s, batch.a) ||
        !append_bytes(batch.a.children[0], bytes.data(), bytes.size()) ||
        ArrowArrayAppendUInt(batch.a.children[1], next_id) != NANOARROW_OK ||
        ArrowArrayAppendUInt(batch.a.children[2], total_length) != NANOARROW_OK || !finish(batch.a, 1, error)) {
        if (error.empty()) {
            error = "out of memory";
        }
        return false;
    }
    return index_files::write_file(dir, "part_0_tokens.lance", schema.s, batch.a, bitpacked(), files, error);
}

bool write_docs(const std::filesystem::path& dir, const Built& b, std::vector<WrittenFile>& files, std::string& error) {
    OwnedSchema schema;
    OwnedArray batch;
    // Lance's v3 layout for list elements adds the element's position in its list.
    const std::int64_t columns = b.elements ? 3 : 2;
    if (!struct_schema(schema.s, columns) || !field(schema.s.children[0], "_rowid", NANOARROW_TYPE_UINT64, false) ||
        !field(schema.s.children[1], "_num_tokens", NANOARROW_TYPE_UINT32, false) ||
        (b.elements && !field(schema.s.children[2], "_doc_index_0", NANOARROW_TYPE_UINT32, false)) ||
        !start(schema.s, batch.a)) {
        error = "out of memory";
        return false;
    }
    for (std::size_t d = 0; d < b.row_ids.size(); ++d) {
        if (ArrowArrayAppendUInt(batch.a.children[0], b.row_ids[d]) != NANOARROW_OK ||
            ArrowArrayAppendUInt(batch.a.children[1], b.num_tokens[d]) != NANOARROW_OK ||
            (b.elements && ArrowArrayAppendUInt(batch.a.children[2], b.doc_index[d]) != NANOARROW_OK)) {
            error = "out of memory";
            return false;
        }
    }
    if (!finish(batch.a, static_cast<std::int64_t>(b.row_ids.size()), error)) {
        return false;
    }
    LanceFileExtras extras;
    extras.bitpack_integers = true;
    extras.schema_metadata["total_tokens"] = index_files::bytes_of(std::to_string(b.total_tokens));
    return index_files::write_file(dir, "part_0_docs.lance", schema.s, batch.a, extras, files, error);
}

/// The posting lists, by token id: blocks of 128 with Lance's block scores, the best of them, the
/// length and the impact skip data (an entry per block, then one per 32 blocks).
/// One posting block's positions as Lance's PackedDelta stream (encoding.rs): each document's
/// positions as deltas (the first absolute), 128 at a time bit-packed after a bit-width byte, the
/// rest as varints.
void encode_position_block(const std::uint32_t* positions, const std::uint32_t* freqs, std::size_t docs,
                           std::vector<std::uint8_t>& out) {
    std::uint32_t pending[fts::kPostingBlock];
    std::size_t count = 0;
    std::uint8_t packed[fts::kPostingBlock * 4];
    const auto push = [&](std::uint32_t delta) {
        pending[count++] = delta;
        if (count == fts::kPostingBlock) {
            std::uint32_t any = 0;
            for (const auto v : pending) {
                any |= v;
            }
            const auto bits = static_cast<std::uint8_t>(std::bit_width(any));
            fts::bitpack4x_pack(pending, bits, packed);
            out.push_back(bits);
            out.insert(out.end(), packed, packed + static_cast<std::size_t>(bits) * fts::kPostingBlock / 8U);
            count = 0;
        }
    };
    std::size_t at = 0;
    for (std::size_t d = 0; d < docs; ++d) {
        for (std::uint32_t i = 0; i < freqs[d]; ++i, ++at) {
            push(i == 0U ? positions[at] : positions[at] - positions[at - 1U]);
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        fts::put_varint(out, pending[i]);
    }
}

/// One document's positions as lance-index's compress_positions writes them, one element each: the
/// count (u32), a sorted block per full 128 positions ([u32 first][u8 bits][packed deltas]), then the
/// remainder's plain u32s.
void legacy_position_elements(const std::uint32_t* positions, std::size_t n,
                              std::vector<std::vector<std::uint8_t>>& out) {
    out.clear();
    std::vector<std::uint8_t> head(4);
    const auto count = static_cast<std::uint32_t>(n);
    std::memcpy(head.data(), &count, 4);
    out.push_back(std::move(head));
    std::size_t at = 0;
    for (; at + fts::kPostingBlock <= n; at += fts::kPostingBlock) {
        std::uint32_t deltas[fts::kPostingBlock];
        std::uint32_t any = 0;
        for (std::size_t i = 0; i < fts::kPostingBlock; ++i) {
            deltas[i] = positions[at + i] - (i == 0 ? positions[at] : positions[at + i - 1]);
            any |= deltas[i];
        }
        const std::uint8_t bits = fts::bit_width(any);
        std::vector<std::uint8_t> block(5 + 16U * bits);
        std::memcpy(block.data(), &positions[at], 4);
        block[4] = bits;
        fts::bitpack4x_pack(deltas, bits, block.data() + 5);
        out.push_back(std::move(block));
    }
    if (at < n) {
        std::vector<std::uint8_t> rest(4 * (n - at));
        std::memcpy(rest.data(), positions + at, rest.size());
        out.push_back(std::move(rest));
    }
}

bool write_postings(const std::filesystem::path& dir, const Built& b, std::vector<WrittenFile>& files,
                    std::string& error) {
    OwnedSchema schema;
    OwnedArray batch;
    const bool with_positions = b.with_positions;
    const bool legacy = with_positions && b.format_version == 1U;  // one _position per posting
    if (!struct_schema(schema.s, with_positions ? (legacy ? 5 : 6) : 4) ||
        !list_field(schema.s.children[0], "_posting") ||
        !field(schema.s.children[1], "_max_score", NANOARROW_TYPE_FLOAT, false) ||
        !field(schema.s.children[2], "_length", NANOARROW_TYPE_UINT32, false) ||
        !list_field(schema.s.children[3], "_impacts") ||
        (with_positions && !legacy &&
         (!field(schema.s.children[4], "_compressed_position", NANOARROW_TYPE_LARGE_BINARY, false) ||
          ArrowSchemaInitFromType(schema.s.children[5], NANOARROW_TYPE_LIST) != NANOARROW_OK ||
          ArrowSchemaSetName(schema.s.children[5], "_position_block_offset") != NANOARROW_OK ||
          ArrowSchemaSetType(schema.s.children[5]->children[0], NANOARROW_TYPE_UINT32) != NANOARROW_OK)) ||
        (legacy && !legacy_position_field(schema.s.children[4])) || !start(schema.s, batch.a)) {
        error = "out of memory";
        return false;
    }
    if (with_positions && !legacy) {
        schema.s.children[5]->flags &= ~ARROW_FLAG_NULLABLE;
    }
    std::vector<std::uint8_t> stream;
    const std::size_t n_docs = b.row_ids.size();
    const float avgdl = static_cast<float>(b.total_tokens) / static_cast<float>(n_docs);
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> entry;
    std::vector<Impact> impacts;
    std::vector<Impact> level1;
    std::vector<std::vector<std::uint8_t>> level1_entries;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> scratch;
    ArrowArray* postings = batch.a.children[0];
    ArrowArray* impact_list = batch.a.children[3];
    bool ok = true;
    for (std::size_t t = 0; ok && t < b.tokens.size(); ++t) {
        const auto& docs = b.docs[t];
        const auto& freqs = b.freqs[t];
        const std::size_t length = docs.size();
        const float idf_scale = idf(length, n_docs) * (kK1 + 1.0F);
        float max_score = std::numeric_limits<float>::lowest();
        level1.clear();
        level1_entries.clear();
        std::size_t blocks = 0;
        const std::size_t block = b.block_size;
        for (std::size_t at = 0; ok && at < length; at += block) {
            const std::size_t n = std::min(block, length - at);
            float best = std::numeric_limits<float>::lowest();
            impacts.clear();
            for (std::size_t i = at; i < at + n; ++i) {
                const std::uint32_t dl = b.num_tokens[docs[i]];
                const float norm = kK1 * (1.0F - kB + kB * static_cast<float>(dl) / avgdl);
                const auto f = static_cast<float>(freqs[i]);
                best = std::max(best, f / (f + norm));
                impacts.push_back({docs[i], freqs[i], dl});
            }
            const float score = best * idf_scale;
            max_score = std::max(max_score, score);
            bytes.clear();
            fts::encode_posting_block(docs.data() + at, freqs.data() + at, n, score, bytes,
                                      b.format_version == 1U ? fts::TailCodec::Fixed32 : fts::TailCodec::VarintDelta,
                                      block);
            ok = append_bytes(postings->children[0], bytes.data(), bytes.size());
            encode_impact_entry(impacts, scratch, entry);
            ok = ok && append_bytes(impact_list->children[0], entry.data(), entry.size());
            level1.insert(level1.end(), impacts.begin(), impacts.end());
            if (++blocks % kLevel1Blocks == 0U) {
                encode_impact_entry(level1, scratch, entry);
                level1_entries.push_back(entry);
                level1.clear();
            }
        }
        if (!level1.empty()) {
            encode_impact_entry(level1, scratch, entry);
            level1_entries.push_back(entry);
        }
        for (const auto& e : level1_entries) {
            ok = ok && append_bytes(impact_list->children[0], e.data(), e.size());
        }
        ok = ok && ArrowArrayFinishElement(postings) == NANOARROW_OK &&
             ArrowArrayAppendDouble(batch.a.children[1], max_score) == NANOARROW_OK &&
             ArrowArrayAppendUInt(batch.a.children[2], length) == NANOARROW_OK &&
             ArrowArrayFinishElement(impact_list) == NANOARROW_OK;
        if (ok && with_positions && !legacy) {
            // Every block's positions, one stream after another, and where each starts.
            stream.clear();
            ArrowArray* offsets = batch.a.children[5];
            std::size_t first = 0;
            for (std::size_t at = 0; ok && at < length; at += b.block_size) {
                const std::size_t n = std::min(b.block_size, length - at);
                ok = ArrowArrayAppendUInt(offsets->children[0], stream.size()) == NANOARROW_OK;
                encode_position_block(b.positions[t].data() + first, freqs.data() + at, n, stream);
                for (std::size_t i = at; i < at + n; ++i) {
                    first += freqs[i];
                }
            }
            ok = ok && append_bytes(batch.a.children[4], stream.data(), stream.size()) &&
                 ArrowArrayFinishElement(offsets) == NANOARROW_OK;
        }
        if (ok && legacy) {
            // One list per document of this posting, in posting order.
            ArrowArray* per_doc = batch.a.children[4];
            std::vector<std::vector<std::uint8_t>> elements;
            std::size_t first = 0;
            for (std::size_t k = 0; ok && k < length; ++k) {
                legacy_position_elements(b.positions[t].data() + first, freqs[k], elements);
                for (const auto& e : elements) {
                    ok = ok && append_bytes(per_doc->children[0]->children[0], e.data(), e.size());
                }
                ok = ok && ArrowArrayFinishElement(per_doc->children[0]) == NANOARROW_OK;
                first += freqs[k];
            }
            ok = ok && ArrowArrayFinishElement(per_doc) == NANOARROW_OK;
        }
    }
    if (!ok || !finish(batch.a, static_cast<std::int64_t>(b.tokens.size()), error)) {
        if (error.empty()) {
            error = "out of memory";
        }
        return false;
    }
    LanceFileExtras extras;
    extras.bitpack_integers = true;
    extras.schema_metadata["format_version"] = index_files::bytes_of(std::to_string(b.format_version));
    extras.schema_metadata["posting_block_size"] = index_files::bytes_of(std::to_string(b.block_size));
    if (b.format_version >= 2U) {  // format 1's invert.lance has no posting_tail_codec
        extras.schema_metadata["posting_tail_codec"] = index_files::bytes_of(posting_tail_codec(b.format_version));
    }
    if (with_positions && !legacy) {
        extras.schema_metadata["positions_layout"] = index_files::bytes_of("shared_stream_v2");
        extras.schema_metadata["positions_codec"] = index_files::bytes_of("packed_delta_v1");
    }
    return index_files::write_file(dir, "part_0_invert.lance", schema.s, batch.a, extras, files, error);
}

bool write_metadata(const std::filesystem::path& dir, const fts::AnalyzerParams& params,
                    std::vector<WrittenFile>& files, std::string& error) {
    // deleted_fragments: an empty Roaring bitmap (portable format, no runs).
    static const std::uint8_t kEmptyBitmap[] = {0x3A, 0x30, 0, 0, 0, 0, 0, 0};
    OwnedSchema schema;
    OwnedArray batch;
    if (!struct_schema(schema.s, 1) || !field(schema.s.children[0], "deleted_fragments", NANOARROW_TYPE_BINARY, false) ||
        !start(schema.s, batch.a) || !append_bytes(batch.a.children[0], kEmptyBitmap, sizeof(kEmptyBitmap)) ||
        !finish(batch.a, 1, error)) {
        if (error.empty()) {
            error = "out of memory";
        }
        return false;
    }
    LanceFileExtras extras;
    extras.bitpack_integers = true;
    extras.schema_metadata["partitions"] = index_files::bytes_of("[0]");
    extras.schema_metadata["token_set_format"] = index_files::bytes_of("fst");
    extras.schema_metadata["params"] = index_files::bytes_of(fts::params_json(params));
    const std::uint32_t format = fts::resolved_format_version(params);
    extras.schema_metadata["format_version"] = index_files::bytes_of(std::to_string(format));
    extras.schema_metadata["posting_tail_codec"] = index_files::bytes_of(posting_tail_codec(format));
    extras.schema_metadata["posting_block_size"] = index_files::bytes_of(std::to_string(params.block_size));
    if (params.with_position && format >= 2) {  // format 1 keeps its positions in the legacy _position
        extras.schema_metadata["positions_layout"] = index_files::bytes_of("shared_stream_v2");
        extras.schema_metadata["positions_codec"] = index_files::bytes_of("packed_delta_v1");
    }
    return index_files::write_file(dir, "metadata.lance", schema.s, batch.a, extras, files, error);
}

}  // namespace

namespace {

bool create_inverted_index(const std::filesystem::path& dataset_path, const std::string& column,
                           const InvertedIndexOptions& options, const index_build::SegmentTarget* target,
                           std::uint64_t& new_version, std::string& error) {
    error.clear();
    fts::AnalyzerParams params = options.params;

    // Lance's rules (format.rs, tokenizer.rs): the block sizes, the format versions, their pairs.
    if (params.block_size != 128U && params.block_size != 256U) {
        error = "FTS inverted index block_size must be one of 128 or 256, got " + std::to_string(params.block_size);
        return false;
    }
    const std::uint32_t format = fts::resolved_format_version(params);
    if (format < 1U || format > 3U) {
        error = "unsupported FTS format version " + std::to_string(format) + ", expected 1, 2, or 3";
        return false;
    }
    if (format < 3U && params.block_size == 256U) {
        error = "FTS format_version=" + std::to_string(format) +
                " is incompatible with block_size=256; use format_version=3";
        return false;
    }
    params.format_version = format;
    fts::Analyzer analyzer;
    if (!analyzer.init(params, error)) {
        return false;
    }
    pb::Manifest manifest;
    std::uint64_t version = 0;
    if (target != nullptr && target->manifest != nullptr) {
        manifest = *target->manifest;
        version = target->version;
    } else if (!load_latest_manifest(dataset_path, manifest, version, error)) {
        return false;
    }
    const bool commit = target == nullptr || target->out == nullptr;
    AddressToRowId to_row_id;  // stable row ids go into the index, row addresses otherwise
    if (!AddressToRowId::build(manifest, to_row_id, error)) {
        return false;
    }
    std::vector<std::string> parts;
    const auto* f = index_files::find_field(manifest, column, parts);
    if (f == nullptr) {
        error = column.find('[') != std::string::npos
                    ? "FTS field path '" + column + "' does not exist in the dataset schema"  // a list element path
                    : "column '" + column + "' not found";
        return false;
    }
    if (parts.size() != 1U) {
        error = "an INVERTED index on a nested column is not supported";
        return false;
    }
    const bool elements = options.granularity == DocGranularity::ListElement;
    // Lance's default names: the row index is <column>_idx, the list-element one <column>_list_element_idx.
    const std::string name = options.name.empty() ? column + (elements ? "_list_element_idx" : "_idx") : options.name;
    const auto existing = std::find_if(manifest.indices.begin(), manifest.indices.end(),
                                       [&](const pb::IndexMetadata& i) { return i.name == name; });
    if (commit && existing != manifest.indices.end() && !options.replace) {
        error = "Index name '" + name + "' already exists, please specify a different name or use replace=True";
        return false;
    }

    // The documents.
    LanceScanRequest request;
    request.has_version = true;
    request.version = version;
    const std::vector<std::string> columns = {column};
    request.columns = &columns;
    request.with_row_address = true;
    request.fragment_ids = target != nullptr ? target->fragments : nullptr;
    const auto is_text = [&](const ArrowSchema& schema) {
        const ArrowSchema* text = schema.n_children == 2 ? schema.children[0] : nullptr;
        if (fts::text_schema_kind(text) == fts::TextColumn::kNone) {
            error = "an INVERTED index needs a string column or a list of strings column; " + column + " is not one";
            return false;
        }
        if (elements && fts::text_schema_kind(text) != fts::TextColumn::kStringList) {
            error = "Invalid user input: FTS field path '" + column + "' has no List layer and cannot use ListElement document granularity";
            return false;
        }
        return true;
    };
    Built built;
    built.elements = elements;
    built.with_positions = params.with_position;
    built.format_version = format;
    built.block_size = params.block_size;
    const index_build::Progress* progress = target != nullptr ? target->progress : nullptr;
    if (progress != nullptr) {
        (*progress)(0, "tokenize_docs", 0, "rows", 0);
    }
    if (target != nullptr && target->rows != nullptr && !target->rows->empty()) {
        // An old segment's documents first, deleted or not.
        OwnedSchema taken;
        index_files::OwnedBatches rows;
        LanceScanRequest by_address = request;
        by_address.fragment_ids = nullptr;
        std::vector<std::uint64_t> addresses = *target->rows;
        if (to_row_id.stable()) {  // the old segment's rows are ids
            RowIdToAddress resolver;
            if (!RowIdToAddress::build(dataset_path, manifest, resolver, error)) {
                return false;
            }
            for (auto& row : addresses) {
                if (!resolver.find(row, row)) {
                    row = UINT64_MAX;
                }
            }
            addresses.erase(std::remove(addresses.begin(), addresses.end(), UINT64_MAX), addresses.end());
        }
        if (!lance_dataset_take_rows(dataset_path, by_address, addresses, taken.s, rows.v, error) ||
            !is_text(taken.s) || !add_documents(taken.s, rows.v, analyzer, to_row_id, built, error)) {
            return false;
        }
    }
    if (request.fragment_ids == nullptr || !request.fragment_ids->empty()) {
        OwnedSchema scanned;
        index_files::OwnedBatches batches;
        if (!lance_dataset_scan(dataset_path, request, scanned.s, batches.v, error) || !is_text(scanned.s) ||
            !add_documents(scanned.s, batches.v, analyzer, to_row_id, built, error)) {
            return false;
        }
    }

    if (progress != nullptr) {
        (*progress)(2, "tokenize_docs", 0, "", 0);
    }

    // The files.
    const auto uuid = target != nullptr && target->uuid != nullptr ? *target->uuid : index_files::new_uuid();
    const auto dir = dataset_path / "_indices" / pb::uuid_string(uuid);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        error = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }
    std::vector<WrittenFile> files;
    if (!index_build::stage(progress, "write_metadata", 4, "files", [&] {
            return write_postings(dir, built, files, error) && write_tokens(dir, built, files, error) &&
                   write_docs(dir, built, files, error) && write_metadata(dir, params, files, error);
        })) {
        std::filesystem::remove_all(dir, ec);
        return false;
    }

    // The manifest entry.
    std::vector<std::uint32_t> fragment_ids;
    if (target != nullptr && (target->coverage != nullptr || target->fragments != nullptr)) {
        for (const auto id : target->coverage != nullptr ? *target->coverage : *target->fragments) {
            fragment_ids.push_back(static_cast<std::uint32_t>(id));
        }
    } else {
        for (const auto& frag : manifest.fragments) {
            fragment_ids.push_back(static_cast<std::uint32_t>(frag.id));
        }
    }
    std::sort(fragment_ids.begin(), fragment_ids.end());
    std::vector<pb::IndexMetadata::File> index_files_list;
    for (const auto& file : files) {
        index_files_list.push_back({file.name, file.size});
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    auto entry = pb::make_index_metadata(uuid, {f->id}, name, version, fragment_ids,
                                                       "/lance.table.InvertedIndexDetails", format,
                                                       static_cast<std::uint64_t>(now), index_files_list,
                                                       index_details(params, options.granularity));
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
        std::filesystem::remove_all(dir, ec);
        return false;
    }
    return true;
}

}  // namespace

bool dataset_create_inverted_index(const std::filesystem::path& dataset_path, const std::string& column,
                                   const InvertedIndexOptions& options, std::uint64_t& new_version,
                                   std::string& error) {
    return create_inverted_index(dataset_path, column, options, nullptr, new_version, error);
}

bool index_build::build_inverted_segment(const std::filesystem::path& dataset_path, const std::string& column,
                                         const InvertedIndexOptions& options, const SegmentTarget& target,
                                         std::string& error) {
    std::uint64_t unused = 0;
    return create_inverted_index(dataset_path, column, options, &target, unused, error);
}

std::vector<std::uint8_t> index_build::inverted_details(const fts::AnalyzerParams& params, DocGranularity granularity) {
    return index_details(params, granularity);
}

}  // namespace nano_lance
