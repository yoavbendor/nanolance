// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <memory>
#include <vector>

/// Zero-copy slices of a record batch (a struct array). A slice is a new struct array whose children
/// are views of the batch's children at an offset; every view keeps the batch alive, so views can be
/// released in any order, children included (Arrow C Data Interface ownership rules).
namespace nano_lance {

/// A decoded batch shared by the views made of it. Owns `array` (releases it when the last view goes).
struct SharedBatch {
    ArrowArray array{};
    SharedBatch() = default;
    explicit SharedBatch(ArrowArray&& a) : array(a) { a.release = nullptr; }
    ~SharedBatch() {
        if (array.release != nullptr) {
            array.release(&array);
        }
    }
    SharedBatch(const SharedBatch&) = delete;
    SharedBatch& operator=(const SharedBatch&) = delete;
};

/// Rows [offset, offset + length) of `base`, with its top-level children in `order` (indices into
/// base's children; all of them, in order, when empty).
ArrowArray slice_batch(const std::shared_ptr<SharedBatch>& base, std::int64_t offset, std::int64_t length,
                       const std::vector<std::int64_t>& order = {});

/// Keep rows [offset, offset + length) of the concatenation of `batches` (length < 0: to the end),
/// slicing the first and last batch kept and releasing the rest. Batches keep their order.
void slice_batches(std::vector<ArrowArray>& batches, std::uint64_t offset, std::int64_t length);

}  // namespace nano_lance
