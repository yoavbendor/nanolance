// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Utf8View / BinaryView columns converted to string / binary (src/arrow/arrow_views.hpp): inline and out-of-line
// values, several data buffers, a sliced array, nulls, empty and all-null columns, nested columns, and the
// refusal of malformed views. Each converted column is read back with nanoarrow and compared with its input.

#include "arrow/arrow_views.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;
using Values = std::vector<std::optional<std::string>>;

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

// A view column built from its values: short ones inline, long ones copied into the data buffer the caller names.
// The buffers it hands out point into its own vectors, so it is not copied or moved.
struct ViewColumn {
    ViewColumn(std::size_t data_buffers, bool is_binary) : data(data_buffers), binary(is_binary) {}
    ViewColumn(const ViewColumn&) = delete;
    ViewColumn& operator=(const ViewColumn&) = delete;
    ~ViewColumn() {
        if (schema.release != nullptr) {
            schema.release(&schema);
        }
    }

    void add(const std::optional<std::string>& value, std::size_t buffer = 0) {
        Bytes view(16, 0);
        valid.push_back(value.has_value());
        if (value) {
            const auto length = static_cast<std::int32_t>(value->size());
            std::memcpy(view.data(), &length, 4);
            if (length <= 12) {
                std::memcpy(view.data() + 4, value->data(), value->size());
            } else {
                Bytes& target = data.at(buffer);
                const auto offset = static_cast<std::int32_t>(target.size());
                target.insert(target.end(), value->begin(), value->end());
                const auto index = static_cast<std::int32_t>(buffer);
                std::memcpy(view.data() + 4, value->data(), 4);
                std::memcpy(view.data() + 8, &index, 4);
                std::memcpy(view.data() + 12, &offset, 4);
            }
        }
        views.insert(views.end(), view.begin(), view.end());
    }

    // The array over all the values added, seen as rows [offset, offset + length).
    void finish(std::int64_t offset, std::int64_t length) {
        std::int64_t nulls = 0;
        for (std::int64_t i = offset; i < offset + length; ++i) {
            nulls += valid[static_cast<std::size_t>(i)] ? 0 : 1;
        }
        if (nulls > 0) {
            validity.assign((valid.size() + 7) / 8, 0);
            for (std::size_t i = 0; i < valid.size(); ++i) {
                if (valid[i]) {
                    validity[i / 8] |= static_cast<std::uint8_t>(1U << (i % 8));
                }
            }
        }
        sizes.clear();
        buffers.clear();
        buffers.push_back(nulls > 0 ? validity.data() : nullptr);
        buffers.push_back(views.data());
        for (const auto& d : data) {
            buffers.push_back(d.data());
            sizes.push_back(static_cast<std::int64_t>(d.size()));
        }
        buffers.push_back(sizes.data());
        array = ArrowArray{};
        array.length = length;
        array.null_count = nulls;
        array.offset = offset;
        array.n_buffers = static_cast<std::int64_t>(buffers.size());
        array.buffers = buffers.data();
        ArrowSchemaInit(&schema);
        ArrowSchemaSetFormat(&schema, binary ? "vz" : "vu");
        ArrowSchemaSetName(&schema, "s");
    }

    std::vector<bool> valid;
    Bytes views;
    std::vector<Bytes> data;
    bool binary;
    Bytes validity;
    std::vector<std::int64_t> sizes;
    std::vector<const void*> buffers;
    ArrowArray array{};
    ArrowSchema schema{};
};

// Reads a converted (or input) column's values with nanoarrow, which checks the layout too.
Values read_column(const ArrowSchema& schema, const ArrowArray& array) {
    ArrowArrayView view;
    ArrowError error{};
    require(ArrowArrayViewInitFromSchema(&view, &schema, &error) == NANOARROW_OK,
            std::string("view init: ") + error.message);
    require(ArrowArrayViewSetArray(&view, &array, &error) == NANOARROW_OK,
            std::string("view set: ") + error.message);
    Values out;
    for (std::int64_t i = 0; i < array.length; ++i) {
        if (ArrowArrayViewIsNull(&view, i)) {
            out.emplace_back(std::nullopt);
            continue;
        }
        const ArrowBufferView bytes = ArrowArrayViewGetBytesUnsafe(&view, i);
        out.emplace_back(std::string(bytes.data.as_char, static_cast<std::size_t>(bytes.size_bytes)));
    }
    ArrowArrayViewReset(&view);
    return out;
}

// A column of the expected values: what the converted column must read back as.
void expect_values(const Values& got, const Values& want, const std::string& what) {
    require(got.size() == want.size(), what + ": row count " + std::to_string(got.size()));
    for (std::size_t i = 0; i < want.size(); ++i) {
        require(got[i] == want[i], what + ": row " + std::to_string(i) + " differs");
    }
}

// A batch that is one struct column named `s` over `column`, with the struct's own offset and length.
struct OneColumnBatch {
    OneColumnBatch(ViewColumn& column, std::int64_t offset, std::int64_t length) {
        ArrowSchemaInit(&schema);
        require(ArrowSchemaSetTypeStruct(&schema, 1) == NANOARROW_OK, "struct schema");
        schema.children[0]->release(schema.children[0]);
        require(ArrowSchemaDeepCopy(&column.schema, schema.children[0]) == NANOARROW_OK, "copy child schema");
        require(ArrowSchemaSetName(schema.children[0], "s") == NANOARROW_OK, "name child schema");
        batch = ArrowArray{};
        batch.length = length;
        batch.offset = offset;
        batch.n_buffers = 1;
        batch.buffers = null_buffer;
        batch.n_children = 1;
        batch.children = kids;
        kids[0] = &column.array;
    }
    ~OneColumnBatch() {
        if (schema.release != nullptr) {
            schema.release(&schema);
        }
    }
    OneColumnBatch(const OneColumnBatch&) = delete;
    OneColumnBatch& operator=(const OneColumnBatch&) = delete;

    ArrowSchema schema{};
    ArrowArray batch{};
    const void* null_buffer[1] = {nullptr};
    ArrowArray* kids[1] = {nullptr};
};

void test_has_view_types() {
    ArrowSchema plain{};
    ArrowSchemaInit(&plain);
    ArrowSchemaSetFormat(&plain, "u");
    require(!nano_lance::arrow_views::has_view_types(plain), "a string column has no view");
    plain.release(&plain);

    ArrowSchema binary_view{};
    ArrowSchemaInit(&binary_view);
    ArrowSchemaSetFormat(&binary_view, "vz");
    require(nano_lance::arrow_views::has_view_types(binary_view), "binary_view is a view");
    binary_view.release(&binary_view);

    // A view nested in a list in a struct is found; a plain list next to it is not a view.
    ArrowSchema root{};
    ArrowSchemaInit(&root);
    require(ArrowSchemaSetTypeStruct(&root, 2) == NANOARROW_OK, "struct");
    ArrowSchemaInit(root.children[0]);
    ArrowSchemaSetFormat(root.children[0], "+l");
    require(ArrowSchemaAllocateChildren(root.children[0], 1) == NANOARROW_OK, "list child");
    ArrowSchemaInit(root.children[0]->children[0]);
    ArrowSchemaSetFormat(root.children[0]->children[0], "u");
    ArrowSchemaInit(root.children[1]);
    ArrowSchemaSetFormat(root.children[1], "l");
    require(!nano_lance::arrow_views::has_view_types(root), "a list of string is not a view type");
    ArrowSchemaSetFormat(root.children[0]->children[0], "vu");
    require(nano_lance::arrow_views::has_view_types(root), "a list of string_view is a view type");
    root.release(&root);
}

// The values of the mixed column: nulls, inline (up to 12 bytes), out-of-line values in two data buffers.
void add_mixed(ViewColumn& col) {
    col.add(std::string("abc"));
    col.add(std::nullopt);
    col.add(std::string("exactly12chr"));          // 12 bytes: inline
    col.add(std::string("thirteen bytes"), 0);     // 14 bytes: out of line, buffer 0
    col.add(std::string("a longer value in buffer one"), 1);
    col.add(std::string(""));
    col.add(std::string("second in buffer zero, still long"), 0);
    col.add(std::string("tail"), 1);
}

Values mixed_values() {
    return {std::string("abc"), std::nullopt, std::string("exactly12chr"), std::string("thirteen bytes"),
            std::string("a longer value in buffer one"), std::string(""),
            std::string("second in buffer zero, still long"), std::string("tail")};
}

void test_string_view_values() {
    ViewColumn col(2, false);
    add_mixed(col);
    col.finish(0, 8);
    nano_lance::arrow_views::ViewlessBatch batch;
    std::string error;
    OneColumnBatch input(col, 0, 8);
    require(nano_lance::arrow_views::has_view_types(input.schema), "the batch has a view column");
    require(batch.convert(input.schema, input.batch, error), "convert: " + error);
    const ArrowSchema& child = *batch.schema()->children[0];
    require(std::string(child.format) == "u", "a string_view becomes string, got " + std::string(child.format));
    require(std::string(child.name) == "s", "the column keeps its name");
    require(batch.array()->length == 8 && batch.array()->offset == 0, "the batch keeps its length and offset 0");
    expect_values(read_column(child, *batch.array()->children[0]), mixed_values(), "string_view values");
    require(batch.array()->children[0]->null_count == 1, "one null");
    require(!nano_lance::arrow_views::has_view_types(*batch.schema()), "no view left in the copy");
}

void test_offset_and_sliced_array() {
    // A sliced array: the input's rows 2..5 (offset 2, length 4) are the output's rows 0..3.
    ViewColumn col(2, false);
    add_mixed(col);
    col.finish(2, 4);
    OneColumnBatch input(col, 0, 4);
    nano_lance::arrow_views::ViewlessBatch batch;
    std::string error;
    require(batch.convert(input.schema, input.batch, error), "convert sliced: " + error);
    const Values all = mixed_values();
    const Values want(all.begin() + 2, all.begin() + 6);
    expect_values(read_column(*batch.schema()->children[0], *batch.array()->children[0]), want, "sliced view");
    require(batch.array()->children[0]->offset == 0, "the converted column has offset 0");
}

void test_binary_view_bytes() {
    ViewColumn col(1, true);
    const std::string zeros(20, '\0');
    col.add(std::string("\x00\x01\xff", 3));
    col.add(std::nullopt);
    col.add(zeros);
    col.finish(0, 3);
    OneColumnBatch input(col, 0, 3);
    nano_lance::arrow_views::ViewlessBatch batch;
    std::string error;
    require(batch.convert(input.schema, input.batch, error), "convert binary: " + error);
    require(std::string(batch.schema()->children[0]->format) == "z", "a binary_view becomes binary");
    expect_values(read_column(*batch.schema()->children[0], *batch.array()->children[0]),
                  {std::string("\x00\x01\xff", 3), std::nullopt, zeros}, "binary_view values");
}

void test_empty_and_all_null() {
    ViewColumn empty(0, false);
    empty.finish(0, 0);
    OneColumnBatch input_empty(empty, 0, 0);
    nano_lance::arrow_views::ViewlessBatch batch_empty;
    std::string error;
    require(batch_empty.convert(input_empty.schema, input_empty.batch, error), "convert empty: " + error);
    require(batch_empty.array()->children[0]->length == 0, "an empty column stays empty");

    ViewColumn nulls(1, false);
    nulls.add(std::nullopt);
    nulls.add(std::nullopt);
    nulls.add(std::nullopt);
    nulls.finish(0, 3);
    OneColumnBatch input_nulls(nulls, 0, 3);
    nano_lance::arrow_views::ViewlessBatch batch_nulls;
    require(batch_nulls.convert(input_nulls.schema, input_nulls.batch, error), "convert nulls: " + error);
    const ArrowArray& out = *batch_nulls.array()->children[0];
    require(out.null_count == 3 && out.length == 3, "all three rows are null");
    expect_values(read_column(*batch_nulls.schema()->children[0], out), {std::nullopt, std::nullopt, std::nullopt},
                  "all-null values");
}

void test_plain_columns_are_shells() {
    // A struct with a plain string next to a view: the plain child is not copied, it points at the input's buffers.
    ViewColumn view(2, false);
    add_mixed(view);
    view.finish(0, 8);
    const std::string long_value = "plain value that is longer than twelve";
    const std::int32_t offsets[2] = {0, static_cast<std::int32_t>(long_value.size())};
    const Bytes plain_data(long_value.begin(), long_value.end());
    const void* plain_buffers[3] = {nullptr, offsets, plain_data.data()};
    ArrowArray plain{};
    plain.length = 1;
    plain.n_buffers = 3;
    plain.buffers = plain_buffers;
    ArrowSchema root{};
    ArrowSchemaInit(&root);
    require(ArrowSchemaSetTypeStruct(&root, 2) == NANOARROW_OK, "two-field struct");
    root.children[0]->release(root.children[0]);
    require(ArrowSchemaDeepCopy(&view.schema, root.children[0]) == NANOARROW_OK, "copy view schema");
    root.children[1]->release(root.children[1]);
    ArrowSchemaInit(root.children[1]);
    ArrowSchemaSetFormat(root.children[1], "u");
    ArrowSchemaSetName(root.children[1], "p");
    const void* batch_null[1] = {nullptr};
    ArrowArray* kids[2] = {&view.array, &plain};
    ArrowArray batch{};
    batch.length = 1;
    batch.n_buffers = 1;
    batch.buffers = batch_null;
    batch.n_children = 2;
    batch.children = kids;
    nano_lance::arrow_views::ViewlessBatch converted;
    std::string error;
    require(converted.convert(root, batch, error), "convert mixed struct: " + error);
    require(converted.array()->children[1]->buffers[1] == plain.buffers[1],
            "a plain column points at the caller's buffers");
    require(std::string(converted.schema()->children[1]->format) == "u", "a plain string keeps its format");
    require(converted.array()->children[1]->length == 1, "the plain child keeps its length");
    root.release(&root);
}

void test_struct_offset_keeps_child_indexing() {
    // A struct sliced at offset 1: its children are not sliced with it, so the child's rows keep their indices.
    ViewColumn child(1, false);
    child.add(std::string("a"));
    child.add(std::string("bbbbbbbbbbbbbbbbbbbbbbbbb"));
    child.add(std::nullopt);
    child.finish(0, 3);
    OneColumnBatch input(child, 1, 2);
    nano_lance::arrow_views::ViewlessBatch batch;
    std::string error;
    require(batch.convert(input.schema, input.batch, error), "convert struct offset: " + error);
    require(batch.array()->offset == 1 && batch.array()->length == 2, "the struct keeps its offset and length");
    expect_values(read_column(*batch.schema()->children[0], *batch.array()->children[0]),
                  {std::string("a"), std::string("bbbbbbbbbbbbbbbbbbbbbbbbb"), std::nullopt}, "struct child");
}

void test_list_of_views() {
    // list<string_view>: rows [a], [], [b, null]; the child holds the three values.
    ViewColumn child(1, false);
    child.add(std::string("first value over twelve bytes"), 0);
    child.add(std::string("second"));
    child.add(std::nullopt);
    child.finish(0, 3);
    const std::int32_t offsets[4] = {0, 1, 1, 3};
    const void* list_buffers[2] = {nullptr, offsets};
    ArrowArray* kids[1] = {&child.array};
    ArrowArray list{};
    list.length = 3;
    list.n_buffers = 2;
    list.buffers = list_buffers;
    list.n_children = 1;
    list.children = kids;
    ArrowSchema list_schema{};
    ArrowSchemaInit(&list_schema);
    ArrowSchemaSetFormat(&list_schema, "+l");
    require(ArrowSchemaAllocateChildren(&list_schema, 1) == NANOARROW_OK, "list schema");
    require(ArrowSchemaDeepCopy(&child.schema, list_schema.children[0]) == NANOARROW_OK, "copy item schema");
    ArrowSchemaSetName(list_schema.children[0], "item");

    nano_lance::arrow_views::ViewlessBatch batch;
    std::string error;
    require(batch.convert(list_schema, list, error), "convert list: " + error);
    expect_values(read_column(*batch.schema()->children[0], *batch.array()->children[0]),
                  {std::string("first value over twelve bytes"), std::string("second"), std::nullopt}, "list items");
    require(batch.array()->children[0]->length == 3 && batch.array()->children[0]->offset == 0,
            "the item array keeps its length");
    require(std::string(batch.schema()->format) == "+l", "the list keeps its format");
    list_schema.release(&list_schema);
}

void expect_refused(const ArrowArray& column, const std::string& what) {
    ArrowSchema schema{};
    ArrowSchemaInit(&schema);
    ArrowSchemaSetFormat(&schema, "vu");
    ArrowSchemaSetName(&schema, "s");
    ArrowSchema root{};
    ArrowSchemaInit(&root);
    require(ArrowSchemaSetTypeStruct(&root, 1) == NANOARROW_OK, "root");
    root.children[0]->release(root.children[0]);
    require(ArrowSchemaDeepCopy(&schema, root.children[0]) == NANOARROW_OK, "copy");
    const void* null_buffer[1] = {nullptr};
    ArrowArray* kids[1] = {const_cast<ArrowArray*>(&column)};
    ArrowArray batch{};
    batch.length = column.length;
    batch.n_buffers = 1;
    batch.buffers = null_buffer;
    batch.n_children = 1;
    batch.children = kids;
    nano_lance::arrow_views::ViewlessBatch converted;
    std::string error;
    require(!converted.convert(root, batch, error), what + " is refused");
    require(!error.empty(), what + " names the problem");
    root.release(&root);
    schema.release(&schema);
}

void test_malformed_views_are_refused() {
    ViewColumn bad_index(1, false);
    bad_index.add(std::string("this value points at buffer one"), 0);
    bad_index.finish(0, 1);
    // Point the view at a data buffer that does not exist.
    const std::int32_t index = 5;
    std::memcpy(bad_index.views.data() + 8, &index, 4);
    expect_refused(bad_index.array, "a view with a missing data buffer");

    ViewColumn past_end(1, false);
    past_end.add(std::string("a value of twenty-two b"), 0);
    past_end.finish(0, 1);
    // The sizes buffer says the data buffer is shorter than the view reads it.
    past_end.sizes[0] = 4;
    expect_refused(past_end.array, "a view that runs past its data buffer");
}

void test_view_column_is_a_copy_not_a_borrow() {
    // The converted column owns its bytes: releasing the input after the conversion leaves it readable. Checked by
    // converting a batch and reading the copy after the input's data has been overwritten.
    ViewColumn col(2, false);
    add_mixed(col);
    col.finish(0, 8);
    OneColumnBatch input(col, 0, 8);
    nano_lance::arrow_views::ViewlessBatch batch;
    std::string error;
    require(batch.convert(input.schema, input.batch, error), "convert: " + error);
    for (auto& d : col.data) {
        std::fill(d.begin(), d.end(), static_cast<std::uint8_t>('?'));
    }
    expect_values(read_column(*batch.schema()->children[0], *batch.array()->children[0]), mixed_values(),
                  "the copy survives the input's data buffers");
}

}  // namespace

int main() {
    test_has_view_types();
    test_string_view_values();
    test_offset_and_sliced_array();
    test_binary_view_bytes();
    test_empty_and_all_null();
    test_plain_columns_are_shells();
    test_struct_offset_keeps_child_indexing();
    test_list_of_views();
    test_malformed_views_are_refused();
    test_view_column_is_a_copy_not_a_borrow();
    std::cout << "arrow_views: all tests passed\n";
    return 0;
}
