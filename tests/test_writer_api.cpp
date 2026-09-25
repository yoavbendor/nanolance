// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The two shapes of the writer's front door.
//
// `nano_lance_writer_open` takes every option at once, so "all the set_* calls must come before the
// first write_batch" stops being a rule enforced at runtime and becomes a thing you cannot express.
// `nano_lance::Writer` wraps the handle so that closing it is not the caller's problem.
//
// What these tests are really protecting is EQUIVALENCE: the new spellings must produce byte-identical
// datasets to the old ones, or they are a second way to write files rather than a nicer way to write
// the same files.

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"
#include "nanolance/writer.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path scratch(const char* suffix) {
    auto p = std::filesystem::temp_directory_path() / ("nano_lance_writer_api_" + std::string(suffix));
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

const std::int64_t kIds[] = {4, 4, 4, 7, 7, 9};
const char kNameData[] = "alicebobcarolalicebobcarol";
const std::int32_t kNameOffsets[] = {0, 5, 8, 13, 18, 21, 26};

/// A two-column batch: an int64 with runs in it and a utf8 column, so the structural encodings and
/// the zstd path both have something to chew on.
void write_sample(NanoLanceWriter* writer) {
    ArrowSchema children[2]{};
    children[0].format = "l";
    children[0].name = "id";
    children[1].format = "u";
    children[1].name = "name";
    ArrowSchema* child_ptrs[2] = {&children[0], &children[1]};
    ArrowSchema schema{};
    schema.format = "+s";
    schema.n_children = 2;
    schema.children = child_ptrs;

    const void* id_buffers[2] = {nullptr, kIds};
    ArrowArray id_array{};
    id_array.length = 6;
    id_array.n_buffers = 2;
    id_array.buffers = id_buffers;

    const void* name_buffers[3] = {nullptr, kNameOffsets, kNameData};
    ArrowArray name_array{};
    name_array.length = 6;
    name_array.n_buffers = 3;
    name_array.buffers = name_buffers;

    ArrowArray* array_children[2] = {&id_array, &name_array};
    ArrowArray batch{};
    batch.length = 6;
    batch.n_children = 2;
    batch.children = array_children;

    require(nano_lance_write_batch(writer, &batch, &schema) == NANO_LANCE_OK,
            std::string("write_batch: ") + nano_lance_writer_last_error(writer));
}

std::vector<std::uint8_t> data_file_bytes(const std::filesystem::path& ds) {
    for (const auto& e : std::filesystem::directory_iterator(ds / "data")) {
        if (e.path().extension() == ".lance") {
            std::ifstream in(e.path(), std::ios::binary);
            return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                             std::istreambuf_iterator<char>());
        }
    }
    require(false, "no data file written");
    return {};
}

/// The same dataset, written the old way and the new way, must come out byte for byte the same.
void test_options_struct_matches_the_setters() {
    const auto old_way = scratch("setters");
    {
        NanoLanceWriter w{};
        require(nano_lance_writer_init(&w, old_way.string().c_str(), 3) == NANO_LANCE_OK, "init");
        require(nano_lance_writer_set_compression(&w, true) == NANO_LANCE_OK, "set_compression");
        require(nano_lance_writer_set_column_encoding(&w, "id", "bitpack") == NANO_LANCE_OK, "set_encoding");
        write_sample(&w);
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
    }

    const auto new_way = scratch("options");
    {
        const NanoLanceColumnEncoding encodings[] = {{"id", "bitpack"}};
        NanoLanceWriteOptions options{};
        options.compression_level = 3;
        options.compression = true;
        options.column_encodings = encodings;
        options.num_column_encodings = 1;

        NanoLanceWriter w{};
        require(nano_lance_writer_open(&w, new_way.string().c_str(), &options) == NANO_LANCE_OK,
                std::string("open: ") + w.last_error);
        write_sample(&w);
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
    }

    require(data_file_bytes(old_way) == data_file_bytes(new_way),
            "the options struct must produce the same bytes as the equivalent set_* calls");

    std::error_code ec;
    std::filesystem::remove_all(old_way, ec);
    std::filesystem::remove_all(new_way, ec);
}

/// A zeroed struct, a NULL pointer and plain `init` all have to mean the same writer -- that is the
/// property that lets `NanoLanceWriteOptions options = {0}` stay correct as fields are added.
void test_zeroed_options_are_the_defaults() {
    const auto by_init = scratch("default_init");
    {
        NanoLanceWriter w{};
        require(nano_lance_writer_init(&w, by_init.string().c_str(), 0) == NANO_LANCE_OK, "init");
        write_sample(&w);
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
    }

    const auto by_zero = scratch("default_zero");
    {
        NanoLanceWriteOptions options;
        nano_lance_write_options_init(&options);
        NanoLanceWriter w{};
        require(nano_lance_writer_open(&w, by_zero.string().c_str(), &options) == NANO_LANCE_OK, "open");
        write_sample(&w);
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
    }

    const auto by_null = scratch("default_null");
    {
        NanoLanceWriter w{};
        require(nano_lance_writer_open(&w, by_null.string().c_str(), nullptr) == NANO_LANCE_OK, "open NULL");
        write_sample(&w);
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
    }

    const auto expected = data_file_bytes(by_init);
    require(data_file_bytes(by_zero) == expected, "a zeroed options struct must mean the defaults");
    require(data_file_bytes(by_null) == expected, "a NULL options pointer must mean the defaults");

    std::error_code ec;
    for (const auto& p : {by_init, by_zero, by_null}) {
        std::filesystem::remove_all(p, ec);
    }
}

/// A one-column batch whose values are all identical, so structural encoding has something
/// unambiguous to do with it (ConstantLayout: the value lives in the page descriptor and the page
/// has no data buffers at all).
///
/// The two-column sample above deliberately is NOT used for this: at six rows, bitpacking correctly
/// loses to a flat page -- a FastLanes chunk pads to 1024 values whatever the row count -- so
/// structural encoding on and off produce the same bytes and the test would prove nothing. That is
/// the writer being right, not the option being ignored, and it is exactly the confusion a constant
/// column avoids.
void write_constant_sample(NanoLanceWriter* writer) {
    ArrowSchema child{};
    child.format = "l";
    child.name = "k";
    ArrowSchema* child_ptrs[1] = {&child};
    ArrowSchema schema{};
    schema.format = "+s";
    schema.n_children = 1;
    schema.children = child_ptrs;

    static const std::int64_t kSame[8] = {11, 11, 11, 11, 11, 11, 11, 11};
    const void* buffers[2] = {nullptr, kSame};
    ArrowArray column{};
    column.length = 8;
    column.n_buffers = 2;
    column.buffers = buffers;

    ArrowArray* array_children[1] = {&column};
    ArrowArray batch{};
    batch.length = 8;
    batch.n_children = 1;
    batch.children = array_children;

    require(nano_lance_write_batch(writer, &batch, &schema) == NANO_LANCE_OK,
            std::string("write_batch: ") + nano_lance_writer_last_error(writer));
}

/// `disable_structural_encoding` is negated so a zeroed struct keeps the default; check the negation
/// actually reaches the writer rather than being ignored in either direction.
void test_disable_structural_encoding_is_wired_the_right_way_round() {
    const auto with_setter = scratch("nostruct_setter");
    {
        NanoLanceWriter w{};
        require(nano_lance_writer_init(&w, with_setter.string().c_str(), 0) == NANO_LANCE_OK, "init");
        require(nano_lance_writer_set_structural_encoding(&w, false) == NANO_LANCE_OK, "set_structural");
        write_constant_sample(&w);
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
    }

    const auto with_option = scratch("nostruct_option");
    {
        NanoLanceWriteOptions options{};
        options.disable_structural_encoding = true;
        NanoLanceWriter w{};
        require(nano_lance_writer_open(&w, with_option.string().c_str(), &options) == NANO_LANCE_OK, "open");
        write_constant_sample(&w);
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
    }

    const auto defaults = scratch("struct_default");
    {
        NanoLanceWriter w{};
        require(nano_lance_writer_open(&w, defaults.string().c_str(), nullptr) == NANO_LANCE_OK, "open");
        write_constant_sample(&w);
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
    }

    require(data_file_bytes(with_option) == data_file_bytes(with_setter),
            "disable_structural_encoding must match set_structural_encoding(false)");
    require(data_file_bytes(with_option) != data_file_bytes(defaults),
            "...and must actually differ from the default, or it is being ignored");

    std::error_code ec;
    for (const auto& p : {with_setter, with_option, defaults}) {
        std::filesystem::remove_all(p, ec);
    }
}

void test_open_rejects_bad_options() {
    NanoLanceWriteOptions options{};
    options.compression_level = 99;
    NanoLanceWriter w{};
    const auto bad_level = scratch("bad");
    require(nano_lance_writer_open(&w, bad_level.string().c_str(), &options) == NANO_LANCE_INVALID_ARGUMENT,
            "an out-of-range compression level must be refused");
    require(w.private_data == nullptr, "a refused open must not leave a handle behind");

    options = NanoLanceWriteOptions{};
    options.num_column_encodings = 2;  // ...with a NULL array.
    require(nano_lance_writer_open(&w, bad_level.string().c_str(), &options) == NANO_LANCE_INVALID_ARGUMENT,
            "a NULL encoding array with a non-zero count must be refused");

    options = NanoLanceWriteOptions{};
    const NanoLanceColumnEncoding bogus[] = {{"id", "brotli"}};
    options.column_encodings = bogus;
    options.num_column_encodings = 1;
    require(nano_lance_writer_open(&w, bad_level.string().c_str(), &options) == NANO_LANCE_INVALID_ARGUMENT,
            "an unknown column encoding must be refused at open");

    options = NanoLanceWriteOptions{};
    options.append = true;
    options.blob_uri_dictionary = true;
    require(nano_lance_writer_open(&w, bad_level.string().c_str(), &options) == NANO_LANCE_UNSUPPORTED,
            "blob URI dictionary on an append writer must be refused, with the same code as the setter");

    std::error_code ec;
    std::filesystem::remove_all(bad_level, ec);
}

/// The RAII type: same bytes as the C sequence, and it closes itself on an early return.
void test_cxx_writer() {
    const auto by_hand = scratch("cxx_by_hand");
    {
        NanoLanceWriter w{};
        require(nano_lance_writer_init(&w, by_hand.string().c_str(), 3) == NANO_LANCE_OK, "init");
        require(nano_lance_writer_set_compression(&w, true) == NANO_LANCE_OK, "set_compression");
        write_sample(&w);
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit");
        require(nano_lance_writer_close(&w) == NANO_LANCE_OK, "close");
    }

    const auto by_raii = scratch("cxx_raii");
    {
        nano_lance::Writer writer;
        nano_lance::WriteOptions options;
        options.compression_level = 3;
        options.compression = true;
        require(writer.open(by_raii, options), std::string("open: ") + writer.error());
        write_sample(writer.handle());
        require(writer.pending_batches() == 1, "one pending batch");
        require(writer.commit(), std::string("commit: ") + writer.error());
        // No close() -- the destructor does it.
    }
    require(data_file_bytes(by_raii) == data_file_bytes(by_hand), "the RAII writer must write the same bytes");

    // An early return with the writer still open must not leak: run it under whatever sanitizer the
    // build has, and check the dataset is left in the state a close() would leave it in.
    const auto abandoned = scratch("cxx_abandoned");
    {
        nano_lance::Writer writer;
        require(writer.open(abandoned), "open");
        write_sample(writer.handle());
        // ...and then just leave, without committing or closing.
    }
    require(!std::filesystem::exists(abandoned / "_versions"),
            "an uncommitted writer leaves no manifest behind");

    // Move: the moved-from writer must be empty, and the moved-to one must still work.
    const auto moved = scratch("cxx_moved");
    {
        nano_lance::Writer source;
        require(source.open(moved), "open");
        nano_lance::Writer sink = std::move(source);
        require(!source.is_open(), "a moved-from writer owns nothing");  // NOLINT(bugprone-use-after-move)
        require(sink.is_open(), "a moved-to writer owns the handle");
        write_sample(sink.handle());
        require(sink.commit(), std::string("commit: ") + sink.error());
    }
    {
        ArrowSchema schema{};
        std::vector<ArrowArray> batches;
        std::string err;
        require(nano_lance::lance_table_read_dataset(moved, schema, batches, err), err);
        require(batches.size() == 1 && batches[0].length == 6, "moved writer's dataset reads back");
        for (auto& b : batches) {
            if (b.release != nullptr) {
                b.release(&b);
            }
        }
        if (schema.release != nullptr) {
            schema.release(&schema);
        }
    }

    std::error_code ec;
    for (const auto& p : {by_hand, by_raii, abandoned, moved}) {
        std::filesystem::remove_all(p, ec);
    }
}

}  // namespace

int main() {
    test_options_struct_matches_the_setters();
    test_zeroed_options_are_the_defaults();
    test_disable_structural_encoding_is_wired_the_right_way_round();
    test_open_rejects_bad_options();
    test_cxx_writer();
    std::cout << "writer API tests passed\n";
    return 0;
}
