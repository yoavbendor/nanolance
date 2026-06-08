#include "nano_lance_writer/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path temp_dataset_path() {
    auto path = std::filesystem::temp_directory_path() / "nano_lance_writer_lifecycle_dataset";
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return path;
}

}  // namespace

int main() {
    NanoLanceWriter writer{};
    require(nano_lance_writer_init(nullptr, "/tmp/unused", 3) == NANO_LANCE_INVALID_ARGUMENT,
            "null writer init should fail");
    require(nano_lance_writer_init(&writer, "", 3) == NANO_LANCE_INVALID_ARGUMENT, "empty path should fail");
    require(std::string(nano_lance_writer_last_error(&writer)).find("dataset path") != std::string::npos,
            "empty path error not reported");
    require(nano_lance_writer_init(&writer, "/tmp/unused", 23) == NANO_LANCE_INVALID_ARGUMENT,
            "invalid compression level should fail");

    const auto dataset_path = temp_dataset_path();
    require(nano_lance_writer_init(&writer, dataset_path.c_str(), 3) == NANO_LANCE_OK, "valid init should pass");
    require(nano_lance_writer_pending_batches(&writer) == 0, "new writer should have no pending batches");

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
    require(nano_lance_write_batch(&writer, &batch, &field) == NANO_LANCE_OK, "write batch should pass");
    require(nano_lance_writer_pending_batches(&writer) == 1, "pending batch count mismatch");
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK, "create commit should pass");
    require(std::filesystem::exists(dataset_path / "_versions"), "commit should create _versions directory");
    require(std::filesystem::exists(dataset_path / "data"), "commit should create data directory");
    require(nano_lance_writer_pending_batches(&writer) == 0, "commit should clear pending batches");
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_INVALID_STATE, "commit without pending rows should fail");
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "close should pass");
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "double close should be harmless");

    std::error_code error;
    std::filesystem::remove_all(dataset_path, error);
    return 0;
}
