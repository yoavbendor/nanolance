#include "nanolance/nano_lance_reader.h"
#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_dataset() {
    auto p = std::filesystem::temp_directory_path() / "nano_lance_writer_append_ds";
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

void write_batch(NanoLanceWriter& writer, std::int64_t v0) {
    ArrowArray batch{};
    batch.length = 1;
    ArrowSchema field{};
    field.format = "l";
    field.name = "id";
    field.flags = 0;
    const std::int64_t values[] = {v0};
    const void* buffers[] = {nullptr, values};
    batch.n_buffers = 2;
    batch.buffers = buffers;
    require(nano_lance_write_batch(&writer, &batch, &field) == NANO_LANCE_OK, "write batch");
}

}  // namespace

int main() {
    const auto ds = temp_dataset();
    char err[512]{};

    {
        NanoLanceWriter writer{};
        require(nano_lance_writer_init(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init");
        write_batch(writer, 42);
        require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "create commit");
        write_batch(writer, 77);
        require(nano_lance_writer_commit(&writer, true) == NANO_LANCE_OK, "append commit same session");
        require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    }

    {
        NanoLanceWriter writer{};
        require(nano_lance_writer_init_append(&writer, ds.string().c_str(), 3) == NANO_LANCE_OK, "init_append");
        write_batch(writer, 100);
        require(nano_lance_writer_commit(&writer, true) == NANO_LANCE_OK, "append after init_append");
        require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");
    }

    NanoLanceDatasetMetadata meta{};
    require(nano_lance_dataset_read_latest(ds.string().c_str(), &meta, err, sizeof err) == NANO_LANCE_READER_OK, err);
    require(meta.manifest_version == 3, "expected manifest v3 after three commits");
    require(meta.fragments_len == 3, "expected three fragments");
    require(meta.total_physical_rows == 3, "sum of physical rows");
    require(meta.fragments[0].physical_rows == 1, "first fragment rows");
    require(meta.fragments[1].physical_rows == 1, "second fragment rows");
    require(meta.fragments[2].physical_rows == 1, "third fragment rows");
    require(meta.fragments[0].id != meta.fragments[1].id, "fragment ids should differ");
    require(meta.max_fragment_id >= 2, "max_fragment_id bumped");
    nano_lance_dataset_metadata_free(&meta);

    std::error_code ec;
    std::filesystem::remove_all(ds, ec);
    return 0;
}
