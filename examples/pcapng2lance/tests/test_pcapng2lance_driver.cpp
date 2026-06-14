// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// M1 end-to-end driver test. Writes a pcapng fixture, runs the pcapng2lance executable on it, then:
//   - checks the committed manifest (row count + expected columns) via nano_lance_dataset_read_latest,
//   - verifies the external-payload premise: nano_lance_fetch_external_blob(uri, off, size) returns
//     exactly the source file's bytes at [off, off+size).
// Per-column VALUE checks live in the pylance interop test (stock Lance reads every column type; the
// nanolance writer-parity reader's fast path doesn't cover uint16 when a blob column is present).
// argv[1] = path to the pcapng2lance executable (passed by CTest).

#include "pcap_fixtures.hpp"
#include "nanotins/pcap_blocks.hpp"

#include "nanolance/nano_lance_reader.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "driver test failed: %s\n", msg);
        std::exit(1);
    }
}

void write_file(const std::filesystem::path& p, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string file_uri(const std::filesystem::path& p) {
    const auto abs = std::filesystem::absolute(p).generic_string();
    return abs.size() > 1 && abs[1] == ':' ? ("file:///" + abs) : ("file://" + abs);
}

bool has_field(const NanoLanceDatasetMetadata& meta, const char* name) {
    for (std::size_t i = 0; i < meta.fields_len; ++i) {
        if (meta.fields[i].name && std::strcmp(meta.fields[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    require(argc >= 2, "expected path to pcapng2lance executable");
    const std::string exe = argv[1];

    const auto tmp = std::filesystem::temp_directory_path() / "pcapng2lance_driver";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp);
    const auto fixture = tmp / "capture.pcapng";
    const auto dataset = tmp / "out.lance";

    const auto bytes = pcapfix::build_pcapng(/*le=*/true);
    write_file(fixture, bytes);

    // Run the converter. On Windows cmd.exe strips the outer quote pair, so wrap the whole command.
    const std::string inner = "\"" + exe + "\" \"" + fixture.string() + "\" \"" + dataset.string() + "\"";
#ifdef _WIN32
    const std::string cmd = "\"" + inner + "\"";
#else
    const std::string cmd = inner;
#endif
    require(std::system(cmd.c_str()) == 0, "pcapng2lance exited non-zero");

    // --- manifest sanity: row count + expected columns present ---
    NanoLanceDatasetMetadata meta{};
    char err[512]{};
    require(nano_lance_dataset_read_latest(dataset.string().c_str(), &meta, err, sizeof err) == NANO_LANCE_READER_OK,
            err);
    require(meta.total_physical_rows == 2, "expected 2 committed rows");
    require(has_field(meta, "ts_raw") && has_field(meta, "caplen") && has_field(meta, "link_type") &&
                has_field(meta, "ts_resol") && has_field(meta, "epb_flags") && has_field(meta, "payload_ref"),
            "expected packet columns present in manifest");

    // SHB option (shb_os) was stored as dataset KV metadata on a field; verify it round-tripped.
    bool found_shb_os = false;
    for (std::size_t i = 0; i < meta.fields_len; ++i) {
        for (std::size_t m = 0; m < meta.fields[i].metadata_len; ++m) {
            const auto& kv = meta.fields[i].metadata[m];
            if (kv.key && std::strcmp(kv.key, "pcapng:shb0:os") == 0) {
                const std::string val(reinterpret_cast<const char*>(kv.value_bytes), kv.value_len);
                require(val == "nanolance-test", "shb_os metadata value mismatch");
                found_shb_os = true;
            }
        }
    }
    require(found_shb_os, "pcapng:shb:os metadata missing");
    nano_lance_dataset_metadata_free(&meta);

    // --- external-payload premise: fetch via the reader API and byte-compare to the source ---
    pcapblocks::Bytes span(bytes.data(), bytes.size());
    std::vector<pcapblocks::BlockRef> refs;
    std::string serr;
    require(pcapblocks::scan_blocks(span, refs, serr), serr.c_str());
    const std::string uri = file_uri(fixture);
    int packet_index = 0;
    const std::vector<std::vector<std::uint8_t>> expected = {pcapfix::kPayload0, pcapfix::kPayload1};
    for (const auto& r : refs) {
        if (r.kind != pcapblocks::Kind::Epb) {
            continue;
        }
        pcapblocks::EpbView e{};
        require(pcapblocks::parse_epb(span, r, e), "parse_epb");
        std::vector<std::uint8_t> buf(e.caplen);
        std::size_t got = 0;
        char ferr[512]{};
        const int rc = nano_lance_fetch_external_blob(uri.c_str(), e.payload_file_offset, e.caplen, buf.data(),
                                                      buf.size(), &got, ferr, sizeof ferr);
        require(rc == 0, ferr);
        require(got == e.caplen, "fetched size mismatch");
        require(buf == expected[packet_index], "fetched payload bytes mismatch source");
        ++packet_index;
    }
    require(packet_index == 2, "expected to fetch 2 payloads");

    std::puts("pcapng2lance driver test ok");
    return 0;
}
