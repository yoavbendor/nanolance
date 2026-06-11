// soa device-view: filling a soa<T> via raw()+scatter() (the bulk/GPU path) must produce identical
// columns to soa<T>::store() (the serial path), including be<>/bits<> conversions. This is the primitive
// that lets a bulk kernel fill any described struct's SoA with no hand-written column list.

#include "nanotins/bits.hpp"
#include "nanotins/endian.hpp"
#include "nanotins/reflect.hpp"

#include <boost/describe.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "soa scatter test failed: %s\n", msg);
        std::exit(1);
    }
}

struct Row {
    std::uint32_t a;
    nanotins::be<std::uint16_t> b;  // big-endian on the wire
    nanotins::bits<std::uint8_t, nanotins::field<"hi", 4>, nanotins::field<"lo", 4>> nib;
    std::int64_t c;
};
BOOST_DESCRIBE_STRUCT(Row, (), (a, b, nib, c))

}  // namespace

int main() {
    constexpr std::size_t n = 5;
    auto mk = [](std::size_t i) {
        Row r{};
        r.a = static_cast<std::uint32_t>(1000 + i);
        r.b.raw[0] = 0x12;  // be bytes 0x12 0x34 -> host 0x1234
        r.b.raw[1] = 0x34;
        r.nib.word = static_cast<std::uint8_t>((i << 4) | (i + 1));  // hi=i, lo=i+1 (bare scalar word)
        r.c = -static_cast<std::int64_t>(i) * 7;
        return r;
    };

    // Reference: fill via store().
    nanotins::soa<Row> ref;
    ref.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        ref.store(i, mk(i));
    }

    // Under test: fill via raw()+scatter() (what a bulk kernel does).
    nanotins::soa<Row> got;
    got.resize(n);
    const nanotins::soa_ptrs<Row> p = got.raw();
    for (std::size_t i = 0; i < n; ++i) {
        nanotins::scatter<Row>(p, i, mk(i));
    }

    // Compare every flattened column (a, b, hi, lo, c) element-by-element.
    nanotins::for_each_column<Row>([&]<std::size_t I, class Col>() {
        for (std::size_t i = 0; i < n; ++i) {
            require(ref.column<I>()[i] == got.column<I>()[i], Col::name());
        }
    });

    std::puts("nanotins soa scatter ok (raw()+scatter == store for plain/be/bits columns)");
    return 0;
}
