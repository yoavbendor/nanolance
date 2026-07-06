// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Python bindings for nanolance: fast Arrow <-> Lance reader/writer.

#include "arrow_capsule.hpp"

#include <nanolance/nano_lance_reader.h>
#include <nanolance/nano_lance_writer.h>

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace nb = nanobind;
using nanolance_py::arrow_capsule::BatchIterator;
using nanolance_py::arrow_capsule::ExportedTable;
using nanolance_py::arrow_capsule::detail::OwnedArray;
using nanolance_py::arrow_capsule::detail::OwnedSchema;

namespace {

void throw_lance_writer(const char* context, int code, const NanoLanceWriter* writer) {
    const char* err = writer ? nano_lance_writer_last_error(writer) : "";
    std::string msg = std::string(context) + " failed";
    if (err != nullptr && err[0] != '\0') {
        msg += ": ";
        msg += err;
    }
    (void)code;
    throw std::runtime_error(msg);
}

void throw_lance_reader(const char* context, int code, const char* err) {
    std::string msg = std::string(context) + " failed";
    if (err != nullptr && err[0] != '\0') {
        msg += ": ";
        msg += err;
    }
    (void)code;
    throw std::runtime_error(msg);
}

struct LanceWriterOptions {
    int compression_level = 3;
    bool compression = false;
    bool blob_uri_dictionary = false;
    bool ignore_nullability = true;
    bool append = false;
};

void write_table(nb::handle table, const std::filesystem::path& path, const LanceWriterOptions& opts) {
    NanoLanceWriter writer{};
    int rc = opts.append ? nano_lance_writer_init_append(&writer, path.string().c_str(),
                                                         opts.compression_level)
                         : nano_lance_writer_init(&writer, path.string().c_str(),
                                                  opts.compression_level);
    if (rc != NANO_LANCE_OK) {
        throw_lance_writer("nano_lance_writer_init", rc, &writer);
    }

    if (opts.compression) {
        rc = nano_lance_writer_set_compression(&writer, true);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_writer_set_compression", rc, &writer);
        }
    }
    if (opts.blob_uri_dictionary) {
        rc = nano_lance_writer_set_blob_uri_dictionary(&writer, true);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_writer_set_blob_uri_dictionary", rc, &writer);
        }
    }
    if (opts.ignore_nullability) {
        rc = nano_lance_writer_set_ignore_nullability(&writer, true);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_writer_set_ignore_nullability", rc, &writer);
        }
    }

    BatchIterator it(table);
    OwnedSchema schema;
    OwnedArray array;
    bool any = false;
    while (it.next(&schema.schema, &array.array)) {
        rc = nano_lance_write_batch(&writer, &array.array, &schema.schema);
        if (rc != NANO_LANCE_OK) {
            throw_lance_writer("nano_lance_write_batch", rc, &writer);
        }
        any = true;
        array.reset();
    }
    if (!any) {
        nano_lance_writer_close(&writer);
        throw std::runtime_error("cannot write empty table with no record batches");
    }

    rc = nano_lance_writer_commit(&writer, opts.append);
    if (rc != NANO_LANCE_OK) {
        throw_lance_writer("nano_lance_writer_commit", rc, &writer);
    }
    rc = nano_lance_writer_close(&writer);
    if (rc != NANO_LANCE_OK) {
        throw_lance_writer("nano_lance_writer_close", rc, &writer);
    }
}

ExportedTable read_table(const std::filesystem::path& path) {
    ArrowSchema schema{};
    ArrowArray* batches = nullptr;
    std::size_t batch_count = 0;
    char err[512] = {};
    int rc = nano_lance_table_read_dataset(path.string().c_str(), &schema, &batches, &batch_count, err,
                                           sizeof(err));
    if (rc != NANO_LANCE_READER_OK) {
        throw_lance_reader("nano_lance_table_read_dataset", rc, err);
    }
    ExportedTable out = ExportedTable::from_read_result(&schema, batches, batch_count);
    nano_lance_table_read_result_free(&schema, batches, batch_count);
    return out;
}

}  // namespace

NB_MODULE(_nanolance, m) {
    m.doc() = "nanolance: fast zero-copy Arrow <-> Lance reader/writer";

    nb::class_<LanceWriterOptions>(m, "WriteOptions")
        .def(nb::init<>())
        .def_rw("compression_level", &LanceWriterOptions::compression_level)
        .def_rw("compression", &LanceWriterOptions::compression)
        .def_rw("blob_uri_dictionary", &LanceWriterOptions::blob_uri_dictionary)
        .def_rw("ignore_nullability", &LanceWriterOptions::ignore_nullability)
        .def_rw("append", &LanceWriterOptions::append);

    m.def(
        "write_table",
        [](nb::handle table, const std::filesystem::path& path, const LanceWriterOptions& opts) {
            write_table(table, path, opts);
        },
        nb::arg("table"), nb::arg("path"), nb::arg("options") = LanceWriterOptions{},
        "Write an Arrow table to a Lance dataset directory.");

    nb::class_<ExportedTable>(m, "LanceTable")
        .def("__arrow_c_stream__", &ExportedTable::arrow_c_stream, nb::arg("requested_schema") = nb::none(),
             "Arrow PyCapsule stream export (zero-copy into pyarrow/polars).")
        .def("__arrow_c_array_stream__", &ExportedTable::arrow_c_stream,
             nb::arg("requested_schema") = nb::none());

    m.def("read_table", &read_table, nb::arg("path"),
          "Read a nanolance-written Lance dataset as an Arrow-exportable handle.");
}
