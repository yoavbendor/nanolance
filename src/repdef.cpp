// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/repdef.hpp"

#include <algorithm>
#include <cstddef>

namespace nano_lance::repdef {

namespace {

bool is_item_layer(std::uint8_t kind) {
    return kind == kAllValidItem || kind == kNullableItem;
}

void append_bit(UnraveledLayer& layer, bool valid) {
    const auto at = layer.length;
    if (!valid && layer.validity.empty()) {
        // First null: materialize every entry so far as valid.
        layer.validity.assign(static_cast<std::size_t>((at + 8U) / 8U), 0U);
        for (std::uint64_t i = 0; i < at; ++i) {
            layer.validity[static_cast<std::size_t>(i >> 3U)] |= static_cast<std::uint8_t>(1U << (i & 7U));
        }
    }
    if (!layer.validity.empty()) {
        if (layer.validity.size() < static_cast<std::size_t>((at + 8U) / 8U)) {
            layer.validity.resize(static_cast<std::size_t>((at + 8U) / 8U), 0U);
        }
        if (valid) {
            layer.validity[static_cast<std::size_t>(at >> 3U)] |= static_cast<std::uint8_t>(1U << (at & 7U));
        } else {
            ++layer.null_count;
        }
    }
    ++layer.length;
}

}  // namespace

bool unravel(const std::vector<std::uint16_t>& rep_in, bool has_rep, const std::vector<std::uint16_t>& def_in,
             bool has_def, const std::vector<std::uint8_t>& layers, std::uint64_t num_items,
             std::vector<UnraveledLayer>& out, std::string& error) {
    out.clear();
    if (layers.empty() || !is_item_layer(layers[0])) {
        error = "repetition/definition layers must start with an item layer";
        return false;
    }
    // levels_to_rep[d]: the repetition depth at which definition level d is visible. Level 0 (a
    // valid item) is visible everywhere; a null list at depth k is invisible to the k-1 layers
    // inside it, which is how an empty or null list takes a level but no value.
    std::vector<std::uint16_t> levels_to_rep{0};
    std::uint16_t num_lists = 0;
    for (const auto kind : layers) {
        switch (kind) {
            case kAllValidItem:
            case kAllValidList:
                break;
            case kNullableItem:
                levels_to_rep.push_back(num_lists);
                break;
            case kNullableList:
            case kEmptyableList:
                ++num_lists;
                levels_to_rep.push_back(num_lists);
                break;
            case kNullAndEmptyList:
                ++num_lists;
                levels_to_rep.push_back(num_lists);
                levels_to_rep.push_back(num_lists);
                break;
            default:
                error = "unknown repetition/definition layer " + std::to_string(kind);
                return false;
        }
        if (kind == kAllValidList) {
            ++num_lists;
        }
    }
    if ((num_lists != 0U) != has_rep) {
        error = has_rep ? "repetition levels on a column with no list layer"
                        : "a list layer with no repetition levels";
        return false;
    }
    if (has_rep && has_def && rep_in.size() != def_in.size()) {
        error = "repetition and definition level counts differ (" + std::to_string(rep_in.size()) + " vs " +
                std::to_string(def_in.size()) + ")";
        return false;
    }
    if (!has_rep && has_def && num_items != kInferItems && def_in.size() != num_items) {
        error = "definition levels cover " + std::to_string(def_in.size()) + " items, the page holds " +
                std::to_string(num_items);
        return false;
    }
    const auto max_def = static_cast<std::uint16_t>(levels_to_rep.size() - 1U);
    if (has_def && std::any_of(def_in.begin(), def_in.end(), [max_def](std::uint16_t d) { return d > max_def; })) {
        error = "definition level above the layers' maximum of " + std::to_string(max_def);
        return false;
    }
    if (has_rep) {
        if (std::any_of(rep_in.begin(), rep_in.end(), [num_lists](std::uint16_t r) { return r > num_lists; })) {
            error = "repetition level above the " + std::to_string(num_lists) + " list layers";
            return false;
        }
        // A page holds whole rows, so its first level starts a row: the outermost repetition level.
        if (!rep_in.empty() && rep_in[0] != num_lists) {
            error = "repetition levels do not start at a row boundary";
            return false;
        }
    }

    std::vector<std::uint16_t> rep = rep_in;
    std::vector<std::uint16_t> def = has_def ? def_in : std::vector<std::uint16_t>{};
    std::uint16_t def_cmp = 0;  // highest definition level still "valid" at the current layer
    std::uint16_t rep_cmp = 0;  // repetition depth of the current layer

    for (std::size_t li = 0; li < layers.size(); ++li) {
        const auto kind = layers[li];
        UnraveledLayer layer;
        layer.kind = kind;

        if (is_item_layer(kind)) {
            if (!has_def) {
                if (!has_rep && num_items == kInferItems) {
                    error = "an item count is needed when there are no levels to infer it from";
                    return false;
                }
                layer.length = has_rep ? rep.size() : num_items;
            } else {
                const bool nullable = kind == kNullableItem;
                for (const auto level : def) {
                    if (levels_to_rep[level] <= rep_cmp) {
                        append_bit(layer, !nullable || level <= def_cmp);
                    }
                }
            }
            if (kind == kNullableItem) {
                ++def_cmp;
            }
            if (li == 0 && num_items != kInferItems && layer.length != num_items) {
                error = "levels describe " + std::to_string(layer.length) + " items, the page holds " +
                        std::to_string(num_items);
                return false;
            }
            // A struct above a list wraps each of that layer's lists: one entry per list.
            if (li != 0 && layer.length != out.back().length) {
                error = "struct layer " + std::to_string(li) + " has " + std::to_string(layer.length) +
                        " entries over " + std::to_string(out.back().length) + " lists";
                return false;
            }
            out.push_back(std::move(layer));
            continue;
        }

        // A list layer. Which definition levels mean "null list" and "empty list" here.
        const std::uint16_t valid_level = def_cmp;
        std::uint16_t null_level = 0;
        std::uint16_t empty_level = 0;
        switch (kind) {
            case kNullableList:
                null_level = static_cast<std::uint16_t>(valid_level + 1U);
                def_cmp = static_cast<std::uint16_t>(def_cmp + 1U);
                break;
            case kEmptyableList:
                empty_level = static_cast<std::uint16_t>(valid_level + 1U);
                def_cmp = static_cast<std::uint16_t>(def_cmp + 1U);
                break;
            case kNullAndEmptyList:
                null_level = static_cast<std::uint16_t>(valid_level + 1U);
                empty_level = static_cast<std::uint16_t>(valid_level + 2U);
                def_cmp = static_cast<std::uint16_t>(def_cmp + 2U);
                break;
            default:  // kAllValidList
                break;
        }
        // Levels above max_level belong to an outer layer and are invisible here. Nullable structs
        // directly above this list raise it: a null struct masks the list, which then reads as null.
        std::uint16_t max_level = std::max({null_level, empty_level, valid_level});
        const std::uint16_t upper_null = max_level;
        for (std::size_t up = li + 1; up < layers.size() && is_item_layer(layers[up]); ++up) {
            if (layers[up] == kNullableItem) {
                ++max_level;
            }
        }
        ++rep_cmp;

        const std::uint64_t children = out.back().length;
        std::int64_t curlen = 0;
        std::size_t write = 0;
        for (std::size_t read = 0; read < rep.size(); ++read) {
            const auto r = rep[read];
            if (r == 0U) {
                ++curlen;  // continues the current list
                continue;
            }
            rep[write] = static_cast<std::uint16_t>(r - 1U);
            if (has_def) {
                const auto d = def[read];
                def[write] = d;
                if (d == 0U) {
                    layer.offsets.push_back(curlen);
                    ++curlen;
                    append_bit(layer, true);
                } else if (d > max_level) {
                    // An outer layer's null or empty list: no list here.
                } else if ((null_level != 0U && d == null_level) || d > upper_null) {
                    layer.offsets.push_back(curlen);
                    append_bit(layer, false);
                } else if (empty_level != 0U && d == empty_level) {
                    layer.offsets.push_back(curlen);
                    append_bit(layer, true);
                } else {
                    // A valid list whose first child is null.
                    layer.offsets.push_back(curlen);
                    ++curlen;
                    append_bit(layer, true);
                }
            } else {
                layer.offsets.push_back(curlen);
                ++curlen;
                append_bit(layer, true);
            }
            ++write;
        }
        layer.offsets.push_back(curlen);
        rep.resize(write);
        if (has_def) {
            def.resize(write);
        }
        if (static_cast<std::uint64_t>(curlen) != children) {
            error = "list layer " + std::to_string(li) + " covers " + std::to_string(curlen) +
                    " children but the layer inside it has " + std::to_string(children);
            return false;
        }
        out.push_back(std::move(layer));
    }
    return true;
}

namespace {

bool valid_at(const std::vector<std::uint8_t>* bits, std::uint64_t i) {
    return bits == nullptr || bits->empty() ||
           ((static_cast<std::size_t>(i >> 3U) < bits->size()) && (((*bits)[static_cast<std::size_t>(i >> 3U)] >> (i & 7U)) & 1U) != 0U);
}

/// Per layer (outermost first, then the item layer): what occurs among the entries serialized.
struct LayerFlags {
    bool has_null = false;
    bool has_empty = false;
};

class Serializer {
public:
    Serializer(const std::vector<SerializeLayer>& layers, const std::vector<std::uint8_t>& item_validity)
        : layers_(layers), item_validity_(item_validity), flags_(layers.size() + 1U) {
        // list_depth_[k]: how many list layers there are from k inward, k included -- the repetition
        // level that starts a new list at layer k. lists_below_[k]: whether any list sits strictly
        // inside layer k, which decides whether a null there still owns a value slot.
        list_depth_.assign(layers.size() + 1U, 0U);
        lists_below_.assign(layers.size() + 1U, false);
        std::uint16_t depth = 0;
        for (std::size_t k = layers.size(); k-- > 0;) {
            lists_below_[k] = depth != 0U;
            if (layers[k].is_list) {
                ++depth;
            }
            list_depth_[k] = depth;
        }
    }

    bool check(std::uint64_t first_row, std::uint64_t num_rows, std::string& error) const {
        for (std::size_t k = 0; k < layers_.size(); ++k) {
            if (layers_[k].is_list && (layers_[k].offsets == nullptr || layers_[k].offsets->empty())) {
                error = "list layer " + std::to_string(k) + " has no offsets";
                return false;
            }
        }
        if (!layers_.empty() && layers_[0].is_list && first_row + num_rows + 1U > layers_[0].offsets->size()) {
            error = "rows past the end of the outermost list";
            return false;
        }
        return true;
    }

    /// Pass 1: which kinds each layer needs. Pass 2 (emit = true): the levels themselves.
    bool visit(std::size_t k, std::uint64_t idx, std::uint16_t rep, bool emit, std::string& error) {
        if (k == layers_.size()) {
            const bool valid = valid_at(&item_validity_, idx);
            if (!emit) {
                flags_[k].has_null = flags_[k].has_null || !valid;
                return true;
            }
            push(rep, valid ? 0U : null_level_[k], idx);
            return true;
        }
        const auto& layer = layers_[k];
        const bool valid = valid_at(layer.validity, idx);
        if (!layer.is_list) {
            if (!valid) {
                if (!emit) {
                    flags_[k].has_null = true;
                    return true;
                }
                push(rep, null_level_[k], lists_below_[k] ? kNoSlot : idx);
                return true;
            }
            return visit(k + 1U, idx, rep, emit, error);
        }
        const auto& offsets = *layer.offsets;
        if (idx + 1U >= offsets.size()) {
            error = "list layer " + std::to_string(k) + " has too few offsets";
            return false;
        }
        const auto begin = offsets[static_cast<std::size_t>(idx)];
        const auto end = offsets[static_cast<std::size_t>(idx + 1U)];
        if (!valid) {
            if (!emit) {
                flags_[k].has_null = true;
                return true;
            }
            push(rep, null_level_[k], kNoSlot);
            return true;
        }
        if (end < begin) {
            error = "list layer " + std::to_string(k) + " has decreasing offsets";
            return false;
        }
        if (end == begin) {
            if (!emit) {
                flags_[k].has_empty = true;
                return true;
            }
            push(rep, empty_level_[k], kNoSlot);
            return true;
        }
        // The first child inherits the level that starts this list (and any around it); each later
        // child starts a new entry one list level in.
        const auto continuation = static_cast<std::uint16_t>(list_depth_[k] - 1U);
        for (auto c = begin; c < end; ++c) {
            if (!visit(k + 1U, static_cast<std::uint64_t>(c), c == begin ? rep : continuation, emit, error)) {
                return false;
            }
        }
        return true;
    }

    /// Between the passes: fix every layer's kind, and number the definition levels from the items
    /// outward, exactly as `unravel` reads them.
    void assign_levels(Serialized& out) {
        null_level_.assign(layers_.size() + 1U, 0U);
        empty_level_.assign(layers_.size() + 1U, 0U);
        std::uint16_t next = 1;
        out.layers.clear();
        // Items first (innermost), then layers from the inside out.
        const auto& item = flags_[layers_.size()];
        out.layers.push_back(item.has_null ? kNullableItem : kAllValidItem);
        if (item.has_null) {
            null_level_[layers_.size()] = next++;
        }
        for (std::size_t k = layers_.size(); k-- > 0;) {
            const auto& f = flags_[k];
            if (!layers_[k].is_list) {
                out.layers.push_back(f.has_null ? kNullableItem : kAllValidItem);
                if (f.has_null) {
                    null_level_[k] = next++;
                }
                continue;
            }
            if (f.has_null && f.has_empty) {
                out.layers.push_back(kNullAndEmptyList);
                null_level_[k] = next++;
                empty_level_[k] = next++;
            } else if (f.has_null) {
                out.layers.push_back(kNullableList);
                null_level_[k] = next++;
            } else if (f.has_empty) {
                out.layers.push_back(kEmptyableList);
                empty_level_[k] = next++;
            } else {
                out.layers.push_back(kAllValidList);
            }
        }
        out.has_def = next > 1U;
        out.has_rep = list_depth_[0] != 0U;
        out_ = &out;
    }

    std::uint16_t row_rep() const { return list_depth_.empty() ? 0U : list_depth_[0]; }

private:
    static constexpr std::uint64_t kNoSlot = ~std::uint64_t{0};

    void push(std::uint16_t rep, std::uint16_t def, std::uint64_t slot) {
        if (out_->has_rep) {
            out_->rep.push_back(rep);
        }
        if (out_->has_def) {
            out_->def.push_back(def);
        }
        if (slot != kNoSlot) {
            out_->items.push_back(slot);
        }
    }

    const std::vector<SerializeLayer>& layers_;
    const std::vector<std::uint8_t>& item_validity_;
    std::vector<LayerFlags> flags_;
    std::vector<std::uint16_t> list_depth_;
    std::vector<bool> lists_below_;
    std::vector<std::uint16_t> null_level_;
    std::vector<std::uint16_t> empty_level_;
    Serialized* out_ = nullptr;
};

}  // namespace

bool serialize(const std::vector<SerializeLayer>& layers, const std::vector<std::uint8_t>& item_validity,
               std::uint64_t first_row, std::uint64_t num_rows, Serialized& out, std::string& error) {
    out = Serialized{};
    Serializer s(layers, item_validity);
    if (!s.check(first_row, num_rows, error)) {
        return false;
    }
    for (std::uint64_t r = first_row; r < first_row + num_rows; ++r) {
        if (!s.visit(0, r, 0, false, error)) {
            return false;
        }
    }
    s.assign_levels(out);
    const auto rep = s.row_rep();
    for (std::uint64_t r = first_row; r < first_row + num_rows; ++r) {
        if (!s.visit(0, r, rep, true, error)) {
            return false;
        }
    }
    return true;
}

}  // namespace nano_lance::repdef
