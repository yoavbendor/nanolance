// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The owning Arrow wrappers and builders (src/arrow/arrow_owned.hpp) that the index writers use: a fixed-width
// array of given values, a struct schema and a struct batch built from them, and the Owned* wrappers, which
// must release exactly what they own, once. Release is checked with counting release callbacks.

#include "arrow/arrow_owned.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

namespace ao = nano_lance::arrow_owned;

int g_releases = 0;

void counting_release(ArrowArray* a) {
    ++g_releases;
    a->release = nullptr;
}

void counting_release_schema(ArrowSchema* s) {
    ++g_releases;
    s->release = nullptr;
}

}  // namespace

int main() {
    int checks = 0;

    // uint_array: n int32 values, no nulls, the bytes copied as given.
    {
        const std::int32_t values[] = {7, -1, 0, 2147483647};
        std::string error;
        ao::OwnedArray owned;
        require(ao::uint_array(NANOARROW_TYPE_INT32, values, 4, sizeof(std::int32_t), owned.a, error),
                "uint_array builds an int32 array (" + error + ")");
        require(owned.a.length == 4, "the array has 4 rows");
        require(owned.a.null_count == 0, "the array has no nulls");
        const auto* data = static_cast<const std::int32_t*>(owned.a.buffers[1]);
        require(data[0] == 7 && data[1] == -1 && data[2] == 0 && data[3] == 2147483647,
                "the values are copied as given");
        checks += 4;
    }

    // uint_array with zero rows, and with a one-byte width.
    {
        std::string error;
        ao::OwnedArray empty;
        require(ao::uint_array(NANOARROW_TYPE_INT64, nullptr, 0, sizeof(std::int64_t), empty.a, error),
                "uint_array builds an empty array");
        require(empty.a.length == 0, "an empty array has length 0");

        const std::uint8_t bytes[] = {1, 2, 255};
        ao::OwnedArray narrow;
        require(ao::uint_array(NANOARROW_TYPE_UINT8, bytes, 3, 1, narrow.a, error), "uint_array builds a uint8 array");
        const auto* data = static_cast<const std::uint8_t*>(narrow.a.buffers[1]);
        require(narrow.a.length == 3 && data[2] == 255, "uint8 values are one byte each");
        checks += 3;
    }

    // type_schema: the format string names the type.
    {
        ao::OwnedSchema int64;
        require(ao::type_schema(NANOARROW_TYPE_INT64, int64.s), "type_schema of int64");
        require(std::string(int64.s.format) == "l", "int64 has the format 'l'");
        ao::OwnedSchema int32;
        require(ao::type_schema(NANOARROW_TYPE_INT32, int32.s), "type_schema of int32");
        require(std::string(int32.s.format) == "i", "int32 has the format 'i'");
        checks += 4;
    }

    // struct_schema: children keep their names, types and nullability, in order.
    {
        ao::OwnedSchema a;
        ao::OwnedSchema b;
        require(ao::type_schema(NANOARROW_TYPE_INT32, a.s) && ao::type_schema(NANOARROW_TYPE_UINT64, b.s),
                "the child schemas");
        const std::vector<std::tuple<const char*, const ArrowSchema*, bool>> children = {
            {"id", &a.s, false}, {"score", &b.s, true}};
        ao::OwnedSchema out;
        std::string error;
        require(ao::struct_schema(children, out.s, error), "struct_schema builds (" + error + ")");
        require(out.s.n_children == 2, "the struct has two children");
        require(std::string(out.s.children[0]->name) == "id" && std::string(out.s.children[1]->name) == "score",
                "the children keep their names in order");
        require(std::string(out.s.children[0]->format) == "i" && std::string(out.s.children[1]->format) == "L",
                "the children keep their types");
        require((out.s.children[0]->flags & ARROW_FLAG_NULLABLE) == 0, "a non-nullable child is not flagged nullable");
        require((out.s.children[1]->flags & ARROW_FLAG_NULLABLE) != 0, "a nullable child is flagged nullable");
        checks += 6;
    }

    // struct_batch: children are moved in (the sources are left released), and the batch has the given length.
    {
        const std::int32_t ids[] = {1, 2, 3};
        const std::uint64_t scores[] = {10, 20, 30};
        ArrowArray id_arr{};
        ArrowArray score_arr{};
        std::string error;
        require(ao::uint_array(NANOARROW_TYPE_INT32, ids, 3, 4, id_arr, error), "the id child");
        require(ao::uint_array(NANOARROW_TYPE_UINT64, scores, 3, 8, score_arr, error), "the score child");

        ArrowArray batch{};
        require(ao::struct_batch({&id_arr, &score_arr}, 3, batch, error), "struct_batch builds (" + error + ")");
        require(batch.length == 3 && batch.n_children == 2, "the batch has 3 rows and 2 children");
        require(id_arr.release == nullptr && score_arr.release == nullptr, "the children are moved into the batch");
        const auto* id_data = static_cast<const std::int32_t*>(batch.children[0]->buffers[1]);
        const auto* score_data = static_cast<const std::uint64_t*>(batch.children[1]->buffers[1]);
        require(id_data[2] == 3 && score_data[1] == 20, "the children's values are intact in the batch");
        require(batch.children[0]->length == 3, "a child keeps its length");
        if (batch.release != nullptr) {
            batch.release(&batch);
        }
        checks += 5;
    }

    // Owned wrappers release what they own, once each: a counting release callback on a fake array, schema and
    // batch list is called exactly once when the wrapper goes out of scope.
    {
        g_releases = 0;
        {
            ao::OwnedArray a;
            a.a.release = &counting_release;
            a.a.length = 0;
        }
        require(g_releases == 1, "OwnedArray releases its array once");

        g_releases = 0;
        {
            ao::OwnedSchema s;
            s.s.release = &counting_release_schema;
        }
        require(g_releases == 1, "OwnedSchema releases its schema once");

        g_releases = 0;
        {
            ao::OwnedBatches batches;
            for (int i = 0; i < 3; ++i) {
                ArrowArray b{};
                b.release = &counting_release;
                batches.v.push_back(b);
            }
            batches.v[1].release = nullptr;  // an already released entry is skipped
        }
        require(g_releases == 2, "OwnedBatches releases each of its live arrays once");

        g_releases = 0;
        {
            ao::OwnedArray unused;  // never built: nothing to release, and no call
        }
        require(g_releases == 0, "an unused wrapper releases nothing");
        checks += 4;
    }

    std::cout << "arrow_owned: " << checks << " checks passed\n";
    return 0;
}
