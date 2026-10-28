// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// Layer expr: what a filter means. An SQL expression is parsed, bound to a schema (its column names and types) and then
// run on a batch. No storage is involved, so it works on any Arrow batch. This example filters a batch of two int64
// columns with a WHERE-style predicate and evaluates a value expression.
//
//   - Expression::parse, then bind(schema): parse the SQL text and resolve the column names.
//   - filter: one byte per row, 1 where the predicate is TRUE. NULL and FALSE drop the row, as WHERE does.
//   - evaluate: a value column, cast to the type given.
//
// Run it with: ctest --test-dir build -R nano_lance_example_expr --output-on-failure

#include "nanolance/expr.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int fail(const std::string& message) { std::cerr << "FAIL: " << message << "\n"; return 1; }

// The batch owns no memory, so its structs get a release that does nothing. A struct with no release counts as
// already released, and the readers check for that.
void release_schema(ArrowSchema* schema) { schema->release = nullptr; }
void release_array(ArrowArray* array) { array->release = nullptr; }

// A struct batch of two int64 columns, "id" and "price". The C data interface structs are filled by hand (this
// layer's README does not name nanoarrow). The batch points at the two vectors, which must outlive it.
struct Batch {
    // Every field in the C structs' order (format, name, metadata, flags, n_children, children, dictionary, release,
    // private_data for a schema). Only the release is set; the rest are zero.
    ArrowSchema id_schema{"l", "id", nullptr, 0, 0, nullptr, nullptr, release_schema, nullptr};
    ArrowSchema price_schema{"l", "price", nullptr, 0, 0, nullptr, nullptr, release_schema, nullptr};
    ArrowSchema* schema_children[2] = {&id_schema, &price_schema};
    ArrowSchema schema{"+s", nullptr, nullptr, 0, 2, schema_children, nullptr, release_schema, nullptr};
    const void* id_buffers[2] = {nullptr, nullptr};
    const void* price_buffers[2] = {nullptr, nullptr};
    ArrowArray id_array{0, 0, 0, 2, 0, id_buffers, nullptr, nullptr, release_array, nullptr};
    ArrowArray price_array{0, 0, 0, 2, 0, price_buffers, nullptr, nullptr, release_array, nullptr};
    ArrowArray* array_children[2] = {&id_array, &price_array};
    const void* root_buffers[1] = {nullptr};
    ArrowArray array{0, 0, 0, 1, 2, root_buffers, array_children, nullptr, release_array, nullptr};

    Batch(const std::vector<std::int64_t>& id, const std::vector<std::int64_t>& price) {
        id_buffers[1] = id.data();
        price_buffers[1] = price.data();
        id_array.length = price_array.length = array.length = static_cast<std::int64_t>(id.size());
    }
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
};

}  // namespace

int main() {
    const std::vector<std::int64_t> ids = {1, 2, 3, 4, 5, 6};
    const std::vector<std::int64_t> prices = {10, 20, 30, 40, 50, 60};
    Batch batch(ids, prices);
    std::string error;

    // Step 1: parse the predicate and bind it to the batch's schema. Binding resolves "id" and "price".
    nano_lance::expr::Expression where;
    if (!nano_lance::expr::Expression::parse("id > 2 AND price < 50", where, error) ||
        !where.bind(batch.schema, error)) {
        return fail("parse or bind: " + error);
    }

    // Step 2: filter. keep[i] is 1 where the row passes. Rows 3 and 4 do: id > 2 and price < 50.
    std::vector<std::uint8_t> keep;
    if (!where.filter(batch.array, keep, error)) {
        return fail("filter: " + error);
    }
    std::cout << "keep:";
    for (const auto k : keep) {
        std::cout << " " << static_cast<int>(k);
    }
    std::cout << "\n";
    if (keep != std::vector<std::uint8_t>{0, 0, 1, 1, 0, 0}) {
        return fail("the filter kept the wrong rows");
    }

    // Step 3: evaluate a value expression, "price + id", as int64 (Arrow format "l").
    nano_lance::expr::Expression sum;
    if (!nano_lance::expr::Expression::parse("price + id", sum, error) || !sum.bind(batch.schema, error)) {
        return fail("parse or bind of price + id: " + error);
    }
    ArrowSchema int64_type{"l", "sum", nullptr, 0, 0, nullptr, nullptr, release_schema, nullptr};
    ArrowArray out{};
    if (!sum.evaluate(batch.array, int64_type, out, error)) {
        return fail("evaluate: " + error);
    }
    const auto* values = static_cast<const std::int64_t*>(out.buffers[1]) + out.offset;
    const std::vector<std::int64_t> got(values, values + out.length);
    if (out.release != nullptr) {
        out.release(&out);
    }
    std::cout << "price + id:";
    for (const auto v : got) {
        std::cout << " " << v;
    }
    std::cout << "\n";
    if (got != std::vector<std::int64_t>{11, 22, 33, 44, 55, 66}) {
        return fail("price + id gave the wrong values");
    }
    return 0;
}
