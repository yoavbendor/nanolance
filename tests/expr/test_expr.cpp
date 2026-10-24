// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The SQL expression engine (nanolance/expr.hpp): parsing, SQL's three-valued logic, coercions and
// the values an update writes, on a small batch built here. The end-to-end behavior -- the same
// rows as pylance for the same filter -- is checked in bindings/python/tests/test_pylance_compat.py.

#include "nanolance/expr.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using nano_lance::expr::Expression;

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

// id int64 [0..7], name utf8 (null at 3), x double, d date32 (2024-01-01 + id), s struct<a int32>
struct Batch {
    ArrowSchema schema{};
    ArrowArray array{};
    ~Batch() {
        array.release(&array);
        schema.release(&schema);
    }
};

void build(Batch& b) {
    ArrowSchemaInit(&b.schema);
    require(ArrowSchemaSetTypeStruct(&b.schema, 5) == NANOARROW_OK, "schema");
    ArrowSchemaSetType(b.schema.children[0], NANOARROW_TYPE_INT64);
    ArrowSchemaSetName(b.schema.children[0], "id");
    ArrowSchemaSetType(b.schema.children[1], NANOARROW_TYPE_STRING);
    ArrowSchemaSetName(b.schema.children[1], "Name");
    ArrowSchemaSetType(b.schema.children[2], NANOARROW_TYPE_DOUBLE);
    ArrowSchemaSetName(b.schema.children[2], "x");
    ArrowSchemaSetType(b.schema.children[3], NANOARROW_TYPE_DATE32);
    ArrowSchemaSetName(b.schema.children[3], "d");
    ArrowSchemaSetTypeStruct(b.schema.children[4], 1);
    ArrowSchemaSetName(b.schema.children[4], "s");
    ArrowSchemaSetType(b.schema.children[4]->children[0], NANOARROW_TYPE_INT32);
    ArrowSchemaSetName(b.schema.children[4]->children[0], "a");
    require(ArrowArrayInitFromSchema(&b.array, &b.schema, nullptr) == NANOARROW_OK, "array");
    require(ArrowArrayStartAppending(&b.array) == NANOARROW_OK, "start");
    const char* names[] = {"alice", "bob", "carol", nullptr, "dave", "Eve", "al%", "frank"};
    for (int i = 0; i < 8; ++i) {
        ArrowArrayAppendInt(b.array.children[0], i);
        if (names[i] == nullptr) {
            ArrowArrayAppendNull(b.array.children[1], 1);
        } else {
            ArrowArrayAppendString(b.array.children[1], ArrowCharView(names[i]));
        }
        ArrowArrayAppendDouble(b.array.children[2], i * 0.5);
        ArrowArrayAppendInt(b.array.children[3], 19723 + i);  // 2024-01-01
        ArrowArrayAppendInt(b.array.children[4]->children[0], i * 10);
        ArrowArrayFinishElement(b.array.children[4]);
        ArrowArrayFinishElement(&b.array);
    }
    require(ArrowArrayFinishBuildingDefault(&b.array, nullptr) == NANOARROW_OK, "finish");
}

std::vector<int> rows(const Batch& b, const std::string& sql) {
    Expression e;
    std::string error;
    require(Expression::parse(sql, e, error), "parse '" + sql + "': " + error);
    require(e.bind(b.schema, error), "bind '" + sql + "': " + error);
    std::vector<std::uint8_t> keep;
    require(e.filter(b.array, keep, error), "filter '" + sql + "': " + error);
    std::vector<int> out;
    for (std::size_t i = 0; i < keep.size(); ++i) {
        if (keep[i] != 0U) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

void expect(const Batch& b, const std::string& sql, std::vector<int> want) {
    const auto got = rows(b, sql);
    std::string g;
    for (const auto r : got) {
        g += std::to_string(r) + " ";
    }
    require(got == want, "'" + sql + "' selected [" + g + "]");
}

void expect_error(const Batch& b, const std::string& sql, const std::string& part) {
    Expression e;
    std::string error;
    bool ok = Expression::parse(sql, e, error) && e.bind(b.schema, error);
    if (ok) {
        std::vector<std::uint8_t> keep;
        ok = e.filter(b.array, keep, error);
    }
    require(!ok, "'" + sql + "' should fail");
    require(error.find(part) != std::string::npos, "'" + sql + "' failed with '" + error + "', not '" + part + "'");
}

}  // namespace

int main() {
    Batch b;
    build(b);

    // Comparison, arithmetic, precedence.
    expect(b, "id > 5", {6, 7});
    expect(b, "id == 2 OR id = 3", {2, 3});
    expect(b, "id <> 0 AND id != 1 AND id < 4", {2, 3});
    expect(b, "x * 2 >= 7", {7});
    expect(b, "id % 3 = 0", {0, 3, 6});
    expect(b, "-id > -2", {0, 1});
    expect(b, "id + 1 = 2 * 2", {3});
    expect(b, "NOT (id > 1)", {0, 1});

    // NULL: three-valued logic. `Name = 'x'` is NULL on row 3, which a filter drops, and NOT NULL
    // is still NULL.
    expect(b, "Name IS NULL", {3});
    expect(b, "Name IS NOT NULL AND id < 2", {0, 1});
    expect(b, "NOT (Name = 'alice')", {1, 2, 4, 5, 6, 7});
    expect(b, "Name = 'alice' OR id = 3", {0, 3});
    expect(b, "Name IN ('bob', NULL)", {1});
    expect(b, "Name NOT IN ('bob', NULL)", {});
    expect(b, "(Name = 'zz') IS NOT TRUE", {0, 1, 2, 3, 4, 5, 6, 7});

    // IN, BETWEEN, LIKE.
    expect(b, "id IN (1, 4, 99)", {1, 4});
    expect(b, "id NOT IN (1, 4)", {0, 2, 3, 5, 6, 7});
    expect(b, "id BETWEEN 2 AND 4", {2, 3, 4});
    expect(b, "id NOT BETWEEN 2 AND 6", {0, 1, 7});
    expect(b, "Name LIKE 'a%'", {0, 6});
    expect(b, "Name LIKE '_ob'", {1});
    expect(b, "Name LIKE 'al\\%'", {6});
    expect(b, "Name ILIKE 'e%'", {5});
    expect(b, "Name NOT LIKE '%a%'", {1, 5});

    // Functions and quoting: a column named with a capital, quoted or not.
    expect(b, "lower(Name) = 'eve'", {5});
    expect(b, "\"Name\" = 'bob'", {1});
    expect(b, "`Name` = 'bob'", {1});
    expect(b, "name = 'bob'", {1});  // case-insensitive when unambiguous
    expect(b, "starts_with(Name, 'da')", {4});
    expect(b, "length(Name) = 3", {1, 5, 6});
    expect(b, "coalesce(Name, 'none') = 'none'", {3});
    expect(b, "abs(id - 5) < 2", {4, 5, 6});

    // Dates: a DATE literal, and a string compared with a date column.
    expect(b, "d = DATE '2024-01-03'", {2});
    expect(b, "d >= '2024-01-07'", {6, 7});
    expect(b, "d < TIMESTAMP '2024-01-02 12:00:00'", {0, 1});

    // Struct fields.
    expect(b, "s.a >= 60", {6, 7});

    // CAST.
    expect(b, "CAST(id AS VARCHAR) = '4'", {4});
    expect(b, "CAST('3' AS BIGINT) = id", {3});

    // Errors name what is wrong.
    expect_error(b, "nope > 1", "not found");
    expect_error(b, "id >", "invalid filter");
    expect_error(b, "id > 'x'", "cannot compare");
    expect_error(b, "frobnicate(id)", "not supported");
    expect_error(b, "id / 0 > 1", "division by zero");
    expect_error(b, "id", "boolean");

    // Values: what an update writes, cast to the column's type.
    {
        Expression e;
        std::string error;
        require(Expression::parse("id * 2 + 1", e, error) && e.bind(b.schema, error), error);
        ArrowSchema type{};
        ArrowSchemaInit(&type);
        ArrowSchemaSetType(&type, NANOARROW_TYPE_INT32);
        ArrowArray out{};
        require(e.evaluate(b.array, type, out, error), "evaluate: " + error);
        require(out.length == 8, "evaluate length");
        const auto* v = static_cast<const std::int32_t*>(out.buffers[1]);
        require(v[0] == 1 && v[7] == 15, "evaluate values");
        out.release(&out);
        type.release(&type);
    }
    {
        Expression e;
        std::string error;
        require(Expression::parse("[1.5, 2.5]", e, error) && e.bind(b.schema, error), error);
        ArrowSchema type{};
        ArrowSchemaInit(&type);
        ArrowSchemaSetTypeFixedSize(&type, NANOARROW_TYPE_FIXED_SIZE_LIST, 2);
        ArrowSchemaSetType(type.children[0], NANOARROW_TYPE_FLOAT);
        ArrowArray out{};
        require(e.evaluate(b.array, type, out, error), "evaluate list: " + error);
        require(out.length == 8 && out.children[0]->length == 16, "list lengths");
        out.release(&out);
        type.release(&type);
    }
    {
        Expression e;
        std::string error;
        require(Expression::parse("X'616263'", e, error) && e.bind(b.schema, error), error);
        ArrowSchema type{};
        ArrowSchemaInit(&type);
        ArrowSchemaSetType(&type, NANOARROW_TYPE_BINARY);
        ArrowArray out{};
        require(e.evaluate(b.array, type, out, error), "evaluate binary: " + error);
        const auto* offsets = static_cast<const std::int32_t*>(out.buffers[1]);
        require(offsets[1] - offsets[0] == 3, "binary literal is 3 bytes");
        out.release(&out);
        type.release(&type);
    }

    std::cout << "expr: all checks passed\n";
    return 0;
}
