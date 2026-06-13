// Phase 0b: zero-/minimal-copy to_arrow for the fixed soa<T,N>. The bulk per-column fill must produce a
// record batch byte-for-byte identical to the dynamic soa<T>'s per-row append path. We compare the two
// ArrowArrays' column data buffers directly (memcmp), plus spot-check the host values.

#include "soatins/arrow_glue.hpp"
#include "soatins/endian.hpp"
#include "soatins/reflect.hpp"

#include <nanoarrow/nanoarrow.h>

#include <boost/describe.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct Row {
    soatins::be<std::uint32_t> a;  // big-endian wire -> host u32 column
    std::uint16_t b;               // plain u16
    std::int64_t c;                // signed
    double d;                      // float
};
BOOST_DESCRIBE_STRUCT(Row, (), (a, b, c, d))

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

Row make_row(std::uint32_t i) {
    Row r{};
    const std::uint32_t va = i * 7u + 1u;
    r.a.raw[0] = static_cast<std::uint8_t>(va >> 24);
    r.a.raw[1] = static_cast<std::uint8_t>(va >> 16);
    r.a.raw[2] = static_cast<std::uint8_t>(va >> 8);
    r.a.raw[3] = static_cast<std::uint8_t>(va);
    r.b = static_cast<std::uint16_t>(i * 3u);
    r.c = -static_cast<std::int64_t>(i) * 11;
    r.d = static_cast<double>(i) * 1.5;
    return r;
}

}  // namespace

int main() {
    constexpr std::size_t N = 32;
    constexpr std::size_t K = 20;

    // Fixed soa<Row,N> -> to_arrow (bulk per-column fill).
    soatins::soa<Row, N> fixed;
    for (std::size_t i = 0; i < K; ++i) {
        fixed.append(make_row(static_cast<std::uint32_t>(i)));
    }
    ArrowArray fa{};
    std::string err;
    CHECK(soatins::to_arrow(fixed, fa, err));
    CHECK(fa.length == static_cast<std::int64_t>(K));
    CHECK(fa.n_children == static_cast<std::int64_t>(soatins::column_count<Row>));

    // Dynamic soa<Row> -> to_arrow (per-row append) — the reference.
    soatins::soa<Row> dyn;
    dyn.resize(K);
    for (std::size_t i = 0; i < K; ++i) {
        dyn.store(i, make_row(static_cast<std::uint32_t>(i)));
    }
    ArrowArray da{};
    CHECK(soatins::to_arrow(dyn, da, err));
    CHECK(da.length == static_cast<std::int64_t>(K));

    // Byte-for-byte: each column's data buffer (buffer index 1) must match.
    soatins::for_each_column<Row>([&]<std::size_t I, class Col>() {
        const auto* fb = static_cast<const std::uint8_t*>(fa.children[I]->buffers[1]);
        const auto* db = static_cast<const std::uint8_t*>(da.children[I]->buffers[1]);
        CHECK(fb != nullptr);
        CHECK(db != nullptr);
        const std::size_t bytes = K * sizeof(typename Col::elem);
        CHECK(std::memcmp(fb, db, bytes) == 0);
    });

    // Spot-check the actual host values through the fixed array's typed data buffers.
    {
        const auto* a = static_cast<const std::uint32_t*>(fa.children[0]->buffers[1]);
        const auto* b = static_cast<const std::uint16_t*>(fa.children[1]->buffers[1]);
        const auto* c = static_cast<const std::int64_t*>(fa.children[2]->buffers[1]);
        const auto* d = static_cast<const double*>(fa.children[3]->buffers[1]);
        for (std::size_t i = 0; i < K; ++i) {
            CHECK(a[i] == i * 7u + 1u);
            CHECK(b[i] == static_cast<std::uint16_t>(i * 3u));
            CHECK(c[i] == -static_cast<std::int64_t>(i) * 11);
            CHECK(d[i] == static_cast<double>(i) * 1.5);
        }
    }

    if (fa.release) fa.release(&fa);
    if (da.release) da.release(&da);

    std::printf("soa_fixed_arrow: ok (bulk to_arrow == row-append, %zu cols x %zu rows)\n",
                soatins::column_count<Row>, K);
    return 0;
}
