// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Zero-copy slices of record batches (include/nanolance/arrow_slice.hpp): a slice of a struct batch shows rows
// [offset, offset + length) of each child, with the children chosen by index; a view keeps the batch alive
// until the last view is released, in any order; slice_batches keeps the rows of a concatenation that fall in a
// range and releases the rest, each batch exactly once. Release is checked with counting release callbacks.

#include "arrow/arrow_owned.hpp"
#include "nanolance/arrow_slice.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

int g_releases = 0;

void counting_release(ArrowArray* a) {
    ++g_releases;
    a->release = nullptr;
}

// A batch of `rows` rows with no children, only a length and a release that is counted.
ArrowArray counted_batch(std::int64_t rows) {
    ArrowArray a{};
    a.length = rows;
    a.release = &counting_release;
    return a;
}

// A struct batch of an int32 child 0..9 and a uint64 child 100..109.
ArrowArray two_child_batch() {
    std::int32_t ids[10];
    std::uint64_t scores[10];
    for (int i = 0; i < 10; ++i) {
        ids[i] = i;
        scores[i] = 100U + static_cast<std::uint64_t>(i);
    }
    ArrowArray id{};
    ArrowArray score{};
    std::string error;
    require(nano_lance::arrow_owned::uint_array(NANOARROW_TYPE_INT32, ids, 10, 4, id, error), "the id child");
    require(nano_lance::arrow_owned::uint_array(NANOARROW_TYPE_UINT64, scores, 10, 8, score, error),
            "the score child");
    ArrowArray batch{};
    require(nano_lance::arrow_owned::struct_batch({&id, &score}, 10, batch, error), "the batch (" + error + ")");
    return batch;
}

}  // namespace

int main() {
    using nano_lance::SharedBatch;
    int checks = 0;

    // A slice of rows 3..6 of both children: the length, the children and their values.
    {
        auto base = std::make_shared<SharedBatch>(two_child_batch());
        ArrowArray view = nano_lance::slice_batch(base, 3, 4);
        require(view.length == 4, "the slice has 4 rows");
        require(view.n_children == 2, "the slice keeps both children");
        require(view.children[0]->length == 4 && view.children[1]->length == 4, "each child has 4 rows");
        const auto* ids = static_cast<const std::int32_t*>(view.children[0]->buffers[1]);
        const auto* scores = static_cast<const std::uint64_t*>(view.children[1]->buffers[1]);
        for (int i = 0; i < 4; ++i) {
            const auto row = view.children[0]->offset + i;
            require(ids[row] == 3 + i, "slice id " + std::to_string(i));
            require(scores[view.children[1]->offset + i] == 103U + static_cast<std::uint64_t>(i),
                    "slice score " + std::to_string(i));
        }
        checks += 4 + 8;
        view.release(&view);
        require(view.release == nullptr, "a released view clears its release");
    }

    // The child order: only child 1 (the score), by index.
    {
        auto base = std::make_shared<SharedBatch>(two_child_batch());
        ArrowArray view = nano_lance::slice_batch(base, 0, 2, {1});
        require(view.n_children == 1, "a one-child slice has one child");
        const auto* scores = static_cast<const std::uint64_t*>(view.children[0]->buffers[1]);
        require(scores[view.children[0]->offset] == 100U && scores[view.children[0]->offset + 1] == 101U,
                "the chosen child's values are the score column");
        checks += 2;
        view.release(&view);
    }

    // A slice of zero rows is an empty view.
    {
        auto base = std::make_shared<SharedBatch>(two_child_batch());
        ArrowArray view = nano_lance::slice_batch(base, 10, 0);
        require(view.length == 0 && view.children[0]->length == 0, "a slice at the end has no rows");
        checks += 1;
        view.release(&view);
    }

    // Lifetime: the views keep the batch alive. Dropping the base and releasing the views in either order
    // releases the batch exactly once, and only after the last view.
    {
        g_releases = 0;
        auto base = std::make_shared<SharedBatch>(counted_batch(10));
        ArrowArray first = nano_lance::slice_batch(base, 0, 5);
        ArrowArray second = nano_lance::slice_batch(base, 5, 5);
        base.reset();
        require(g_releases == 0, "the batch is alive while views hold it");
        first.release(&first);
        require(g_releases == 0, "the batch is alive while one view is left");
        second.release(&second);
        require(g_releases == 1, "the batch is released once, after the last view");
        checks += 3;
    }

    // slice_batches over three batches of 4 rows (12 rows): rows 2..7 keep the middle of batch 0, all of batch 1,
    // and drop batch 2 (released at once).
    {
        g_releases = 0;
        std::vector<ArrowArray> batches = {counted_batch(4), counted_batch(4), counted_batch(4)};
        nano_lance::slice_batches(batches, 2, 6);
        require(batches.size() == 2, "two batches remain");
        require(batches[0].length == 2 && batches[1].length == 4, "the kept rows are 2 and 4");
        require(g_releases == 1, "the batch past the range is released at once");
        for (auto& b : batches) {
            b.release(&b);
        }
        require(g_releases == 3, "the kept batches are released once each (batch 0 through its last view)");
        checks += 4;
    }

    // Unbounded length (-1): everything from the offset on.
    {
        g_releases = 0;
        std::vector<ArrowArray> batches = {counted_batch(4), counted_batch(4), counted_batch(4)};
        nano_lance::slice_batches(batches, 5, -1);
        require(batches.size() == 2, "rows 5..11 touch two batches");
        require(batches[0].length == 3 && batches[1].length == 4, "the first kept batch is cut at row 5");
        require(g_releases == 1, "the batch before the offset is released");
        for (auto& b : batches) {
            b.release(&b);
        }
        checks += 3;
    }

    // An offset past every row keeps nothing and releases everything.
    {
        g_releases = 0;
        std::vector<ArrowArray> batches = {counted_batch(4), counted_batch(4)};
        nano_lance::slice_batches(batches, 20, -1);
        require(batches.empty(), "a range past the end keeps no batch");
        require(g_releases == 2, "every batch is released");
        checks += 2;
    }

    // A slice of the whole batch keeps the batch as it is (no view made).
    {
        g_releases = 0;
        std::vector<ArrowArray> batches = {counted_batch(4)};
        nano_lance::slice_batches(batches, 0, -1);
        require(batches.size() == 1 && batches[0].length == 4, "the whole batch is kept");
        require(g_releases == 0, "a batch kept whole is not released");
        batches[0].release(&batches[0]);
        require(g_releases == 1, "and is released by its owner");
        checks += 3;
    }

    std::cout << "arrow_slice: " << checks << " checks passed\n";
    return 0;
}
