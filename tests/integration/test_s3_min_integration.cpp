// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Live integration test for the built-in S3 reader against a real S3-compatible endpoint (e.g. MinIO).
// It is GATED: with no endpoint configured it SKIPs (CTest skip code 77) so credential-less CI stays green.
//
// To run it, point it at a bucket and an object that holds the deterministic pattern byte[i] == i % 251
// (tests/s3_minio_integration.sh spins up MinIO and uploads exactly that object):
//
//   NANOLANCE_S3_TEST_URI=s3://blobs/capture.bin   (required; absent -> skip)
//   AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY        (required by the reader)
//   AWS_ENDPOINT_URL=http://127.0.0.1:9000          (for MinIO / path-style endpoints)
//   AWS_REGION=us-east-1
//   NANOLANCE_S3_TEST_SIZE=100000                   (optional; object size, default 100000)

#include "nanolance/nano_lance_reader.h"
#include "nanos3reader/s3_reader.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

constexpr int kSkip = 77;  // CTest SKIP_RETURN_CODE
int failures = 0;

unsigned char pattern(std::uint64_t i) { return static_cast<unsigned char>(i % 251); }

// One ranged fetch through the public C API, verifying bytes against the pattern.
void check_range(const std::string& uri, std::uint64_t pos, std::uint64_t size, std::size_t expect_n) {
    std::uint8_t buf[256]{};
    std::size_t n = 0;
    char err[512]{};
    const int rc = nano_lance_fetch_external_blob(uri.c_str(), pos, size, buf, sizeof buf, &n, err, sizeof err);
    if (rc != NANO_LANCE_READER_OK) {
        std::cerr << "FAIL range@" << pos << ": rc=" << rc << " err=" << err << '\n';
        ++failures;
        return;
    }
    if (n != expect_n) {
        std::cerr << "FAIL range@" << pos << ": got " << n << " bytes, want " << expect_n << '\n';
        ++failures;
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (buf[i] != pattern(pos + i)) {
            std::cerr << "FAIL range@" << pos << ": byte " << i << " is " << int(buf[i]) << ", want "
                      << int(pattern(pos + i)) << '\n';
            ++failures;
            return;
        }
    }
    std::cout << "ok range@" << pos << " (" << n << " bytes)\n";
}

}  // namespace

int main() {
    const char* uri_env = std::getenv("NANOLANCE_S3_TEST_URI");
    if (uri_env == nullptr || uri_env[0] == '\0') {
        std::cout << "SKIP: set NANOLANCE_S3_TEST_URI (+ AWS_* env) to run the live S3 integration test\n";
        return kSkip;
    }
    const std::string uri = uri_env;
    const std::uint64_t size =
        std::getenv("NANOLANCE_S3_TEST_SIZE") ? std::strtoull(std::getenv("NANOLANCE_S3_TEST_SIZE"), nullptr, 10)
                                              : 100000ULL;

    // 1. Ranged reads at varied offsets (signature must be accepted by the real endpoint).
    check_range(uri, 0, 16, 16);
    check_range(uri, 40000, 64, 64);
    // 2. EOF-capped read at the tail: ask for more than remains, expect exactly the remainder.
    if (size >= 10) {
        check_range(uri, size - 10, 100, 10);
    }

    // 3. Multi-window stitching: a tiny read-ahead forces many sequential 206 GETs over one connection.
    {
        nanos3reader::S3MinStreamFactory factory;
        auto s = factory.open(uri, 4096);
        if (!s) {
            std::cerr << "FAIL multiwindow open: " << factory.error() << '\n';
            ++failures;
        } else {
            std::uint64_t off = 0;
            s->clear();
            s->seekg(0);
            bool bad = false;
            while (off < size && !bad) {
                char b[1000];
                s->read(b, sizeof b);
                const std::streamsize got = s->gcount();
                for (std::streamsize i = 0; i < got; ++i) {
                    if (static_cast<unsigned char>(b[i]) != pattern(off + static_cast<std::uint64_t>(i))) {
                        std::cerr << "FAIL multiwindow: mismatch at " << (off + i) << '\n';
                        ++failures;
                        bad = true;
                        break;
                    }
                }
                if (got == 0) {
                    break;
                }
                off += static_cast<std::uint64_t>(got);
            }
            if (!bad && off == size) {
                std::cout << "ok multiwindow (stitched " << size << " bytes over 4 KiB windows)\n";
            } else if (!bad) {
                std::cerr << "FAIL multiwindow: read " << off << " of " << size << " bytes\n";
                ++failures;
            }
        }
    }

    // 4. A missing key must surface as an I/O error (NoSuchKey), not a silent success.
    {
        const std::string missing = uri + ".nanolance_missing";
        std::uint8_t buf[8]{};
        std::size_t n = 0;
        char err[512]{};
        const int rc =
            nano_lance_fetch_external_blob(missing.c_str(), 0, 8, buf, sizeof buf, &n, err, sizeof err);
        if (rc == NANO_LANCE_READER_OK) {
            std::cerr << "FAIL missing-key: expected I/O error, got OK with " << n << " bytes\n";
            ++failures;
        } else {
            std::cout << "ok missing-key surfaced: " << err << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " S3 integration check(s) failed\n";
        return 1;
    }
    std::cout << "S3 integration OK\n";
    return 0;
}
