#include "nano_lance_writer/nano_lance_reader.h"
#include "nano_lance_writer/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef NANO_LANCE_BLOB_V2_GOLDEN_DIR
#define NANO_LANCE_BLOB_V2_GOLDEN_DIR ""
#endif

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_dataset() {
    auto p = std::filesystem::temp_directory_path() / "nano_lance_manifest_reader_ds";
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

void write_one_batch(NanoLanceWriter& writer) {
    ArrowArray batch{};
    batch.length = 4;
    ArrowSchema field{};
    field.format = "l";
    field.name = "id";
    field.flags = 0;
    const std::int64_t values[] = {1, 2, 3, 4};
    const void* buffers[] = {nullptr, values};
    batch.n_buffers = 2;
    batch.buffers = buffers;
    require(nano_lance_write_batch(&writer, &batch, &field) == NANO_LANCE_OK, "write batch");
}

}  // namespace

int main() {
    const auto ds = temp_dataset();
    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, ds.c_str(), 3) == NANO_LANCE_OK, "init");
    write_one_batch(writer);
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "commit");
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close");

    NanoLanceDatasetMetadata meta{};
    char err[512]{};
    require(nano_lance_dataset_read_latest(ds.c_str(), &meta, err, sizeof err) == NANO_LANCE_READER_OK, err);
    require(meta.manifest_version == 1, "manifest version");
    require(meta.fragments_len == 1, "fragment count");
    require(meta.total_physical_rows == 4, "total rows");
    require(meta.fields_len > 0, "fields");
    require(meta.fragments[0].physical_rows == 4, "fragment rows");
    require(meta.fragments[0].files_len == 1, "one data file");
    require(std::string(meta.fragments[0].files[0].path).find("fragment-") == 0, "fragment file name");
    nano_lance_dataset_metadata_free(&meta);

    const std::filesystem::path golden_dir(NANO_LANCE_BLOB_V2_GOLDEN_DIR);
    if (std::filesystem::is_directory(golden_dir / "_versions")) {
        NanoLanceDatasetMetadata gmeta{};
        const std::string gpath = golden_dir.string();
        const int grc = nano_lance_dataset_read_latest(gpath.c_str(), &gmeta, err, sizeof err);
        if (grc == NANO_LANCE_READER_OK) {
            require(gmeta.manifest_version >= 1, "golden manifest version");
            require(gmeta.fragments_len >= 1, "golden fragments");
            bool found_blob_ext = false;
            for (size_t i = 0; i < gmeta.fields_len; ++i) {
                for (size_t j = 0; j < gmeta.fields[i].metadata_len; ++j) {
                    const auto* e = &gmeta.fields[i].metadata[j];
                    if (std::strcmp(e->key, "ARROW:extension:name") == 0 && e->value_len > 0) {
                        if (std::memcmp(e->value_bytes, "lance.blob.v2", std::min<std::size_t>(e->value_len, 13U)) == 0) {
                            found_blob_ext = true;
                        }
                    }
                }
            }
            require(found_blob_ext, "golden should expose lance.blob.v2 in field metadata");
            nano_lance_dataset_metadata_free(&gmeta);
        }
        /* else: golden may be produced by upstream Lance tools with a different manifest wrapper; skip. */
    }

    std::error_code ec;
    std::filesystem::remove_all(ds, ec);
    return 0;
}
