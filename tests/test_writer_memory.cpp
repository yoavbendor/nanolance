// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The writer's memory budget (nano_lance_writer_set_max_pending_bytes), for devices that must keep
// their resident set small while saving.
//
// The writer buffers every batch until a commit, so without a budget a session's whole data is
// resident at once. With one, write_batch flushes a fragment whenever the buffered data reaches the
// budget. This checks, in order of how much it can be trusted:
//
//   1. the ACCOUNTING -- pending_bytes never exceeds budget + one batch after any write_batch, and
//      the rows all arrive, in several fragments, in order, readable by nanolance's own reader;
//   2. the CONTRACT around it -- the caller's final commit(false) still works after write_batch has
//      flushed on its own, is a no-op when nothing is left, and the budget cannot be combined with
//      blob URI dictionary mode (whose layout cannot be appended to);
//   3. the EFFECT -- peak resident memory while writing 40 x 64 Ki-row batches under a 4 MiB budget
//      stays a small fraction of what the same write costs unbounded. Measured with the kernel's
//      high-water mark, reset before each run. Not under a sanitizer: ASan holds freed memory in
//      quarantine by design, so RSS there measures ASan, not the writer (1 and 2 still run).

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/typed_writer.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cstring>

#if defined(__SANITIZE_ADDRESS__)
#define NANOLANCE_UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#define NANOLANCE_UNDER_SANITIZER 1
#endif
#endif

namespace {

constexpr int kRowsPerBatch = 65536;
constexpr int kBatches = 40;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_dataset(const std::string& name) {
    const auto path = std::filesystem::temp_directory_path() / ("nanolance_writer_memory_" + name + ".lance");
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return path;
}

/// Linux: the resident-set high-water mark, reset by writing 5 to clear_refs. 0 where unavailable.
std::uint64_t reset_and_read_hwm_kb(bool reset) {
    if (reset) {
        std::ofstream("/proc/self/clear_refs") << "5";
    }
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            return std::strtoull(line.c_str() + 6, nullptr, 10);
        }
    }
    return 0;
}

struct Batch {
    ArrowSchema schema{};
    ArrowArray array{};
    std::uint64_t bytes = 0;
};

/// Batch `b`: an int64 id and a ~20-byte string per row.
Batch make_batch(int b) {
    Batch out;
    ArrowSchemaInit(&out.schema);
    require(ArrowSchemaSetTypeStruct(&out.schema, 2) == NANOARROW_OK, "schema");
    require(ArrowSchemaSetType(out.schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK, "schema");
    require(ArrowSchemaSetName(out.schema.children[0], "id") == NANOARROW_OK, "schema");
    require(ArrowSchemaSetType(out.schema.children[1], NANOARROW_TYPE_STRING) == NANOARROW_OK, "schema");
    require(ArrowSchemaSetName(out.schema.children[1], "label") == NANOARROW_OK, "schema");
    require(ArrowArrayInitFromSchema(&out.array, &out.schema, nullptr) == NANOARROW_OK, "array");
    require(ArrowArrayStartAppending(&out.array) == NANOARROW_OK, "append");
    for (int i = 0; i < kRowsPerBatch; ++i) {
        const std::int64_t row = static_cast<std::int64_t>(b) * kRowsPerBatch + i;
        const std::string label = "row-" + std::to_string(row * 7919 % 1000003) + "-payload";
        require(ArrowArrayAppendInt(out.array.children[0], row) == NANOARROW_OK, "append id");
        require(ArrowArrayAppendString(out.array.children[1],
                                       {label.data(), static_cast<std::int64_t>(label.size())}) == NANOARROW_OK,
                "append label");
        require(ArrowArrayFinishElement(&out.array) == NANOARROW_OK, "finish row");
        out.bytes += 8U + 4U + label.size();
    }
    require(ArrowArrayFinishBuildingDefault(&out.array, nullptr) == NANOARROW_OK, "finish");
    return out;
}

void release(Batch& batch) {
    batch.array.release(&batch.array);
    batch.schema.release(&batch.schema);
}

/// Write kBatches through a writer with `budget` (0 = none), checking the accounting after every
/// batch. Returns the high-water RSS growth in KiB while writing (0 where it cannot be measured).
std::uint64_t write_all(const std::filesystem::path& path, std::uint64_t budget, std::uint64_t& max_batch_bytes) {
    NanoLanceWriteOptions options{};
    options.max_pending_bytes = budget;
    NanoLanceWriter writer{};
    require(nano_lance_writer_open(&writer, path.string().c_str(), &options) == NANO_LANCE_OK, "open");
    max_batch_bytes = 0;
    const auto before = reset_and_read_hwm_kb(true);
    for (int b = 0; b < kBatches; ++b) {
        Batch batch = make_batch(b);
        max_batch_bytes = std::max(max_batch_bytes, batch.bytes);
        require(nano_lance_write_batch(&writer, &batch.array, &batch.schema) == NANO_LANCE_OK,
                nano_lance_writer_last_error(&writer));
        release(batch);
        if (budget != 0U) {
            // Between writes the writer holds less than the budget -- it flushes as soon as it
            // reaches it -- and never more than the budget plus the batch that crossed it, even
            // counting buffer capacity rather than size.
            require(nano_lance_writer_pending_bytes(&writer) < budget + 2U * max_batch_bytes,
                    "pending bytes " + std::to_string(nano_lance_writer_pending_bytes(&writer)) +
                        " exceed the budget plus one batch");
        }
    }
    // The caller's code is the same with or without a budget: one commit(false) at the end.
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, nano_lance_writer_last_error(&writer));
    require(nano_lance_writer_pending_rows(&writer) == 0U, "nothing pending after the final commit");
    const auto after = reset_and_read_hwm_kb(false);
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    return after > before ? after - before : 0U;
}

/// Every row, in order, through nanolance's own reader; returns the row count.
std::uint64_t read_ids_in_order(const std::filesystem::path& path) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(path, schema, batches, error), "read back: " + error);
    std::int64_t expected = 0;
    for (auto& batch : batches) {
        const ArrowArray* ids = batch.children[0];
        const auto* values = static_cast<const std::int64_t*>(ids->buffers[1]) + ids->offset;
        for (std::int64_t i = 0; i < ids->length; ++i) {
            require(values[i] == expected, "rows stay in order across flushes (row " + std::to_string(expected) + ")");
            ++expected;
        }
        ArrowArrayRelease(&batch);
    }
    ArrowSchemaRelease(&schema);
    return static_cast<std::uint64_t>(expected);
}

void verify_contents(const std::filesystem::path& path, bool expect_several_fragments) {
    require(read_ids_in_order(path) == static_cast<std::uint64_t>(kBatches) * kRowsPerBatch, "every row arrives");
    std::size_t fragments = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path / "data")) {
        fragments += entry.path().extension() == ".lance" ? 1U : 0U;
    }
    require(expect_several_fragments ? fragments > 1U : fragments == 1U,
            "fragment count " + std::to_string(fragments));
}

void budget_bounds_the_buffer_and_the_rows_survive() {
    std::uint64_t batch_bytes = 0;
    const auto bounded = temp_dataset("bounded");
    const std::uint64_t budget = 4U << 20U;
    const auto bounded_kb = write_all(bounded, budget, batch_bytes);
    verify_contents(bounded, true);

    const auto unbounded = temp_dataset("unbounded");
    const auto unbounded_kb = write_all(unbounded, 0U, batch_bytes);
    verify_contents(unbounded, false);
    std::cerr << "writer memory: peak RSS growth " << unbounded_kb / 1024 << " MiB unbounded, " << bounded_kb / 1024
              << " MiB with a " << (budget >> 20U) << " MiB budget\n";
#ifndef NANOLANCE_UNDER_SANITIZER
    if (unbounded_kb != 0U) {
        // ~52 MiB of rows. Unbounded, all of it is resident at the commit; bounded, a few budgets'
        // worth. The factor is loose on purpose: this guards against the budget being ignored, not
        // against allocator noise.
        require(bounded_kb * 3U < unbounded_kb, "a 4 MiB budget must cut peak memory at least 3x");
        require(bounded_kb < 6U * (budget >> 10U), "peak memory stays within a few budgets");
    }
#endif
    std::error_code ec;
    std::filesystem::remove_all(bounded, ec);
    std::filesystem::remove_all(unbounded, ec);
}

void the_contract_around_the_budget() {
    // A budget smaller than one batch flushes every batch; the final commit then has nothing left
    // and is a no-op rather than "no pending rows to commit".
    const auto path = temp_dataset("tiny");
    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, path.string().c_str(), 0) == NANO_LANCE_OK, "init");
    require(nano_lance_writer_set_max_pending_bytes(&writer, 1U) == NANO_LANCE_OK, "set budget");
    for (int b = 0; b < 3; ++b) {
        Batch batch = make_batch(b);
        require(nano_lance_write_batch(&writer, &batch.array, &batch.schema) == NANO_LANCE_OK,
                nano_lance_writer_last_error(&writer));
        release(batch);
        require(nano_lance_writer_pending_rows(&writer) == 0U, "a batch over budget is flushed at once");
    }
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "a final commit with nothing left is a no-op");
    nano_lance_writer_close(&writer);
    require(read_ids_in_order(path) == 3U * kRowsPerBatch, "three flushed batches read back");

    // Blob URI dictionary mode cannot append, which every flush after the first is.
    NanoLanceWriter dict{};
    require(nano_lance_writer_init(&dict, temp_dataset("dict").string().c_str(), 0) == NANO_LANCE_OK, "init");
    require(nano_lance_writer_set_blob_uri_dictionary(&dict, true) == NANO_LANCE_OK, "dictionary");
    require(nano_lance_writer_set_max_pending_bytes(&dict, 1U << 20U) == NANO_LANCE_UNSUPPORTED,
            "a budget is refused in dictionary mode");
    nano_lance_writer_close(&dict);
    NanoLanceWriteOptions options{};
    options.blob_uri_dictionary = true;
    options.max_pending_bytes = 1U << 20U;
    NanoLanceWriter both{};
    require(nano_lance_writer_open(&both, temp_dataset("both").string().c_str(), &options) == NANO_LANCE_UNSUPPORTED,
            "and refused through the options struct too");
    nano_lance_writer_close(&both);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

void the_typed_writer_takes_a_budget() {
    // The compile-time typed facade is what a C++ device application is most likely to use, and it
    // borrows fixed-width spans by default: a flush inside write_batch has to release them too.
    namespace nt = nano_lance::typed;
    using Schema = nt::schema<nt::column<std::uint64_t, "id">, nt::column<std::string_view, "label">>;
    constexpr std::size_t kRows = 4096;
    constexpr int kTypedBatches = 20;
    std::vector<std::uint64_t> ids(kRows * kTypedBatches);
    std::vector<std::string> storage(ids.size());
    std::vector<std::string_view> labels(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        ids[i] = i;
        storage[i] = "label-" + std::to_string(i * 7919 % 100003);
        labels[i] = storage[i];
    }
    const auto path = temp_dataset("typed");
    {
        nt::writer<Schema> writer(path.string().c_str(), {.max_pending_bytes = 256U << 10U});
        require(writer.ok(), "typed writer with a budget");
        for (int b = 0; b < kTypedBatches; ++b) {
            const auto at = static_cast<std::size_t>(b) * kRows;
            require(writer.write_batch(std::span<const std::uint64_t>(ids.data() + at, kRows),
                                       std::span<const std::string_view>(labels.data() + at, kRows)),
                    writer.last_error());
        }
        require(writer.commit(), writer.last_error());
    }
    require(read_ids_in_order(path) == ids.size(), "every typed row, in order");
    std::size_t fragments = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path / "data")) {
        fragments += entry.path().extension() == ".lance" ? 1U : 0U;
    }
    require(fragments > 1U, "the typed writer's budget flushed");
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

}  // namespace

int main() {
    the_typed_writer_takes_a_budget();
    budget_bounds_the_buffer_and_the_rows_survive();
    the_contract_around_the_budget();
    std::cout << "writer memory tests passed\n";
    return 0;
}
