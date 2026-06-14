// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/data_file_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_dataset() {
    auto p = std::filesystem::temp_directory_path() / "nano_lance_data_file_reader_ds";
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

}  // namespace

int main() {
    const auto ds = temp_dataset();

    {
        NanoLanceWriter writer{};
        require(nano_lance_writer_init(&writer, ds.string().c_str(), 0) == NANO_LANCE_OK, "init");
        ArrowArray batch{};
        batch.length = 7;
        ArrowSchema field{};
        field.format = "l";
        field.name = "x";
        field.flags = 0;
        const std::int64_t values[] = {10, 11, 12, 13, 14, 15, 16};
        const void* buffers[] = {nullptr, values};
        batch.n_buffers = 2;
        batch.buffers = buffers;
        require(nano_lance_write_batch(&writer, &batch, &field) == NANO_LANCE_OK, "write batch");
        require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    }

    const auto data_path = ds / "data";
    std::filesystem::path fragment;
    for (const auto& e : std::filesystem::directory_iterator(data_path)) {
        if (e.is_regular_file() && e.path().extension() == ".lance") {
            fragment = e.path();
            break;
        }
    }
    require(!fragment.empty(), "expected fragment .lance file");

    nano_lance::pb::FileDescriptor descriptor{};
    nano_lance::LanceDataFileFooterLayout layout{};
    std::string err;
    require(nano_lance::read_lance_data_file_footer_and_descriptor(fragment, descriptor, layout, err), err.c_str());
    require(descriptor.length == 7ULL, "descriptor row length");
    require(layout.num_columns == 1U, "one physical column");
    require(!descriptor.fields.empty(), "descriptor has fields");

    std::error_code ec;
    std::filesystem::remove_all(ds, ec);
    return 0;
}
