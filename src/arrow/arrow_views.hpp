// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Arrow's view types (Utf8View, BinaryView) turned into the string and binary columns they are written as (internal).

#pragma once

#include <nanoarrow/nanoarrow.h>

#include <deque>
#include <string>
#include <vector>

namespace nano_lance::arrow_views {

/// True when `schema`, or a field under it (children, dictionary), is a string_view or binary_view ("vu", "vz").
bool has_view_types(const ArrowSchema& schema);

/// One batch with its view columns copied as string / binary, and the schema to match (pylance writes them so).
/// Plain columns are not copied: they point at the caller's buffers, which must outlive this object and stay
/// unchanged while it is in use. The copies and the schema copy are released with this object. Use it for one batch.
class ViewlessBatch {
public:
    ViewlessBatch() = default;
    ~ViewlessBatch();
    ViewlessBatch(const ViewlessBatch&) = delete;
    ViewlessBatch& operator=(const ViewlessBatch&) = delete;

    /// Convert `batch`, whose schema is `schema`. False with `error` set when a view array is malformed, or when a
    /// view column holds more than 2 GiB of data (a string column's offsets are 32-bit).
    bool convert(const ArrowSchema& schema, const ArrowArray& batch, std::string& error);

    /// The converted batch and schema; valid after a successful convert().
    [[nodiscard]] ArrowArray* array() { return &nodes_.front(); }
    [[nodiscard]] ArrowSchema* schema() { return &schema_; }

private:
    bool convert_node(const ArrowSchema& schema, const ArrowArray& in, ArrowArray& out, std::string& error);

    ArrowSchema schema_{};
    /// Every array node, root first. A view column is an owned copy (its release frees it); any other node is a
    /// shell with a null release that points at the caller's buffers. A deque keeps the addresses stable.
    std::deque<ArrowArray> nodes_;
    std::deque<std::vector<const void*>> buffers_;
    std::deque<std::vector<ArrowArray*>> children_;
};

}  // namespace nano_lance::arrow_views
