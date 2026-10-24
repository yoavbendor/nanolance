// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The text of an FTS document (index/fts/fts_document_text.hpp), in memory: a column's kind from its schema
// and its array view (string, list of strings, neither); a row's document -- a string as it is, a list's
// non-null elements joined by one space (Lance's materialize_string_list), null for a null row -- also
// through a sliced array; and a text column below struct fields, whose document is null where a struct
// above it is null.

#include "index/fts/fts_document_text.hpp"

#include "index_test_util.hpp"

#include <nanoarrow/nanoarrow.h>

#include <string>
#include <string_view>
#include <vector>

using index_test::append_string;
using index_test::require;
namespace fts = nano_lance::fts;

namespace {

struct View {
    ArrowArrayView view{};
    View(const ArrowSchema& schema, const ArrowArray& array) {
        ArrowError err{};
        require(ArrowArrayViewInitFromSchema(&view, &schema, &err) == NANOARROW_OK, "view init");
        require(ArrowArrayViewSetArray(&view, &array, &err) == NANOARROW_OK, std::string("view set: ") + err.message);
    }
    ~View() { ArrowArrayViewReset(&view); }
};

std::string doc(const ArrowArrayView* v, fts::TextColumn kind, std::int64_t row, bool& present) {
    std::string joined;
    std::string_view text;
    present = fts::row_text(v, kind, row, joined, text);
    return present ? std::string(text) : std::string();
}

}  // namespace

int main() {
    // A string column: kind, documents, a null row.
    {
        ArrowSchema schema{};
        ArrowSchemaInit(&schema);
        require(ArrowSchemaSetType(&schema, NANOARROW_TYPE_STRING) == NANOARROW_OK, "string schema");
        require(fts::text_schema_kind(&schema) == fts::TextColumn::kString, "a utf8 schema is a string column");
        ArrowArray array{};
        require(ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK, "array");
        require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "append");
        append_string(&array, "hello world");
        require(ArrowArrayAppendNull(&array, 1) == NANOARROW_OK, "null");
        append_string(&array, "");
        require(ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK, "finish");
        View v(schema, array);
        require(fts::text_column_kind(&v.view) == fts::TextColumn::kString, "a string view is a string column");
        bool present = false;
        require(doc(&v.view, fts::TextColumn::kString, 0, present) == "hello world" && present, "row 0's text");
        doc(&v.view, fts::TextColumn::kString, 1, present);
        require(!present, "a null row has no document");
        require(doc(&v.view, fts::TextColumn::kString, 2, present).empty() && present, "an empty string is a document");
        ArrowArrayRelease(&array);
        ArrowSchemaRelease(&schema);
    }

    // A list<utf8> column: elements joined by one space, null elements skipped, a null list null, an
    // empty list an empty document; and a slice (offset 1) gives the same rows.
    {
        ArrowSchema schema{};
        ArrowSchemaInit(&schema);
        require(ArrowSchemaSetType(&schema, NANOARROW_TYPE_LIST) == NANOARROW_OK, "list schema");
        require(ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_STRING) == NANOARROW_OK, "item");
        require(fts::text_schema_kind(&schema) == fts::TextColumn::kStringList, "list<utf8> is a string list");
        ArrowArray array{};
        require(ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK, "array");
        require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "append");
        append_string(array.children[0], "red");
        require(ArrowArrayAppendNull(array.children[0], 1) == NANOARROW_OK, "null element");
        append_string(array.children[0], "fox");
        append_string(array.children[0], "");
        append_string(array.children[0], "jumps");
        require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "row 0");
        require(ArrowArrayAppendNull(&array, 1) == NANOARROW_OK, "row 1: null list");
        require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "row 2: empty list");
        append_string(array.children[0], "one");
        require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "row 3");
        require(ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK, "finish");
        View v(schema, array);
        require(fts::text_column_kind(&v.view) == fts::TextColumn::kStringList, "the view is a string list");
        bool present = false;
        // An empty element still adds its separator (Lance joins the non-null elements).
        require(doc(&v.view, fts::TextColumn::kStringList, 0, present) == "red fox  jumps" && present,
                "the elements joined by one space, the null left out");
        doc(&v.view, fts::TextColumn::kStringList, 1, present);
        require(!present, "a null list has no document");
        require(doc(&v.view, fts::TextColumn::kStringList, 2, present).empty() && present, "an empty list is empty");
        require(doc(&v.view, fts::TextColumn::kStringList, 3, present) == "one", "a one-element list");

        array.offset = 3;
        array.length = 1;
        View sliced(schema, array);
        require(doc(&sliced.view, fts::TextColumn::kStringList, 0, present) == "one" && present,
                "a sliced list reads its own row");
        array.offset = 0;
        array.length = 4;
        ArrowArrayRelease(&array);
        ArrowSchemaRelease(&schema);
    }

    // Neither: an int column, and a list of ints.
    {
        ArrowSchema schema{};
        ArrowSchemaInit(&schema);
        require(ArrowSchemaSetType(&schema, NANOARROW_TYPE_INT64) == NANOARROW_OK, "int schema");
        require(fts::text_schema_kind(&schema) == fts::TextColumn::kNone, "int64 is no text");
        ArrowSchemaRelease(&schema);
        ArrowSchemaInit(&schema);
        require(ArrowSchemaSetType(&schema, NANOARROW_TYPE_LIST) == NANOARROW_OK, "list schema");
        require(ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT32) == NANOARROW_OK, "item");
        require(fts::text_schema_kind(&schema) == fts::TextColumn::kNone, "list<int32> is no text");
        ArrowSchemaRelease(&schema);
        require(fts::text_schema_kind(nullptr) == fts::TextColumn::kNone, "no schema is no text");
    }

    // A text leaf below structs: meta { body { text } }, a null struct making the document null.
    {
        ArrowSchema schema{};
        ArrowSchemaInit(&schema);
        require(ArrowSchemaSetTypeStruct(&schema, 2) == NANOARROW_OK, "meta");
        require(ArrowSchemaSetType(schema.children[0], NANOARROW_TYPE_INT64) == NANOARROW_OK, "meta.n");
        require(ArrowSchemaSetName(schema.children[0], "n") == NANOARROW_OK, "name n");
        ArrowSchema* body = schema.children[1];
        require(ArrowSchemaSetTypeStruct(body, 1) == NANOARROW_OK, "body");
        require(ArrowSchemaSetName(body, "body") == NANOARROW_OK, "name body");
        require(ArrowSchemaSetType(body->children[0], NANOARROW_TYPE_STRING) == NANOARROW_OK, "text");
        require(ArrowSchemaSetName(body->children[0], "text") == NANOARROW_OK, "name text");

        std::vector<std::size_t> index;
        require(fts::text_child_path(&schema, {"body", "text"}, index), "the path fits the schema");
        require((index == std::vector<std::size_t>{1, 0}), "the path's child positions");
        require(!fts::text_child_path(&schema, {"body", "missing"}, index), "a missing name does not fit");
        require(fts::text_schema_kind(fts::text_schema_at(&schema, {1, 0})) == fts::TextColumn::kString,
                "the leaf is a string column");
        require(fts::text_schema_at(&schema, {1, 5}) == nullptr, "an index past the children is null");

        ArrowArray array{};
        require(ArrowArrayInitFromSchema(&array, &schema, nullptr) == NANOARROW_OK, "array");
        require(ArrowArrayStartAppending(&array) == NANOARROW_OK, "append");
        // Row 0: everything present. Row 1: body null. Row 2: meta itself null.
        require(ArrowArrayAppendInt(array.children[0], 1) == NANOARROW_OK, "n0");
        append_string(array.children[1]->children[0], "inside");
        require(ArrowArrayFinishElement(array.children[1]) == NANOARROW_OK, "body0");
        require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "meta0");
        require(ArrowArrayAppendInt(array.children[0], 2) == NANOARROW_OK, "n1");
        require(ArrowArrayAppendNull(array.children[1], 1) == NANOARROW_OK, "body1 null");
        require(ArrowArrayFinishElement(&array) == NANOARROW_OK, "meta1");
        require(ArrowArrayAppendNull(&array, 1) == NANOARROW_OK, "meta2 null");
        require(ArrowArrayFinishBuildingDefault(&array, nullptr) == NANOARROW_OK, "finish");
        View v(schema, array);
        std::vector<const ArrowArrayView*> above;
        const ArrowArrayView* leaf = fts::text_view_at(&v.view, {1, 0}, above);
        require(leaf != nullptr && above.size() == 2U, "the leaf view and the two structs above it");
        bool present = false;
        require(!fts::null_above(above, 0) && doc(leaf, fts::TextColumn::kString, 0, present) == "inside",
                "row 0 has its text");
        require(fts::null_above(above, 1), "row 1's body is null");
        require(fts::null_above(above, 2), "row 2's meta is null");
        ArrowArrayRelease(&array);
        ArrowSchemaRelease(&schema);
    }

    std::cout << "fts_document_text: ok (string, list and nested text documents)\n";
    return 0;
}
