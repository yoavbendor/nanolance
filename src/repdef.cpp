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
    if (!has_rep && has_def && def_in.size() != num_items) {
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
            if (li == 0 && layer.length != num_items) {
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

}  // namespace nano_lance::repdef
