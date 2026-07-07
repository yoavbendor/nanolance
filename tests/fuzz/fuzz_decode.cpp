// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// libFuzzer harness over nanolance's untrusted-parse surface. Every byte string is fed to the protobuf
// decoders (manifest / file-descriptor / column-metadata) and — via a temp file — to the data-file
// footer + column-metadata reader, which exercises the footer offset math and the read-safety caps.
// Build with -DNANOLANCE_BUILD_FUZZERS=ON on a Clang toolchain; run under -fsanitize=fuzzer,address,undefined.

#include "nanolance/data_file_reader.hpp"

#include "lance_minimal.pb.hpp"

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path temp_path() {
    static const auto dir = std::filesystem::temp_directory_path();
    // Distinct per-process so parallel fuzzer workers don't collide.
    return dir / ("nanolance_fuzz_" + std::to_string(static_cast<unsigned long>(::getpid())) + ".lance");
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::vector<std::uint8_t> bytes(data, data + size);

    // 1. In-memory protobuf decoders (the repeated-field growth paths).
    nano_lance::pb::Manifest manifest;
    (void)nano_lance::pb::decode_manifest(bytes, manifest);
    nano_lance::pb::FileDescriptor descriptor;
    (void)nano_lance::pb::decode_file_descriptor(bytes, descriptor);
    nano_lance::pb::ColumnMetadata column;
    (void)nano_lance::pb::decode_column_metadata(bytes, column);

    // 2. Data-file footer + column-metadata reader (footer offset math + read-safety caps + zstd path
    //    reachable via column decode). Needs a file on disk.
    const auto path = temp_path();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    nano_lance::pb::FileDescriptor fdesc;
    nano_lance::LanceDataFileFooterLayout layout;
    std::string error;
    if (nano_lance::read_lance_data_file_footer_and_descriptor(path, fdesc, layout, error)) {
        std::vector<nano_lance::pb::ColumnMetadata> columns;
        (void)nano_lance::read_lance_data_file_column_metadatas(path, layout, columns, error);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return 0;
}
