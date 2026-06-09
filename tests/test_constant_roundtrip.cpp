// Round-trip test for ConstantLayout: a constant fixed-width column under --compress is stored as a
// single inline value (zero data buffers) and read back identically.
#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_dataset(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_const_" + std::string(suffix));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

std::uintmax_t data_dir_bytes(const std::filesystem::path& ds) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(ds / "data", ec)) {
        if (e.is_regular_file() && e.path().extension() == ".lance") {
            total += std::filesystem::file_size(e.path(), ec);
        }
    }
    return total;
}

void write_int64(const std::filesystem::path& ds, bool compress, const std::vector<std::int64_t>& vals) {
    ArrowArray batch{};
    batch.length = static_cast<int64_t>(vals.size());
    batch.n_buffers = 2;
    const void* buffers[] = {nullptr, vals.data()};
    batch.buffers = buffers;
    ArrowSchema field{};
    field.format = "l";
    field.name = "v";
    field.flags = 0;
    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
    require(nano_lance_writer_set_compression(&writer, compress) == NANO_LANCE_OK, "set compression");
    require(nano_lance_write_batch(&writer, &batch, &field) == NANO_LANCE_OK, "write");
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
}

std::vector<std::int64_t> read_int64(const std::filesystem::path& ds) {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string error;
    require(nano_lance::lance_table_read_dataset(ds, schema, batches, error), error.c_str());
    require(batches.size() == 1U, "one batch");
    const ArrowArray* col = (batches[0].n_children > 0) ? batches[0].children[0] : &batches[0];
    const auto* v = static_cast<const std::int64_t*>(col->buffers[1]);
    std::vector<std::int64_t> out(v, v + col->length);
    ArrowSchemaRelease(&schema);
    ArrowArrayRelease(&batches[0]);
    return out;
}

}  // namespace

int main() {
    const std::vector<std::int64_t> vals(20000, 1500);  // all identical -> ConstantLayout

    const auto ds_plain = temp_dataset("plain");
    const auto ds_const = temp_dataset("const");
    write_int64(ds_plain, /*compress=*/false, vals);
    write_int64(ds_const, /*compress=*/true, vals);

    require(read_int64(ds_plain) == vals, "plain constant column must round-trip");
    require(read_int64(ds_const) == vals, "ConstantLayout column must round-trip");

    const auto plain_bytes = data_dir_bytes(ds_plain);
    const auto const_bytes = data_dir_bytes(ds_const);
    std::cerr << "constant: plain=" << plain_bytes << "B constant=" << const_bytes << "B\n";
    // A constant column should be tiny regardless of row count (value stored once in the descriptor).
    require(const_bytes < 2048U, "ConstantLayout column must be near-zero on disk");

    std::error_code ec;
    std::filesystem::remove_all(ds_plain, ec);
    std::filesystem::remove_all(ds_const, ec);
    return 0;
}
