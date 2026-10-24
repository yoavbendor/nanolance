// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Arrow arrays, schemas and batches that own their C structs, and the builders the index writers use (internal).

#pragma once

#include <nanoarrow/nanoarrow.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace nano_lance::arrow_owned {

struct OwnedSchema {
    ArrowSchema s{};
    OwnedSchema() = default;
    OwnedSchema(const OwnedSchema&) = delete;
    OwnedSchema& operator=(const OwnedSchema&) = delete;
    ~OwnedSchema() {
        if (s.release != nullptr) {
            s.release(&s);
        }
    }
};

struct OwnedArray {
    ArrowArray a{};
    OwnedArray() = default;
    OwnedArray(const OwnedArray&) = delete;
    OwnedArray& operator=(const OwnedArray&) = delete;
    ~OwnedArray() {
        if (a.release != nullptr) {
            a.release(&a);
        }
    }
};

struct OwnedBatches {
    std::vector<ArrowArray> v;
    ~OwnedBatches() {
        for (auto& a : v) {
            if (a.release != nullptr) {
                a.release(&a);
            }
        }
    }
};

struct OwnedViews {
    std::vector<std::unique_ptr<ArrowArrayView>> v;
    ~OwnedViews() {
        for (auto& view : v) {
            ArrowArrayViewReset(view.get());
        }
    }
};

/// A non-nullable array of `n` fixed-width values of `type` (`width` bytes each).
bool uint_array(ArrowType type, const void* values, std::int64_t n, std::size_t width, ArrowArray& out,
                std::string& error);

/// A struct schema of `children` (name, type to copy, nullable).
bool struct_schema(const std::vector<std::tuple<const char*, const ArrowSchema*, bool>>& children, ArrowSchema& out,
                   std::string& error);

bool type_schema(ArrowType type, ArrowSchema& out);

/// A struct batch of `children` (moved in).
bool struct_batch(std::vector<ArrowArray*> children, std::int64_t length, ArrowArray& out, std::string& error);

}  // namespace nano_lance::arrow_owned
