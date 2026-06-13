// Phase 0a: the fixed-capacity SoA (soa<T,N>) + the non-owning soa_view<T>.
// Proves: (1) append() fills via the same reflection fold as the dynamic soa<T>, column-for-column
// (including the be<> byte-swap); (2) the occupied counter / full() fire exactly at N; (3) clear()
// resets; (4) a soa_view borrowed from raw()+size() reads the same values. No Arrow yet (that's
// Phase 0b's zero-copy to_arrow).

#include "soatins/endian.hpp"
#include "soatins/reflect.hpp"

#include <boost/describe.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

struct Row {
    soatins::be<std::uint32_t> a;  // big-endian on the wire -> host u32 column
    std::uint16_t b;               // plain u16 column
    std::uint8_t c;                // plain u8 column
};
BOOST_DESCRIBE_STRUCT(Row, (), (a, b, c))

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

// Build a row whose host values are a deterministic function of i. `a` is stored big-endian in its raw
// bytes, so col_at<Row,0>::get (which calls .host()) reads back exactly `va`.
Row make_row(std::uint32_t i) {
    Row r{};
    const std::uint32_t va = i * 7u + 1u;
    r.a.raw[0] = static_cast<std::uint8_t>(va >> 24);
    r.a.raw[1] = static_cast<std::uint8_t>(va >> 16);
    r.a.raw[2] = static_cast<std::uint8_t>(va >> 8);
    r.a.raw[3] = static_cast<std::uint8_t>(va);
    r.b = static_cast<std::uint16_t>(i * 3u);
    r.c = static_cast<std::uint8_t>(i);
    return r;
}

}  // namespace

int main() {
    constexpr std::size_t N = 16;
    constexpr std::size_t K = 10;  // fill fewer than N to exercise the partial tail

    // Fixed soa<Row, N>: fill via append(), tracking the occupied counter + full().
    soatins::soa<Row, N> fixed;
    CHECK(fixed.size() == 0);
    CHECK(fixed.capacity == N);
    for (std::size_t i = 0; i < K; ++i) {
        const bool now_full = fixed.append(make_row(static_cast<std::uint32_t>(i)));
        CHECK(now_full == (i + 1 == N));  // never full before the N-th append
    }
    CHECK(fixed.size() == K);
    CHECK(!fixed.full());
    CHECK(fixed.space() == N - K);

    // Dynamic soa<Row> (== soa<Row, soa_dynamic>): the reference, sized + stored row by row.
    soatins::soa<Row> dyn;
    dyn.resize(K);
    for (std::size_t i = 0; i < K; ++i) {
        dyn.store(i, make_row(static_cast<std::uint32_t>(i)));
    }
    CHECK(dyn.size() == K);

    // Column-for-column parity: fixed (std::array backing) == dynamic (std::vector backing).
    for (std::size_t i = 0; i < K; ++i) {
        CHECK(fixed.column<0>()[i] == dyn.column<0>()[i]);  // a (u32, byte-swapped to host)
        CHECK(fixed.column<1>()[i] == dyn.column<1>()[i]);  // b (u16)
        CHECK(fixed.column<2>()[i] == dyn.column<2>()[i]);  // c (u8)
        CHECK(fixed.column<0>()[i] == i * 7u + 1u);         // and the actual expected host values
        CHECK(fixed.column<1>()[i] == static_cast<std::uint16_t>(i * 3u));
        CHECK(fixed.column<2>()[i] == static_cast<std::uint8_t>(i));
    }

    // Non-owning view borrowed from the fixed SoA's buffers (zero-copy read).
    soatins::soa_view<Row> view(fixed.raw(), fixed.size());
    CHECK(view.size() == K);
    for (std::size_t i = 0; i < K; ++i) {
        CHECK(view.column<0>()[i] == i * 7u + 1u);
        CHECK(view.column<1>()[i] == static_cast<std::uint16_t>(i * 3u));
        CHECK(view.column<2>()[i] == static_cast<std::uint8_t>(i));
    }

    // Fill to capacity: the N-th append must report full.
    soatins::soa<Row, N> brimming;
    bool full_at_n = false;
    for (std::size_t i = 0; i < N; ++i) {
        full_at_n = brimming.append(make_row(static_cast<std::uint32_t>(i)));
    }
    CHECK(full_at_n);
    CHECK(brimming.full());
    CHECK(brimming.space() == 0);

    // clear() resets the occupied counter (storage untouched / reusable).
    brimming.clear();
    CHECK(brimming.size() == 0);
    CHECK(!brimming.full());

    std::printf("soa_fixed: ok (fixed==dynamic for %zu cols, full() at N=%zu, view zero-copy read)\n",
                soatins::column_count<Row>, N);
    return 0;
}
