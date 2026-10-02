// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// End-to-end test for the compile-time typed writer facade (nanolance/typed_writer.hpp): a schema
// mixing all column kinds (bitpack-declared ints, bss-zstd-declared double, auto bool, auto string)
// written via typed spans, read back with the C++ reader, values compared exactly. Exercises the
// default borrow-buffers (zero-copy) mode across a multi-batch write, which forces the borrowed
// columns through the materialize-on-second-batch fallback too.
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/typed_writer.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

// NB: `require(f(error), error)` is a trap. The two arguments are evaluated in unspecified
// order, so c_str() can capture a pointer into the EMPTY string before f() runs; when f() then fails
// and assigns a long message, the string reallocates and that pointer dangles. Real failures printed
// a stray letter instead of their message. Pass the string itself.
void require(bool ok, const std::string& msg) {
    require(ok, msg.c_str());
}

}  // namespace

int main() {
    namespace nt = nano_lance::typed;
    using PacketSchema = nt::schema<nt::column<std::uint64_t, "ts", nt::encoding::bitpack>,
                                    nt::column<double, "gain", nt::encoding::bss_zstd>,
                                    nt::column<bool, "flag">,
                                    nt::column<std::string_view, "uri">,
                                    nt::column<std::array<std::uint8_t, 6>, "mac">>;

    const std::size_t n = 60000;  // multi-page for every encoding involved
    std::vector<std::uint64_t> ts(n);
    std::vector<double> gain(n);
    std::vector<std::uint8_t> flag_bytes(n);  // bool source; span<const bool> built from a copy below
    std::vector<std::string> uri_storage(n);
    std::vector<std::string_view> uri(n);
    std::vector<bool> flags_vb(n);
    std::unique_ptr<bool[]> flags(new bool[n]);
    std::vector<std::array<std::uint8_t, 6>> mac(n);
    for (std::size_t i = 0; i < n; ++i) {
        ts[i] = 1700000000000000ULL + i * 1500U + (i * 2654435761ULL) % 200U;
        gain[i] = std::sin(static_cast<double>(i) * 0.001) * 10.0;
        flags[i] = (i * 7U + 3U) % 5U < 2U;
        flag_bytes[i] = flags[i] ? 1U : 0U;
        uri_storage[i] = "s3://bucket/capture_" + std::to_string(i % 11U) + ".pcapng";
        uri[i] = uri_storage[i];
        for (std::size_t b = 0; b < 6U; ++b) {
            mac[i][b] = static_cast<std::uint8_t>((i * 31U + b * 7U) & 0xFFU);
        }
    }

    const auto ds = std::filesystem::temp_directory_path() / "nano_lance_typed_writer_test";
    std::error_code ec;
    std::filesystem::remove_all(ds, ec);

    {
        nt::writer<PacketSchema> w(ds.string().c_str(), {.compression_level = 3, .compress = true});
        require(w.ok(), "typed writer construction");
        // Two batches: exercises multi-batch equal-length validation AND the borrow-mode fallback
        // (second batch materializes the first batch's borrowed fixed-width views).
        const std::size_t half = n / 2U;
        require(w.write_batch(std::span<const std::uint64_t>(ts.data(), half),
                              std::span<const double>(gain.data(), half),
                              std::span<const bool>(flags.get(), half),
                              std::span<const std::string_view>(uri.data(), half),
                              std::span<const std::array<std::uint8_t, 6>>(mac.data(), half)),
                w.last_error());
        require(w.write_batch(std::span<const std::uint64_t>(ts.data() + half, n - half),
                              std::span<const double>(gain.data() + half, n - half),
                              std::span<const bool>(flags.get() + half, n - half),
                              std::span<const std::string_view>(uri.data() + half, n - half),
                              std::span<const std::array<std::uint8_t, 6>>(mac.data() + half, n - half)),
                w.last_error());
        require(w.commit(), w.last_error());
        require(w.close(), "close");
    }

    // Mismatched span lengths must be rejected.
    {
        const auto ds_bad = std::filesystem::temp_directory_path() / "nano_lance_typed_writer_bad";
        std::filesystem::remove_all(ds_bad, ec);
        nt::writer<PacketSchema> w(ds_bad.string().c_str(), {});
        require(w.ok(), "bad-length writer construction");
        require(!w.write_batch(std::span<const std::uint64_t>(ts.data(), 10),
                               std::span<const double>(gain.data(), 9),
                               std::span<const bool>(flags.get(), 10),
                               std::span<const std::string_view>(uri.data(), 10),
                               std::span<const std::array<std::uint8_t, 6>>(mac.data(), 10)),
                "mismatched span lengths must fail write_batch");
        std::filesystem::remove_all(ds_bad, ec);
    }

    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(ds, schema, batches, error), error);
    std::size_t row = 0;
    for (auto& batch : batches) {  // several with a parallel read: one per row range
        require(batch.n_children == 5, "five columns");
        const ArrowArray& col_ts = *batch.children[0];
        const ArrowArray& col_gain = *batch.children[1];
        const ArrowArray& col_flag = *batch.children[2];
        const ArrowArray& col_uri = *batch.children[3];
        const ArrowArray& col_mac = *batch.children[4];
        const auto m = static_cast<std::size_t>(col_ts.length);
        require(row + m <= n, "row count");

        require(std::memcmp(col_ts.buffers[1], ts.data() + row, m * 8U) == 0, "ts values");
        require(std::memcmp(col_gain.buffers[1], gain.data() + row, m * 8U) == 0, "gain values");
        const auto* flag_bits = static_cast<const std::uint8_t*>(col_flag.buffers[1]);
        for (std::size_t i = 0; i < m; ++i) {
            require(((flag_bits[i >> 3U] >> (i & 7U)) & 1U) == flag_bytes[row + i], "flag values");
        }
        const auto* uri_offsets = static_cast<const std::int32_t*>(col_uri.buffers[1]);
        const auto* uri_data = static_cast<const char*>(col_uri.buffers[2]);
        for (std::size_t i = 0; i < m; ++i) {
            const std::string_view got(uri_data + uri_offsets[i],
                                       static_cast<std::size_t>(uri_offsets[i + 1] - uri_offsets[i]));
            require(got == uri[row + i], "uri values");
        }

        require(std::memcmp(col_mac.buffers[1], mac.data() + row, m * 6U) == 0, "mac values (fixed_size_binary)");
        row += m;
        ArrowArrayRelease(&batch);
    }
    require(row == n, "row count");
    ArrowSchemaRelease(&schema);
    std::filesystem::remove_all(ds, ec);
    std::cerr << "typed writer: 5-column compile-time schema round-trips OK (borrow + declarations)\n";
    return 0;
}
