// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// writer_internal.hpp: the hooks nanolance's own operations use on a staged C writer. A staged writer
// (stage_fragments) numbers its fields from a chosen base, reports each data file as it is buffered and written,
// and hands its files over (writer_take_staged) instead of publishing a version: no manifest is written. On an
// append writer, writer_project_append writes part of the schema with the dataset's field ids. Refusals: the
// hooks on a writer that is not staged, a field id base after the first batch, a projection naming a column
// the dataset lacks.

#include "nanolance/writer_internal.hpp"

#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

namespace nl = nano_lance;

namespace {

void require(bool ok, const std::string& msg) {
    if (!ok) {
        std::cerr << "FAIL: " << msg << '\n';
        std::exit(1);
    }
}

std::filesystem::path scratch(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

/// Columns "a" and "b", both int64, holding `values`.
void write_ab(NanoLanceWriter* writer, const std::vector<std::int64_t>& values, bool only_a = false) {
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    const int n = only_a ? 1 : 2;
    require(ArrowSchemaSetTypeStruct(&schema, n) == NANOARROW_OK, "struct schema");
    const char* names[2] = {"a", "b"};
    for (int i = 0; i < n; ++i) {
        require(ArrowSchemaSetType(schema.children[i], NANOARROW_TYPE_INT64) == NANOARROW_OK &&
                    ArrowSchemaSetName(schema.children[i], names[i]) == NANOARROW_OK,
                "child");
    }
    ArrowArray array{};
    require(ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK, "array");
    require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "start");
    for (const auto v : values) {
        for (int i = 0; i < n; ++i) {
            require(ArrowArrayAppendInt(array.children[i], v) == NANOARROW_OK, "append");
        }
        require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "element");
    }
    require(ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK, "finish");
    require(nano_lance_write_batch(writer, &array, &schema) == NANO_LANCE_OK,
            std::string("write_batch: ") + nano_lance_writer_last_error(writer));
    ArrowArrayRelease(&array);
    ArrowSchemaRelease(&schema);
}

}  // namespace

int main() {
    std::string error;

    // A writer that is not staged takes none of the staged hooks.
    {
        const auto path = scratch("nano_lance_writer_internal_plain");
        NanoLanceWriter w{};
        require(nano_lance_writer_open(&w, path.string().c_str(), nullptr) == NANO_LANCE_OK, "open plain");
        std::vector<nl::NewFragment> staged;
        nl::LanceSchemaMapping mapping;
        require(!nl::writer_take_staged(&w, staged, mapping, error), "take_staged needs a staged writer");
        require(!nl::writer_set_file_events(&w, [](int, const std::string&, std::uint64_t) {}, error),
                "file events need a staged writer");
        require(!nl::writer_project_append(&w, {"a"}, error), "project_append needs an append writer");
        nano_lance_writer_close(&w);
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    // A staged writer: field ids from 10, file events, the files handed over, nothing published.
    const auto path = scratch("nano_lance_writer_internal_staged");
    {
        NanoLanceWriteOptions options{};
        options.stage_fragments = true;
        NanoLanceWriter w{};
        require(nano_lance_writer_open(&w, path.string().c_str(), &options) == NANO_LANCE_OK, "open staged");
        require(nl::writer_set_field_id_base(&w, 10, error), "field id base: " + error);
        std::vector<std::tuple<int, std::string, std::uint64_t>> events;
        require(nl::writer_set_file_events(
                    &w, [&](int kind, const std::string& name, std::uint64_t rows) { events.emplace_back(kind, name, rows); },
                    error),
                "file events: " + error);
        write_ab(&w, {1, 2, 3});
        require(!nl::writer_set_field_id_base(&w, 20, error), "the base is refused after the first batch");
        std::vector<nl::NewFragment> staged;
        nl::LanceSchemaMapping mapping;
        require(nl::writer_take_staged(&w, staged, mapping, error), "take_staged: " + error);
        require(staged.size() == 1 && staged[0].rows == 3, "one staged file of three rows");
        require(std::filesystem::exists(path / "data" / staged[0].data_file.relative_path.filename()) ||
                    std::filesystem::exists(path / staged[0].data_file.relative_path),
                "the data file is written");
        require(mapping.fields.size() == 2 && mapping.fields[0].name == "a" && mapping.fields[0].id == 10 &&
                    mapping.fields[1].id == 11,
                "the fields are numbered from the base");
        require(!events.empty() && std::get<0>(events.front()) == 0 && std::get<0>(events.back()) == 2 &&
                    std::get<2>(events.back()) == 3,
                "a buffered event, then a written one of three rows");
        require(std::get<1>(events.front()) == std::get<1>(events.back()), "both events name the same file");
        require(!std::filesystem::exists(path / "_versions") || std::filesystem::is_empty(path / "_versions"),
                "a staged write publishes no version");
        nano_lance_writer_close(&w);
    }

    // An append writer writing part of the schema keeps the dataset's field ids.
    {
        const auto ds = scratch("nano_lance_writer_internal_append");
        NanoLanceWriter w{};
        require(nano_lance_writer_init(&w, ds.string().c_str(), 0) == NANO_LANCE_OK, "init");
        write_ab(&w, {1, 2});
        require(nano_lance_writer_commit(&w, false) == NANO_LANCE_OK, "commit");
        nano_lance_writer_close(&w);

        NanoLanceWriteOptions options{};
        options.append = true;
        options.stage_fragments = true;
        NanoLanceWriter a{};
        require(nano_lance_writer_open(&a, ds.string().c_str(), &options) == NANO_LANCE_OK, "open append");
        require(!nl::writer_project_append(&a, {"nope"}, error), "a column the dataset lacks is refused");
        require(nl::writer_project_append(&a, {"b"}, error), "project_append: " + error);
        std::vector<nl::NewFragment> staged;
        nl::LanceSchemaMapping mapping;
        require(nl::writer_take_staged(&a, staged, mapping, error, true) && staged.empty(),
                "nothing written, nothing staged");
        require(mapping.fields.size() == 1 && mapping.fields[0].name == "b" && mapping.fields[0].id == 1,
                "only b, with its id in the dataset");
        nano_lance_writer_close(&a);
        std::error_code ec;
        std::filesystem::remove_all(ds, ec);
    }

    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::cout << "writer_internal: OK\n";
    return 0;
}
