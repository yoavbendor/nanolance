// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/sparse_layout.hpp"

#include "nanolance/child_block.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace nano_lance::sparse {
namespace {

using page_layout::Compressive;
using page_layout::CompressiveKind;
using page_layout::SparseLayer;
using page_layout::SparseLayout;
using page_layout::SparsePositions;
using repdef::UnraveledLayer;

constexpr std::size_t kU64Bytes = 8U;

/// The number of bytes a block of `count` values of `width` bytes takes, refusing overflow.
bool byte_count(std::uint64_t count, std::size_t width, std::size_t& out) {
    if (count > std::numeric_limits<std::size_t>::max() / width) {
        return false;
    }
    out = static_cast<std::size_t>(count) * width;
    return true;
}

bool needs_buffer(const SparsePositions& p) { return p.kind == SparsePositions::Kind::kExplicit; }

/// Takes the next structural buffer for a set that stores one.
bool take_buffer(const std::vector<std::vector<std::uint8_t>>& buffers, std::size_t& next,
                 const std::vector<std::uint8_t>*& out, std::string& error) {
    if (next >= buffers.size()) {
        error = "sparse page has fewer structural buffers than its layers need";
        return false;
    }
    out = &buffers[next++];
    return true;
}

/// Materialises a position set over a domain of `domain` slots: its sorted positions. Explicit sets are
/// delta-coded in their buffer, the first position absolute and each later one a positive step.
bool materialize_positions(const SparsePositions& p, std::uint64_t domain, const std::vector<std::uint8_t>* buffer,
                           std::vector<std::uint64_t>& out, std::string& error) {
    out.clear();
    const std::uint64_t count = p.num_positions;
    if (count > domain) {
        error = "sparse position set has " + std::to_string(count) + " positions in a domain of " +
                std::to_string(domain);
        return false;
    }
    switch (p.kind) {
        case SparsePositions::Kind::kEmpty:
            if (count != 0U) {
                error = "sparse empty position set has cardinality " + std::to_string(count);
                return false;
            }
            return true;
        case SparsePositions::Kind::kAll:
            if (domain == 0U || count != domain) {
                error = "sparse all-positions set has cardinality " + std::to_string(count) + " in a domain of " +
                        std::to_string(domain);
                return false;
            }
            out.resize(static_cast<std::size_t>(domain));
            for (std::uint64_t i = 0; i < domain; ++i) {
                out[static_cast<std::size_t>(i)] = i;
            }
            return true;
        case SparsePositions::Kind::kRange:
            if (p.length == 0U || p.length != count || p.start > domain || p.length > domain - p.start) {
                error = "sparse range position set " + std::to_string(p.start) + "+" + std::to_string(p.length) +
                        " does not fit a domain of " + std::to_string(domain);
                return false;
            }
            out.resize(static_cast<std::size_t>(count));
            for (std::uint64_t i = 0; i < count; ++i) {
                out[static_cast<std::size_t>(i)] = p.start + i;
            }
            return true;
        case SparsePositions::Kind::kExplicit: {
            if (count == 0U || p.encoding == nullptr || buffer == nullptr) {
                error = "sparse explicit position set is missing its values";
                return false;
            }
            std::vector<std::uint64_t> deltas;
            if (!decode_u64(*p.encoding, *buffer, count, deltas, error)) {
                return false;
            }
            out.reserve(deltas.size());
            std::uint64_t current = 0;
            for (std::size_t i = 0; i < deltas.size(); ++i) {
                if (i == 0U) {
                    current = deltas[i];
                } else {
                    if (deltas[i] == 0U || deltas[i] > std::numeric_limits<std::uint64_t>::max() - current) {
                        error = "sparse explicit positions are not strictly increasing";
                        return false;
                    }
                    current += deltas[i];
                }
                if (current >= domain) {
                    error = "sparse position " + std::to_string(current) + " is outside a domain of " +
                            std::to_string(domain);
                    return false;
                }
                out.push_back(current);
            }
            return true;
        }
    }
    error = "sparse position set has no variant";
    return false;
}

/// The child counts of a list layer's non-empty slots, `k` of them.
bool materialize_counts(const page_layout::SparseCounts& c, std::uint64_t k, const std::vector<std::uint8_t>* buffer,
                        std::vector<std::uint64_t>& out, std::string& error) {
    out.clear();
    switch (c.kind) {
        case page_layout::SparseCounts::Kind::kEmpty:
            if (k != 0U) {
                error = "sparse list has " + std::to_string(k) + " non-empty slots but no counts";
                return false;
            }
            return true;
        case page_layout::SparseCounts::Kind::kConstant:
            if (k == 0U || c.value == 0U) {
                error = "sparse constant list count must be positive and shared by some slots";
                return false;
            }
            out.assign(static_cast<std::size_t>(k), c.value);
            return true;
        case page_layout::SparseCounts::Kind::kExplicit:
            if (k == 0U || c.encoding == nullptr || buffer == nullptr) {
                error = "sparse explicit list counts are missing their values";
                return false;
            }
            if (!decode_u64(*c.encoding, *buffer, k, out, error)) {
                return false;
            }
            for (const auto count : out) {
                if (count == 0U) {
                    error = "sparse list has a zero child count";
                    return false;
                }
            }
            return true;
    }
    error = "sparse count set has no variant";
    return false;
}

/// One layer's sets, decoded: a validity bitmap (LSB first, a set bit is VALID) over its parent domain,
/// and for a list its non-empty slots and their counts.
struct DecodedLayer {
    std::vector<std::uint8_t> valid;  // ceil(num_slots / 8) bytes
    std::vector<std::uint64_t> non_empty;
    std::vector<std::uint64_t> counts;
};

/// The validity bitmap of a validity set over `num_slots` slots.
bool materialize_validity(const page_layout::SparseValidity& v, std::uint64_t num_slots,
                          const std::vector<std::uint8_t>* buffer, std::vector<std::uint8_t>& valid,
                          std::string& error) {
    std::vector<std::uint64_t> positions;
    if (!materialize_positions(v.positions, num_slots, buffer, positions, error)) {
        return false;
    }
    std::size_t bytes = 0;
    if (!byte_count((num_slots + 7U) / 8U, 1, bytes)) {
        error = "sparse validity is too large";
        return false;
    }
    // A null-positions set means every other slot is valid; a valid-positions set means every other
    // slot is null.
    const bool fill = v.null_positions;
    valid.assign(bytes, fill ? std::uint8_t{0xFF} : std::uint8_t{0});
    for (const auto pos : positions) {
        const auto bit = static_cast<std::uint8_t>(1U << (pos & 7U));
        if (fill) {
            valid[static_cast<std::size_t>(pos >> 3U)] &= static_cast<std::uint8_t>(~bit);
        } else {
            valid[static_cast<std::size_t>(pos >> 3U)] |= bit;
        }
    }
    return true;
}

bool is_valid(const std::vector<std::uint8_t>& valid, std::uint64_t slot) {
    return (valid[static_cast<std::size_t>(slot >> 3U)] >> (slot & 7U)) & 1U;
}

/// Turns a validity bitmap over `length` entries into the unravelled form: empty when every entry is valid.
void finish_validity(const std::vector<std::uint8_t>& valid, std::uint64_t length, UnraveledLayer& out) {
    std::uint64_t nulls = 0;
    for (std::uint64_t i = 0; i < length; ++i) {
        nulls += is_valid(valid, i) ? 0U : 1U;
    }
    out.null_count = nulls;
    if (nulls == 0U) {
        out.validity.clear();
    } else {
        out.validity.assign(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>((length + 7U) / 8U));
        // Bits past the last entry are not part of the layer.
        if (length % 8U != 0U) {
            out.validity.back() &= static_cast<std::uint8_t>((1U << (length % 8U)) - 1U);
        }
    }
}

// --- protobuf writing, for the synthetic MiniBlock descriptor ---------------------------------------

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_key(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint8_t wire_type) {
    put_varint(out, (static_cast<std::uint64_t>(field) << 3U) | wire_type);
}

void put_varint_field(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint64_t v) {
    put_key(out, field, 0U);
    put_varint(out, v);
}

void put_bytes_field(std::vector<std::uint8_t>& out, std::uint32_t field, const std::vector<std::uint8_t>& bytes) {
    put_key(out, field, 2U);
    put_varint(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

}  // namespace

bool structural_buffer_count(const SparseLayout& layout, std::size_t& count, std::string& error) {
    count = 0;
    for (const auto& layer : layout.layers) {
        switch (layer.kind) {
            case SparseLayer::Kind::kValidity:
                count += needs_buffer(layer.validity.positions) ? 1U : 0U;
                break;
            case SparseLayer::Kind::kList:
                count += needs_buffer(layer.non_empty) ? 1U : 0U;
                count += layer.counts.kind == page_layout::SparseCounts::Kind::kExplicit ? 1U : 0U;
                count += needs_buffer(layer.validity.positions) ? 1U : 0U;
                break;
            case SparseLayer::Kind::kFixedSizeList:
                count += needs_buffer(layer.validity.positions) ? 1U : 0U;
                break;
        }
    }
    (void)error;
    return true;
}

bool decode_u64(const Compressive& encoding, const std::vector<std::uint8_t>& bytes, std::uint64_t count,
                std::vector<std::uint64_t>& out, std::string& error) {
    out.clear();
    std::vector<std::uint8_t> raw;
    if (!child_block::decode(encoding, bytes, count, 64U, raw, error)) {
        return false;
    }
    std::size_t need = 0;
    if (!byte_count(count, kU64Bytes, need) || raw.size() != need) {
        error = "sparse u64 buffer decoded to " + std::to_string(raw.size()) + " bytes for " + std::to_string(count) +
                " values";
        return false;
    }
    out.resize(static_cast<std::size_t>(count));
    if (need != 0U) {
        std::memcpy(out.data(), raw.data(), need);
    }
    return true;
}

bool chunk_items(const std::vector<std::uint8_t>& meta, std::uint64_t value_bytes, std::uint64_t num_visible_items,
                 std::vector<std::uint64_t>& items, std::string& error) {
    items.clear();
    if (meta.size() % 8U != 0U) {
        error = "sparse chunk metadata is " + std::to_string(meta.size()) + " bytes, not whole entries";
        return false;
    }
    std::uint64_t total_items = 0;
    std::uint64_t total_bytes = 0;
    for (std::size_t at = 0; at < meta.size(); at += 8U) {
        const auto words_minus_one = static_cast<std::uint32_t>(
            meta[at] | (meta[at + 1] << 8U) | (meta[at + 2] << 16U) | (static_cast<std::uint32_t>(meta[at + 3]) << 24U));
        const auto values = static_cast<std::uint32_t>(meta[at + 4] | (meta[at + 5] << 8U) | (meta[at + 6] << 16U) |
                                                       (static_cast<std::uint32_t>(meta[at + 7]) << 24U));
        if (values == 0U) {
            error = "sparse layout has an empty value chunk";
            return false;
        }
        total_items += values;
        total_bytes += (static_cast<std::uint64_t>(words_minus_one) + 1U) * kU64Bytes;
        items.push_back(values);
    }
    if (total_items != num_visible_items) {
        error = "sparse value chunks hold " + std::to_string(total_items) + " values, the page declares " +
                std::to_string(num_visible_items);
        return false;
    }
    if (total_bytes != value_bytes) {
        error = "sparse value chunks take " + std::to_string(total_bytes) + " bytes, the value buffer has " +
                std::to_string(value_bytes);
        return false;
    }
    return true;
}

bool unravel(const SparseLayout& layout, std::uint64_t rows, bool leaf_nullable,
             const std::vector<std::vector<std::uint8_t>>& buffers, std::vector<UnraveledLayer>& out,
             std::string& error) {
    out.clear();
    const std::uint64_t leaf_items = layout.num_visible_items;
    const std::size_t n = layout.layers.size();

    // Pass 1, outermost first, the order the buffers are stored in: decode every set.
    std::size_t next = 0;
    std::vector<DecodedLayer> decoded(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& layer = layout.layers[i];
        const std::vector<std::uint8_t>* buffer = nullptr;
        auto& d = decoded[i];
        switch (layer.kind) {
            case SparseLayer::Kind::kValidity: {
                if (needs_buffer(layer.validity.positions) && !take_buffer(buffers, next, buffer, error)) {
                    return false;
                }
                if (!materialize_validity(layer.validity, layer.num_slots, buffer, d.valid, error)) {
                    error = "sparse layer " + std::to_string(i) + ": " + error;
                    return false;
                }
                break;
            }
            case SparseLayer::Kind::kList: {
                const std::vector<std::uint8_t>* ne_buffer = nullptr;
                const std::vector<std::uint8_t>* count_buffer = nullptr;
                const std::vector<std::uint8_t>* valid_buffer = nullptr;
                if (needs_buffer(layer.non_empty) && !take_buffer(buffers, next, ne_buffer, error)) {
                    return false;
                }
                if (layer.counts.kind == page_layout::SparseCounts::Kind::kExplicit &&
                    !take_buffer(buffers, next, count_buffer, error)) {
                    return false;
                }
                if (needs_buffer(layer.validity.positions) && !take_buffer(buffers, next, valid_buffer, error)) {
                    return false;
                }
                if (!materialize_positions(layer.non_empty, layer.num_slots, ne_buffer, d.non_empty, error) ||
                    !materialize_validity(layer.validity, layer.num_slots, valid_buffer, d.valid, error)) {
                    error = "sparse layer " + std::to_string(i) + ": " + error;
                    return false;
                }
                if (!materialize_counts(layer.counts, d.non_empty.size(), count_buffer, d.counts, error)) {
                    error = "sparse layer " + std::to_string(i) + ": " + error;
                    return false;
                }
                break;
            }
            case SparseLayer::Kind::kFixedSizeList:
                error = "sparse layout has a fixed-size-list layer, which nanolance does not read yet";
                return false;
        }
    }
    if (next != buffers.size()) {
        error = "sparse page has " + std::to_string(buffers.size()) + " structural buffers, its layers use " +
                std::to_string(next);
        return false;
    }

    // Pass 2: the domains must chain from the page's rows down to its leaf items.
    if (n != 0U && layout.layers[0].num_slots != rows) {
        error = "sparse outer layer has " + std::to_string(layout.layers[0].num_slots) + " slots for a page of " +
                std::to_string(rows) + " rows";
        return false;
    }
    if (n == 0U && leaf_items != rows) {
        error = "sparse flat page has " + std::to_string(leaf_items) + " items for " + std::to_string(rows) + " rows";
        return false;
    }
    std::uint64_t expected_items = leaf_items;
    for (std::size_t i = 0; i < n; ++i) {
        const auto& layer = layout.layers[i];
        const std::uint64_t child = i + 1U < n ? layout.layers[i + 1U].num_slots : leaf_items;
        const std::uint64_t child_slots = layer.kind == SparseLayer::Kind::kList
                                              ? layer.num_child_slots
                                              : layer.num_slots;
        if (child_slots != child) {
            error = "sparse layer " + std::to_string(i) + " has a child domain of " + std::to_string(child_slots) +
                    " slots, the next layer has " + std::to_string(child);
            return false;
        }
        if (layer.kind == SparseLayer::Kind::kList) {
            expected_items += layer.num_slots - decoded[i].non_empty.size();
        }
    }
    if (expected_items != layout.num_items) {
        error = "sparse page declares " + std::to_string(layout.num_items) + " items, its layers give " +
                std::to_string(expected_items);
        return false;
    }

    // Pass 3: the leaf, then each layer from the innermost outwards. The innermost validity is the leaf's
    // own when the leaf is nullable; any other validity layer is a struct's.
    const bool leaf_validity = leaf_nullable && n != 0U && layout.layers[n - 1U].kind == SparseLayer::Kind::kValidity;
    out.emplace_back();
    out[0].length = leaf_items;
    if (leaf_validity) {
        out[0].kind = repdef::kNullableItem;
        finish_validity(decoded[n - 1U].valid, leaf_items, out[0]);
    } else {
        out[0].kind = repdef::kAllValidItem;
    }
    for (std::size_t step = n; step-- > 0;) {
        if (step == n - 1U && leaf_validity) {
            continue;
        }
        const auto& layer = layout.layers[step];
        const auto& d = decoded[step];
        UnraveledLayer u;
        u.length = layer.num_slots;
        if (layer.kind == SparseLayer::Kind::kValidity) {
            finish_validity(d.valid, u.length, u);
            u.kind = u.null_count != 0U ? repdef::kNullableItem : repdef::kAllValidItem;
        } else {
            // Offsets over the parent slots: a non-empty slot takes its count of child slots; a valid
            // empty slot and a null slot take none. Null slots with children are malformed.
            u.offsets.assign(static_cast<std::size_t>(layer.num_slots) + 1U, 0);
            std::size_t k = 0;
            std::int64_t running = 0;
            bool any_empty = false;
            for (std::uint64_t slot = 0; slot < layer.num_slots; ++slot) {
                const bool valid = is_valid(d.valid, slot);
                if (k < d.non_empty.size() && d.non_empty[k] == slot) {
                    if (!valid) {
                        error = "sparse list slot " + std::to_string(slot) + " is null and has children";
                        return false;
                    }
                    running += static_cast<std::int64_t>(d.counts[k]);
                    ++k;
                } else if (valid) {
                    any_empty = true;
                }
                u.offsets[static_cast<std::size_t>(slot) + 1U] = running;
            }
            if (static_cast<std::uint64_t>(running) != layer.num_child_slots) {
                error = "sparse list counts add to " + std::to_string(running) + ", the layer declares " +
                        std::to_string(layer.num_child_slots) + " child slots";
                return false;
            }
            finish_validity(d.valid, u.length, u);
            if (u.null_count != 0U && any_empty) {
                u.kind = repdef::kNullAndEmptyList;
            } else if (u.null_count != 0U) {
                u.kind = repdef::kNullableList;
            } else if (any_empty) {
                u.kind = repdef::kEmptyableList;
            } else {
                u.kind = repdef::kAllValidList;
            }
        }
        out.push_back(std::move(u));
    }
    if (out.back().length != rows) {
        error = "sparse layers describe " + std::to_string(out.back().length) + " rows, the page " +
                std::to_string(rows);
        return false;
    }
    return true;
}

std::vector<std::uint8_t> value_descriptor(const SparseLayout& layout, std::uint64_t num_items) {
    // MiniBlockLayout: 3 value_compression, 7 num_buffers, 9 num_items, 10 has_large_chunk. No levels.
    std::vector<std::uint8_t> mini_block;
    put_bytes_field(mini_block, 3U, layout.value_compression_bytes);
    put_varint_field(mini_block, 7U, layout.num_buffers);
    put_varint_field(mini_block, 9U, num_items);
    if (layout.has_large_chunk) {
        put_varint_field(mini_block, 10U, 1U);
    }
    // PageLayout: 1 mini_block_layout.
    std::vector<std::uint8_t> page;
    put_bytes_field(page, 1U, mini_block);
    // EncodingAny: 1 type_url, 2 value.
    static const std::string kTypeUrl = "type.googleapis.com/lance.encodings21.PageLayout";
    std::vector<std::uint8_t> any;
    put_key(any, 1U, 2U);
    put_varint(any, kTypeUrl.size());
    any.insert(any.end(), kTypeUrl.begin(), kTypeUrl.end());
    put_bytes_field(any, 2U, page);
    return any;
}

}  // namespace nano_lance::sparse
