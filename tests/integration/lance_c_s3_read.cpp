// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// liblance_c reads a dataset on an S3-compatible store through lance_dataset_open's storage options.
// Driven by bindings/python/tests/test_object_store.py, which writes the dataset with pylance and
// compares what this prints with pylance's answers:
//
//     lance_c_s3_read URI ENDPOINT [ROW ...]
//
// prints `version V`, `rows N`, then `id ROW VALUE` for each ROW (an int64 column `id`, taken with
// lance_dataset_take). Exits 1 with lance-c's error message on any failure.

#include <nanoarrow/nanoarrow.h>

#include "lance/lance.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int fail(const char* what) {
    std::fprintf(stderr, "%s: %s\n", what, lance_last_error_message());
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s URI ENDPOINT [ROW ...]\n", argv[0]);
        return 2;
    }
    const std::string endpoint = argv[2];
    const char* const opts[] = {"access_key_id", "testing", "secret_access_key", "testing", "region", "us-east-1",
                                "endpoint", endpoint.c_str(), "allow_http", "true", nullptr};
    LanceDataset* ds = lance_dataset_open(argv[1], opts, 0);
    if (ds == nullptr) {
        return fail("lance_dataset_open");
    }
    std::printf("version %llu\n", static_cast<unsigned long long>(lance_dataset_version(ds)));
    std::printf("rows %llu\n", static_cast<unsigned long long>(lance_dataset_count_rows(ds)));

    std::vector<uint64_t> rows;
    for (int i = 3; i < argc; ++i) {
        rows.push_back(std::strtoull(argv[i], nullptr, 10));
    }
    int rc = 0;
    if (!rows.empty()) {
        const char* const columns[] = {"id", nullptr};
        ArrowArrayStream stream{};
        if (lance_dataset_take(ds, rows.data(), rows.size(), columns, &stream) != 0) {
            rc = fail("lance_dataset_take");
        } else {
            size_t next = 0;
            for (;;) {
                ArrowArray array{};
                if (stream.get_next(&stream, &array) != 0) {
                    std::fprintf(stderr, "get_next: %s\n", stream.get_last_error(&stream));
                    rc = 1;
                    break;
                }
                if (array.release == nullptr) {
                    break;
                }
                const ArrowArray* id = array.children[0];
                const auto* values = static_cast<const int64_t*>(id->buffers[1]);
                for (int64_t r = 0; r < id->length && next < rows.size(); ++r, ++next) {
                    std::printf("id %llu %lld\n", static_cast<unsigned long long>(rows[next]),
                                static_cast<long long>(values[id->offset + r]));
                }
                array.release(&array);
            }
            stream.release(&stream);
            if (rc == 0 && next != rows.size()) {
                std::fprintf(stderr, "take returned %zu of %zu rows\n", next, rows.size());
                rc = 1;
            }
        }
    }
    lance_dataset_close(ds);
    return rc;
}
