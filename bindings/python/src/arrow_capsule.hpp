// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Arrow PyCapsule import/export helpers for nanobind extensions.
// Uses the Arrow C Data Interface (no hard pyarrow dependency at runtime).

#pragma once

#include <nanoarrow/nanoarrow.h>

#include <nanobind/nanobind.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nb = nanobind;

namespace nanolance_py::arrow_capsule {

namespace detail {

inline void require_capsule_name(const nb::capsule& cap, std::string_view expected) {
    if (!cap.is_valid()) {
        throw std::runtime_error("invalid Arrow PyCapsule");
    }
    const char* name = cap.name();
    if (name == nullptr || std::string_view(name) != expected) {
        throw std::runtime_error(std::string("expected PyCapsule named '") + std::string(expected) +
                                 "'");
    }
}

inline ArrowSchema* steal_schema_capsule(const nb::capsule& cap) {
    require_capsule_name(cap, "arrow_schema");
    void* ptr = PyCapsule_GetPointer(cap.ptr(), "arrow_schema");
    if (!ptr) {
        throw std::runtime_error("failed to extract arrow_schema capsule");
    }
    PyCapsule_SetDestructor(cap.ptr(), nullptr);
    return static_cast<ArrowSchema*>(ptr);
}

inline ArrowArray* steal_array_capsule(const nb::capsule& cap) {
    require_capsule_name(cap, "arrow_array");
    void* ptr = PyCapsule_GetPointer(cap.ptr(), "arrow_array");
    if (!ptr) {
        throw std::runtime_error("failed to extract arrow_array capsule");
    }
    PyCapsule_SetDestructor(cap.ptr(), nullptr);
    return static_cast<ArrowArray*>(ptr);
}

inline ArrowArrayStream* steal_stream_capsule(const nb::capsule& cap) {
    require_capsule_name(cap, "arrow_array_stream");
    void* ptr = PyCapsule_GetPointer(cap.ptr(), "arrow_array_stream");
    if (!ptr) {
        throw std::runtime_error("failed to extract arrow_array_stream capsule");
    }
    PyCapsule_SetDestructor(cap.ptr(), nullptr);
    return static_cast<ArrowArrayStream*>(ptr);
}

inline nb::capsule make_stream_capsule(ArrowArrayStream* stream) {
    return nb::capsule(stream, "arrow_array_stream", [](void* p) noexcept {
        auto* s = static_cast<ArrowArrayStream*>(p);
        if (s->release) {
            s->release(s);
        }
        std::free(s);
    });
}

inline nb::capsule make_schema_capsule(ArrowSchema* schema) {
    return nb::capsule(schema, "arrow_schema", [](void* p) noexcept {
        auto* s = static_cast<ArrowSchema*>(p);
        if (s->release) {
            s->release(s);
        }
        std::free(s);
    });
}

inline nb::capsule make_array_capsule(ArrowArray* array) {
    return nb::capsule(array, "arrow_array", [](void* p) noexcept {
        auto* a = static_cast<ArrowArray*>(p);
        if (a->release) {
            a->release(a);
        }
        std::free(a);
    });
}

struct SingleBatchStreamState {
    std::unique_ptr<ArrowSchema, void (*)(ArrowSchema*)> schema{nullptr, ArrowSchemaRelease};
    std::unique_ptr<ArrowArray, void (*)(ArrowArray*)> array{nullptr, ArrowArrayRelease};
};

struct OwnedSchema {
    ArrowSchema schema{};
    OwnedSchema() { ArrowSchemaInit(&schema); }
    ~OwnedSchema() { ArrowSchemaRelease(&schema); }
    OwnedSchema(const OwnedSchema&) = delete;
    OwnedSchema& operator=(const OwnedSchema&) = delete;
};

struct OwnedArray {
    ArrowArray array{};
    OwnedArray() { std::memset(&array, 0, sizeof(ArrowArray)); }
    ~OwnedArray() {
        if (array.release) {
            ArrowArrayRelease(&array);
        }
    }
    OwnedArray(const OwnedArray&) = delete;
    OwnedArray& operator=(const OwnedArray&) = delete;
    void reset() {
        if (array.release) {
            ArrowArrayRelease(&array);
        }
        std::memset(&array, 0, sizeof(ArrowArray));
    }
};

}  // namespace detail

inline void nanoarrow_check(const char* context, ArrowErrorCode code) {
    if (code != NANOARROW_OK) {
        throw std::runtime_error(std::string(context) + ": nanoarrow error " + std::to_string(code));
    }
}

inline std::pair<std::unique_ptr<ArrowSchema, void (*)(ArrowSchema*)>,
                std::unique_ptr<ArrowArray, void (*)(ArrowArray*)>>
import_batch(nb::handle obj) {
    if (!nb::hasattr(obj, "__arrow_c_array__")) {
        throw std::runtime_error("object does not implement __arrow_c_array__");
    }
    nb::object result = nb::steal(PyObject_CallMethod(obj.ptr(), "__arrow_c_array__", nullptr));
    if (!result) {
        throw nb::python_error();
    }

    nb::capsule schema_cap;
    nb::capsule array_cap;
    if (nb::isinstance<nb::tuple>(result) || nb::isinstance<nb::list>(result)) {
        nb::sequence seq = nb::cast<nb::sequence>(result);
        const std::size_t n = nb::len(seq);
        if (n == 2) {
            schema_cap = nb::cast<nb::capsule>(seq[0]);
            array_cap = nb::cast<nb::capsule>(seq[1]);
        } else if (n == 1) {
            array_cap = nb::cast<nb::capsule>(seq[0]);
        } else {
            throw std::runtime_error("__arrow_c_array__ must return 1 or 2 capsules");
        }
    } else {
        throw std::runtime_error("__arrow_c_array__ must return a tuple of capsules");
    }

    auto schema = std::unique_ptr<ArrowSchema, void (*)(ArrowSchema*)>(new ArrowSchema(),
                                                                       ArrowSchemaRelease);
    ArrowSchemaInit(schema.get());
    if (schema_cap.is_valid()) {
        ArrowSchema* imported = detail::steal_schema_capsule(schema_cap);
        ArrowSchemaMove(imported, schema.get());
        std::free(imported);
    } else if (nb::hasattr(obj, "__arrow_c_schema__")) {
        nb::object schema_obj =
            nb::steal(PyObject_CallMethod(obj.ptr(), "__arrow_c_schema__", nullptr));
        if (!schema_obj) {
            throw nb::python_error();
        }
        nb::capsule cap = nb::cast<nb::capsule>(schema_obj);
        ArrowSchema* imported = detail::steal_schema_capsule(cap);
        ArrowSchemaMove(imported, schema.get());
        std::free(imported);
    } else {
        throw std::runtime_error("could not import Arrow schema from object");
    }

    auto array = std::unique_ptr<ArrowArray, void (*)(ArrowArray*)>(new ArrowArray(), ArrowArrayRelease);
    std::memset(array.get(), 0, sizeof(ArrowArray));
    ArrowArray* imported_array = detail::steal_array_capsule(array_cap);
    ArrowArrayMove(imported_array, array.get());
    std::free(imported_array);
    return {std::move(schema), std::move(array)};
}

class BatchIterator {
public:
    explicit BatchIterator(nb::handle obj) : keepalive_(nb::borrow(obj)) {
        const char* stream_method = nullptr;
        if (nb::hasattr(obj, "__arrow_c_stream__")) {
            stream_method = "__arrow_c_stream__";
        } else if (nb::hasattr(obj, "__arrow_c_array_stream__")) {
            stream_method = "__arrow_c_array_stream__";
        }
        if (stream_method) {
            nb::object cap_obj = nb::steal(PyObject_CallMethod(obj.ptr(), stream_method, nullptr));
            if (!cap_obj) {
                throw nb::python_error();
            }
            nb::capsule cap = nb::cast<nb::capsule>(cap_obj);
            stream_.reset(detail::steal_stream_capsule(cap));
            return;
        }

        if (nb::hasattr(obj, "__arrow_c_array__")) {
            auto imported = import_batch(obj);
            single_batch_state_ = std::make_shared<detail::SingleBatchStreamState>();
            single_batch_state_->schema = std::move(imported.first);
            single_batch_state_->array = std::move(imported.second);

            auto* stream = static_cast<ArrowArrayStream*>(std::malloc(sizeof(ArrowArrayStream)));
            std::memset(stream, 0, sizeof(ArrowArrayStream));
            nanoarrow_check("ArrowBasicArrayStreamInit",
                            ArrowBasicArrayStreamInit(stream, single_batch_state_->schema.get(), 1));
            ArrowBasicArrayStreamSetArray(stream, 0, single_batch_state_->array.get());
            single_batch_state_->schema.release();
            single_batch_state_->array.release();
            stream_.reset(stream);
            return;
        }

        throw std::runtime_error("object does not export Arrow data via PyCapsule interface");
    }

    bool next(ArrowSchema* out_schema, ArrowArray* out_array) {
        if (!stream_) {
            return false;
        }
        detail::OwnedArray tmp;
        int rc = stream_->get_next(stream_.get(), &tmp.array);
        if (rc != 0) {
            const char* err = stream_->get_last_error ? stream_->get_last_error(stream_.get()) : nullptr;
            throw std::runtime_error(err ? err : "ArrowArrayStream::get_next failed");
        }
        if (tmp.array.release == nullptr) {
            return false;
        }
        if (!schema_loaded_) {
            nanoarrow_check("ArrowArrayStreamGetSchema",
                            ArrowArrayStreamGetSchema(stream_.get(), out_schema, nullptr));
            schema_loaded_ = true;
        }
        ArrowArrayMove(&tmp.array, out_array);
        return true;
    }

private:
    nb::object keepalive_;
    std::shared_ptr<detail::SingleBatchStreamState> single_batch_state_;
    std::unique_ptr<ArrowArrayStream, void (*)(ArrowArrayStream*)> stream_{nullptr,
                                                                            ArrowArrayStreamRelease};
    bool schema_loaded_ = false;
};

struct ExportState {
    ArrowSchema* schema = nullptr;
    std::vector<ArrowArray*> arrays;

    ~ExportState() {
        if (schema) {
            ArrowSchemaRelease(schema);
            std::free(schema);
        }
        for (ArrowArray* arr : arrays) {
            if (arr) {
                ArrowArrayRelease(arr);
                std::free(arr);
            }
        }
    }

    ExportState() = default;
    ExportState(const ExportState&) = delete;
    ExportState& operator=(const ExportState&) = delete;
};

/// A handle over a LIVE ArrowArrayStream, exported to Python once.
///
/// The difference from ExportedTable is when the decode happens. ExportedTable holds batches that
/// have already been decoded; this holds an open dataset and decodes a batch each time the consumer
/// asks for one, which is what makes a larger-than-memory dataset readable and what gets a first
/// batch out without waiting for the last.
///
/// Export is single-shot, as it has to be: a stream is consumed, not copied. The second
/// `__arrow_c_stream__` raises rather than handing out a stream someone else is already draining.
class ExportedStream {
public:
    ExportedStream() = default;

    /// Takes ownership of `stream` (which must be a valid, open ArrowArrayStream).
    static ExportedStream adopt(ArrowArrayStream&& stream) {
        ExportedStream out;
        out.stream_ = static_cast<ArrowArrayStream*>(std::malloc(sizeof(ArrowArrayStream)));
        if (out.stream_ == nullptr) {
            throw std::bad_alloc();
        }
        std::memcpy(out.stream_, &stream, sizeof(ArrowArrayStream));
        std::memset(&stream, 0, sizeof(ArrowArrayStream));
        return out;
    }

    nb::capsule arrow_c_stream(nb::object /*requested_schema*/) {
        if (stream_ == nullptr) {
            throw std::runtime_error(
                "this nanolance reader has already been consumed; call read_table() again for a "
                "second pass (a stream is consumed, not copied)");
        }
        auto* stream = stream_;
        stream_ = nullptr;  // the capsule owns it now, and releases it when Python drops it
        return detail::make_stream_capsule(stream);
    }

    ~ExportedStream() {
        // Only runs when the handle was never exported; after export the capsule owns the stream.
        if (stream_ != nullptr) {
            if (stream_->release != nullptr) {
                stream_->release(stream_);
            }
            std::free(stream_);
        }
    }

    ExportedStream(ExportedStream&& other) noexcept : stream_(other.stream_) { other.stream_ = nullptr; }
    ExportedStream& operator=(ExportedStream&& other) noexcept {
        if (this != &other) {
            std::swap(stream_, other.stream_);
        }
        return *this;
    }
    ExportedStream(const ExportedStream&) = delete;
    ExportedStream& operator=(const ExportedStream&) = delete;

private:
    ArrowArrayStream* stream_ = nullptr;
};

class ExportedTable {
public:
    ExportedTable() = default;

    static ExportedTable from_read_result(ArrowSchema* schema, ArrowArray* batches, std::size_t count) {
        ExportedTable out;
        out.state_ = std::make_shared<ExportState>();
        out.state_->schema = static_cast<ArrowSchema*>(std::malloc(sizeof(ArrowSchema)));
        std::memset(out.state_->schema, 0, sizeof(ArrowSchema));
        ArrowSchemaMove(schema, out.state_->schema);
        out.state_->arrays.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto* arr = static_cast<ArrowArray*>(std::malloc(sizeof(ArrowArray)));
            std::memset(arr, 0, sizeof(ArrowArray));
            ArrowArrayMove(&batches[i], arr);
            out.state_->arrays.push_back(arr);
        }
        return out;
    }

    nb::capsule arrow_c_stream(nb::object /*requested_schema*/) const {
        if (!state_) {
            throw std::runtime_error("empty ExportedTable");
        }
        auto state = state_;
        auto* stream = static_cast<ArrowArrayStream*>(std::malloc(sizeof(ArrowArrayStream)));
        std::memset(stream, 0, sizeof(ArrowArrayStream));
        nanoarrow_check("ArrowBasicArrayStreamInit",
                        ArrowBasicArrayStreamInit(stream, state->schema,
                                                  static_cast<int64_t>(state->arrays.size())));
        state->schema = nullptr;
        for (std::size_t i = 0; i < state->arrays.size(); ++i) {
            ArrowBasicArrayStreamSetArray(stream, static_cast<int64_t>(i), state->arrays[i]);
            state->arrays[i] = nullptr;
        }
        return detail::make_stream_capsule(stream);
    }

private:
    std::shared_ptr<ExportState> state_;
};

}  // namespace nanolance_py::arrow_capsule
