#pragma once

// The SoA derivative of a WireSpec: a fixed-capacity, columnar store filled directly from raw PDU
// bytes. The same spec that drives the wire read (wire_spec.hpp) drives the columns here, so one
// declaration gives you both the overlay and the Lance-ready table.
//
// Reuse over duplication: the column STORAGE (soatins::soa_array_storage) and the column->Arrow mapping
// (soatins::nt_set_column_schema / arrow_kind) are shared with soatins' described-struct SoA. The only
// spec-specific piece is the SCATTER — described structs read a member, a spec reads bytes at an offset.
// scatter()/scatter_spec() are NANOTINS_HD, so the identical fill runs on a CPU pool or a GPU kernel.

#include "nanotins/wire_spec.hpp"

#include "soatins/arrow_glue.hpp"      // nt_set_column_schema, map_arrow_type
#include "soatins/column_traits.hpp"   // kind_for_scalar
#include "soatins/reflect.hpp"         // soa_array_storage, soa_ptrs_h

#include <nanoarrow/nanoarrow.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>

namespace nanotins {

// One column per spec field, in the shape soatins' storage + Arrow glue expect (elem / kind / name()).
template <class F>
struct spec_col {
    using elem = typename F::value_type;
    static constexpr soatins::arrow_kind kind = soatins::column_traits<elem>::kind;
    static constexpr int fixed_width = soatins::column_traits<elem>::fixed_width;
    static const char* name() { return F::name(); }
};

// Build the column list from the spec's FLATTENED leaf fields (spec_fields_t), so specs composed with
// embed<> (which expand to leaf fields) get one column per leaf — not one per embed<> group.
template <class FieldsTuple>
struct columns_of_fields_h;
template <class... Fields>
struct columns_of_fields_h<std::tuple<Fields...>> {
    using type = std::tuple<spec_col<Fields>...>;
};
template <class Spec>
using columns_of_spec = typename columns_of_fields_h<spec_fields_t<Spec>>::type;

// Device-fillable bulk scatter: read every field of `Spec` from `pdu` and write column `i` of the pointer
// pack `p` (one elem* per column). This is the kernel a CPU pool or a GPU stream runs, unchanged.
template <class Spec, class Ptrs>
NANOTINS_HD inline void scatter_spec(const Ptrs& p, std::size_t i, const std::uint8_t* pdu) {
    using fields = spec_fields_t<Spec>;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((std::get<I>(p)[i] = read_field<std::tuple_element_t<I, fields>>(pdu)), ...);
    }(std::make_index_sequence<Spec::field_count>{});
}

// A TRIVIALLY-COPYABLE device pointer pack: one void* per column. std::tuple (soa_ptrs) is not trivially
// copyable under libstdc++, so a kernel lambda capturing it fails nvexec's GPU bulk requirement
// (trivially_copyable<Fun>) — it must memcpy the kernel to the device. A POD void*[N] satisfies it; the
// per-column element type is recovered at compile time from the Spec. (Same reason decode_window_gpu uses
// a POD PduSink rather than a tuple.) Used by the GPU path; the CPU path keeps the tuple pack above.
template <std::size_t N>
struct dev_ptr_pack {
    void* p[N];
};

template <class Spec, std::size_t N>
NANOTINS_HD inline void scatter_spec_pod(const dev_ptr_pack<N>& pack, std::size_t i,
                                         const std::uint8_t* pdu) {
    using fields = spec_fields_t<Spec>;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((static_cast<typename std::tuple_element_t<I, fields>::value_type*>(pack.p[I])[i] =
              read_field<std::tuple_element_t<I, fields>>(pdu)),
         ...);
    }(std::make_index_sequence<Spec::field_count>{});
}

// Fixed-capacity SoA over a wire spec. append()/scatter() read one PDU's fields into row i; raw() hands
// the column pointers to scatter_spec for the bulk/GPU path.
template <class Spec, std::size_t N>
class spec_soa {
public:
    using cols = columns_of_spec<Spec>;
    static constexpr std::size_t ncols = std::tuple_size_v<cols>;
    static constexpr std::size_t capacity = N;

    std::size_t size() const { return size_; }
    bool full() const { return size_ >= N; }
    void clear() { size_ = 0; }

    NANOTINS_HD void scatter(std::size_t i, const std::uint8_t* pdu) { scatter_spec<Spec>(raw_ptrs(), i, pdu); }
    bool append(const std::uint8_t* pdu) {
        scatter(size_, pdu);
        ++size_;
        return size_ >= N;
    }

    template <std::size_t I>
    const auto& column() const {
        return std::get<I>(columns_);
    }

    typename soatins::soa_ptrs_h<cols>::type raw() { return raw_ptrs(); }

private:
    typename soatins::soa_ptrs_h<cols>::type raw_ptrs() {
        typename soatins::soa_ptrs_h<cols>::type p;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((std::get<I>(p) = std::get<I>(columns_).data()), ...);
        }(std::make_index_sequence<ncols>{});
        return p;
    }

    typename soatins::soa_array_storage<cols, N>::type columns_;  // uninitialized trivial arrays (no zero-init)
    std::size_t size_ = 0;
};

// Arrow record batch from a spec_soa: one bulk ArrowBufferAppend per column (no per-row append), reusing
// soatins' column->Arrow schema mapping. The columns are already the contiguous Arrow data-buffer layout.
template <class Spec, std::size_t N>
inline bool to_arrow_spec(const spec_soa<Spec, N>& s, ArrowArray& out, std::string& error) {
    using cols = columns_of_spec<Spec>;
    constexpr std::size_t ncols = std::tuple_size_v<cols>;

    ArrowSchema schema;
    ArrowSchemaInit(&schema);
    if (ArrowSchemaSetTypeStruct(&schema, static_cast<std::int64_t>(ncols)) != NANOARROW_OK) {
        error = "spec arrow: alloc struct schema";
        return false;
    }
    bool ok = true;
    std::string err;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((ok = ok && soatins::nt_set_column_schema(schema.children[I], std::tuple_element_t<I, cols>::kind,
                                                   std::tuple_element_t<I, cols>::fixed_width,
                                                   std::tuple_element_t<I, cols>::name(), err)),
         ...);
    }(std::make_index_sequence<ncols>{});
    if (!ok) {
        error = err;
        ArrowSchemaRelease(&schema);
        return false;
    }
    schema.flags = 0;
    if (ArrowArrayInitFromSchema(&out, &schema, nullptr) != NANOARROW_OK) {
        error = "spec arrow: init array";
        ArrowSchemaRelease(&schema);
        return false;
    }
    ArrowSchemaRelease(&schema);

    const std::int64_t n = static_cast<std::int64_t>(s.size());
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((ok = ok && ArrowBufferAppend(ArrowArrayBuffer(out.children[I], 1), s.template column<I>().data(),
                                       n * static_cast<std::int64_t>(sizeof(typename std::tuple_element_t<I, cols>::elem))) ==
                          NANOARROW_OK,
          out.children[I]->length = n, out.children[I]->null_count = 0),
         ...);
    }(std::make_index_sequence<ncols>{});
    if (!ok) {
        error = "spec arrow: bulk fill column";
        ArrowArrayRelease(&out);
        return false;
    }
    out.length = n;
    out.null_count = 0;
    if (ArrowArrayFinishBuildingDefault(&out, nullptr) != NANOARROW_OK) {
        error = "spec arrow: finalize";
        ArrowArrayRelease(&out);
        return false;
    }
    return true;
}

}  // namespace nanotins
