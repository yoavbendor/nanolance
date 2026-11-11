// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// fixed_size_list<fixed_size_list<int32, 2>, 2> is read (W1a) but not written. The writer must refuse it
// by name at write_batch, not fail with an unrelated message about a missing values buffer.
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    const auto ds = std::filesystem::temp_directory_path() / "nano_lance_fsl_of_fsl_write_refused";
    std::error_code ec;
    std::filesystem::remove_all(ds, ec);

    // Four int32 values: two inner lists of two, one outer row of two inner lists.
    const std::int32_t values[] = {1, 2, 3, 4};
    ArrowArray leaf{};
    leaf.length = 4;
    leaf.n_buffers = 2;
    const void* leaf_buffers[] = {nullptr, values};
    leaf.buffers = leaf_buffers;
    ArrowArray* inner_children[] = {&leaf};
    ArrowArray inner{};
    inner.length = 2;
    inner.n_buffers = 1;
    const void* inner_buffers[] = {nullptr};
    inner.buffers = inner_buffers;
    inner.n_children = 1;
    inner.children = inner_children;
    ArrowArray* outer_children[] = {&inner};
    ArrowArray batch{};
    batch.length = 1;
    batch.n_buffers = 1;
    const void* batch_buffers[] = {nullptr};
    batch.buffers = batch_buffers;
    batch.n_children = 1;
    batch.children = outer_children;

    ArrowSchema leaf_schema{};
    leaf_schema.format = "i";
    leaf_schema.name = "item";
    ArrowSchema* inner_schema_children[] = {&leaf_schema};
    ArrowSchema inner_schema{};
    inner_schema.format = "+w:2";
    inner_schema.name = "item";
    inner_schema.n_children = 1;
    inner_schema.children = inner_schema_children;
    ArrowSchema* outer_schema_children[] = {&inner_schema};
    ArrowSchema field{};
    field.format = "+w:2";
    field.name = "c";
    field.n_children = 1;
    field.children = outer_schema_children;

    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
    const int rc = nano_lance_write_batch(&writer, &batch, &field);
    require(rc == NANO_LANCE_UNSUPPORTED, "a fixed_size_list of fixed_size_list must be refused at write");
    const std::string err = nano_lance_writer_last_error(&writer);
    require(err.find("fixed_size_list of fixed_size_list is read but not written yet") != std::string::npos,
            ("refusal must name the shape, got: " + err).c_str());
    nano_lance_writer_close(&writer);
    std::filesystem::remove_all(ds, ec);
    std::cerr << "fixed_size_list of fixed_size_list is refused at write\n";
    return 0;
}
