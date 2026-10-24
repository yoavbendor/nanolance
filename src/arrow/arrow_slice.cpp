// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/arrow_slice.hpp"

#include <algorithm>

namespace nano_lance {
namespace {

struct ViewPrivate {
    std::shared_ptr<SharedBatch> base;
    std::vector<ArrowArray*> children;
    std::vector<const void*> buffers;
};

void release_child_view(ArrowArray* array) {
    delete static_cast<ViewPrivate*>(array->private_data);
    array->release = nullptr;
}

void release_view(ArrowArray* array) {
    auto* priv = static_cast<ViewPrivate*>(array->private_data);
    for (auto* child : priv->children) {
        if (child->release != nullptr) {
            child->release(child);
        }
        delete child;
    }
    delete priv;
    array->release = nullptr;
}

}  // namespace

ArrowArray slice_batch(const std::shared_ptr<SharedBatch>& base, std::int64_t offset, std::int64_t length,
                       const std::vector<std::int64_t>& order) {
    const ArrowArray& b = base->array;
    auto* priv = new ViewPrivate{base, {}, {}};
    ArrowArray out{};
    out.length = length;
    out.null_count = 0;
    out.offset = 0;
    out.n_buffers = 1;
    priv->buffers.assign(1, nullptr);  // a record batch has no null rows
    out.buffers = priv->buffers.data();
    std::vector<std::int64_t> all;
    if (order.empty()) {
        for (std::int64_t i = 0; i < b.n_children; ++i) {
            all.push_back(i);
        }
    }
    for (const auto index : order.empty() ? all : order) {
        const ArrowArray* src = b.children[index];
        auto* child = new ArrowArray(*src);  // shares the buffers and grandchildren
        child->offset = src->offset + b.offset + offset;
        child->length = length;
        child->null_count = src->null_count == 0 ? 0 : -1;
        child->private_data = new ViewPrivate{base, {}, {}};
        child->release = &release_child_view;
        priv->children.push_back(child);
    }
    out.n_children = static_cast<std::int64_t>(priv->children.size());
    out.children = priv->children.data();
    out.dictionary = nullptr;
    out.private_data = priv;
    out.release = &release_view;
    return out;
}

void slice_batches(std::vector<ArrowArray>& batches, std::uint64_t offset, std::int64_t length) {
    std::vector<ArrowArray> kept;
    std::uint64_t at = 0;  // rows before the current batch
    const bool bounded = length >= 0;
    const auto end = bounded ? offset + static_cast<std::uint64_t>(length) : ~std::uint64_t{0};
    for (auto& batch : batches) {
        const auto rows = static_cast<std::uint64_t>(batch.length);
        const auto first = at;
        at += rows;
        const auto lo = std::max(first, offset);
        const auto hi = std::min(at, end);
        if (hi <= lo) {
            batch.release(&batch);
            continue;
        }
        if (lo == first && hi == at) {
            kept.push_back(batch);
            continue;
        }
        auto shared = std::make_shared<SharedBatch>(std::move(batch));
        kept.push_back(slice_batch(shared, static_cast<std::int64_t>(lo - first), static_cast<std::int64_t>(hi - lo)));
    }
    batches = std::move(kept);
}

}  // namespace nano_lance
