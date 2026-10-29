// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "arrow/arrow_views.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nano_lance::arrow_views {
namespace {

bool is_view_format(const char* format) {
    return format != nullptr && (std::strcmp(format, "vu") == 0 || std::strcmp(format, "vz") == 0);
}

// The schema copy's view formats become the plain ones, field by field.
ArrowErrorCode retype_views(ArrowSchema& schema) {
    if (schema.format != nullptr && std::strcmp(schema.format, "vu") == 0) {
        if (ArrowSchemaSetFormat(&schema, "u") != NANOARROW_OK) {
            return ENOMEM;
        }
    } else if (schema.format != nullptr && std::strcmp(schema.format, "vz") == 0) {
        if (ArrowSchemaSetFormat(&schema, "z") != NANOARROW_OK) {
            return ENOMEM;
        }
    }
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children[i] != nullptr) {
            const ArrowErrorCode rc = retype_views(*schema.children[i]);
            if (rc != NANOARROW_OK) {
                return rc;
            }
        }
    }
    return NANOARROW_OK;
}

}  // namespace

bool has_view_types(const ArrowSchema& schema) {
    if (is_view_format(schema.format)) {
        return true;
    }
    for (std::int64_t i = 0; i < schema.n_children; ++i) {
        if (schema.children[i] != nullptr && has_view_types(*schema.children[i])) {
            return true;
        }
    }
    return schema.dictionary != nullptr && has_view_types(*schema.dictionary);
}

ViewlessBatch::~ViewlessBatch() {
    for (auto& node : nodes_) {
        if (node.release != nullptr) {
            node.release(&node);
        }
    }
    if (schema_.release != nullptr) {
        schema_.release(&schema_);
    }
}

bool ViewlessBatch::convert(const ArrowSchema& schema, const ArrowArray& batch, std::string& error) {
    error.clear();
    if (schema_.release != nullptr) {
        error = "a view conversion is used for one batch only";
        return false;
    }
    if (ArrowSchemaDeepCopy(&schema, &schema_) != NANOARROW_OK || retype_views(schema_) != NANOARROW_OK) {
        error = "out of memory";
        return false;
    }
    ArrowArray& root = nodes_.emplace_back();
    return convert_node(schema, batch, root, error);
}

bool ViewlessBatch::convert_node(const ArrowSchema& schema, const ArrowArray& in, ArrowArray& out,
                                 std::string& error) {
    if (schema.dictionary != nullptr && has_view_types(*schema.dictionary)) {
        error = "view types inside a dictionary are not supported";
        return false;
    }
    if (is_view_format(schema.format)) {
        // A view column: copy each value out of the views and the data buffers into a string or binary array.
        // The rows are the input's rows [offset, offset + length), so the output has offset 0.
        // Buffers: 0 validity, 1 the 16-byte views, 2..n-2 the data buffers, n-1 the int64 sizes of the data buffers.
        if (in.n_buffers < 3 || in.buffers == nullptr || (in.length > 0 && in.buffers[1] == nullptr)) {
            error = "a view column has no views or no data buffer sizes";
            return false;
        }
        const bool binary = std::strcmp(schema.format, "vz") == 0;
        const auto* validity = static_cast<const std::uint8_t*>(in.buffers[0]);
        const auto* views = static_cast<const std::uint8_t*>(in.buffers[1]);
        const auto* sizes = static_cast<const std::int64_t*>(in.buffers[in.n_buffers - 1]);
        const std::int64_t data_buffers = in.n_buffers - 3;
        if (ArrowArrayInitFromType(&out, binary ? NANOARROW_TYPE_BINARY : NANOARROW_TYPE_STRING) != NANOARROW_OK ||
            ArrowArrayStartAppending(&out) != NANOARROW_OK || ArrowArrayReserve(&out, in.length) != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
        for (std::int64_t i = 0; i < in.length; ++i) {
            const std::int64_t row = in.offset + i;
            if (validity != nullptr && ArrowBitGet(validity, row) == 0) {
                if (ArrowArrayAppendNull(&out, 1) != NANOARROW_OK) {
                    error = "out of memory";
                    return false;
                }
                continue;
            }
            const std::uint8_t* view = views + 16 * row;
            std::int32_t length = 0;
            std::memcpy(&length, view, 4);
            if (length < 0) {
                error = "a view column has a negative length";
                return false;
            }
            // Up to 12 bytes sit inline after the length; longer values have a 4-byte prefix, then the data buffer
            // index and the offset into it.
            const std::uint8_t* bytes = view + 4;
            if (length > 12) {
                std::int32_t index = 0;
                std::int32_t offset = 0;
                std::memcpy(&index, view + 8, 4);
                std::memcpy(&offset, view + 12, 4);
                if (index < 0 || index >= data_buffers || offset < 0) {
                    error = "a view column refers to a data buffer that does not exist";
                    return false;
                }
                const auto* data = static_cast<const std::uint8_t*>(in.buffers[2 + index]);
                if (data == nullptr ||
                    (sizes != nullptr && static_cast<std::int64_t>(offset) + length > sizes[index])) {
                    error = "a view column value runs past its data buffer";
                    return false;
                }
                bytes = data + offset;
            }
            const ArrowBufferView value{{bytes}, length};
            const ArrowErrorCode rc = ArrowArrayAppendBytes(&out, value);
            if (rc != NANOARROW_OK) {
                error = rc == EOVERFLOW ? "a string_view column holds more than 2 GiB in one batch, which a string "
                                          "column cannot hold"
                                        : "out of memory";
                return false;
            }
        }
        if (ArrowArrayFinishBuildingDefault(&out, nullptr) != NANOARROW_OK) {
            error = "out of memory";
            return false;
        }
        return true;
    }

    // Any other column: a shell over the input's fields and buffers, with its children converted. It owns nothing
    // (release stays null), so the caller's buffers are what it points at, and the children keep the input's
    // logical indexing: a parent's offset still applies to them.
    out.length = in.length;
    out.null_count = in.null_count;
    out.offset = in.offset;
    out.n_buffers = in.n_buffers;
    out.dictionary = in.dictionary;
    std::vector<const void*>& buffers = buffers_.emplace_back();
    if (in.n_buffers > 0) {
        buffers.assign(in.buffers, in.buffers + in.n_buffers);
    }
    out.buffers = buffers.data();
    out.n_children = in.n_children;
    if (in.n_children > 0) {
        if (in.children == nullptr || schema.children == nullptr || schema.n_children != in.n_children) {
            error = "a column's schema and array disagree on its children";
            return false;
        }
        std::vector<ArrowArray*>& kids = children_.emplace_back(static_cast<std::size_t>(in.n_children), nullptr);
        out.children = kids.data();
        for (std::int64_t i = 0; i < in.n_children; ++i) {
            if (in.children[i] == nullptr || schema.children[i] == nullptr) {
                error = "a column's schema and array disagree on its children";
                return false;
            }
            ArrowArray& child = nodes_.emplace_back();
            kids[static_cast<std::size_t>(i)] = &child;
            if (!convert_node(*schema.children[i], *in.children[i], child, error)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace nano_lance::arrow_views
