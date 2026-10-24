// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/nano_lance_reader.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << msg << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    const auto tmp = std::filesystem::temp_directory_path() / "nano_lance_external_blob.bin";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << "abcdef";
        require(static_cast<bool>(out), "write temp blob");
    }

    const std::string uri = "file://" + tmp.string();
    std::uint8_t buf[8]{};
    size_t n = 0;
    char err[256]{};
    require(nano_lance_fetch_external_blob(uri.c_str(), 2, 3, buf, sizeof buf, &n, err, sizeof err) == NANO_LANCE_READER_OK,
            err);
    require(n == 3, "read length");
    require(std::memcmp(buf, "cde", 3) == 0, "payload");

    n = 0;
    require(nano_lance_fetch_external_blob(uri.c_str(), 0, std::numeric_limits<std::uint64_t>::max(), buf, 4, &n, err,
                                           sizeof err) == NANO_LANCE_READER_OK,
            err);
    require(n == 4, "eof-capped read");

    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return 0;
}
