// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/writer.hpp"

#include <utility>

namespace nano_lance {

Writer::~Writer() { close(); }

Writer::Writer(Writer&& other) noexcept
    : handle_(other.handle_), status_(other.status_), append_(other.append_) {
    other.handle_ = NanoLanceWriter{};
    other.status_ = NANO_LANCE_OK;
    other.append_ = false;
}

Writer& Writer::operator=(Writer&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        status_ = other.status_;
        append_ = other.append_;
        other.handle_ = NanoLanceWriter{};
        other.status_ = NANO_LANCE_OK;
        other.append_ = false;
    }
    return *this;
}

bool Writer::open(const std::filesystem::path& path, const WriteOptions& options) {
    close();

    // The C options struct borrows these strings for the duration of the call only, so keeping them
    // alive until nano_lance_writer_open returns is all that is needed.
    std::vector<NanoLanceColumnEncoding> encodings;
    encodings.reserve(options.column_encodings.size());
    for (const auto& entry : options.column_encodings) {
        encodings.push_back(NanoLanceColumnEncoding{entry.first.c_str(), entry.second.c_str()});
    }

    NanoLanceWriteOptions c_options{};
    c_options.compression_level = options.compression_level;
    c_options.append = options.append;
    c_options.compression = options.compression;
    c_options.disable_structural_encoding = !options.structural_encoding;
    c_options.blob_uri_dictionary = options.blob_uri_dictionary;
    c_options.borrow_buffers = options.borrow_buffers;
    c_options.column_encodings = encodings.empty() ? nullptr : encodings.data();
    c_options.num_column_encodings = encodings.size();
    c_options.max_pending_bytes = options.max_pending_bytes;

    append_ = options.append;
    status_ = nano_lance_writer_open(&handle_, path.string().c_str(), &c_options);
    return status_ == NANO_LANCE_OK;
}

bool Writer::write_batch(ArrowArray* batch, ArrowSchema* schema) {
    status_ = nano_lance_write_batch(&handle_, batch, schema);
    return status_ == NANO_LANCE_OK;
}

bool Writer::commit() { return commit(append_); }

bool Writer::commit(bool is_append) {
    status_ = nano_lance_writer_commit(&handle_, is_append);
    // Every commit after the first has to be an append, whichever mode the writer was opened in.
    if (status_ == NANO_LANCE_OK) {
        append_ = true;
    }
    return status_ == NANO_LANCE_OK;
}

bool Writer::close() {
    if (handle_.private_data == nullptr) {
        return true;
    }
    status_ = nano_lance_writer_close(&handle_);
    return status_ == NANO_LANCE_OK;
}

std::uint64_t Writer::pending_batches() const { return nano_lance_writer_pending_batches(&handle_); }

std::uint64_t Writer::pending_bytes() const { return nano_lance_writer_pending_bytes(&handle_); }

}  // namespace nano_lance
