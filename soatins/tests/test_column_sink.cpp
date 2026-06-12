// Phase 1: column_sink<T,N> auto-flush. push() N rows -> one flush of a full chunk; the partial tail
// drains on finish(). Proves: flush fires exactly at each N-th row (not before), the flushed chunks carry
// the right rows in order, clear() lets storage be reused across flushes, and finish() drains the remainder.

#include "soatins/endian.hpp"
#include "soatins/reflect.hpp"
#include "soatins/sink.hpp"

#include <boost/describe.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct Row {
    soatins::be<std::uint32_t> a;  // big-endian wire -> host u32
    std::uint16_t b;
};
BOOST_DESCRIBE_STRUCT(Row, (), (a, b))

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

Row make_row(std::uint32_t i) {
    Row r{};
    const std::uint32_t va = i * 5u + 2u;
    r.a.raw[0] = static_cast<std::uint8_t>(va >> 24);
    r.a.raw[1] = static_cast<std::uint8_t>(va >> 16);
    r.a.raw[2] = static_cast<std::uint8_t>(va >> 8);
    r.a.raw[3] = static_cast<std::uint8_t>(va);
    r.b = static_cast<std::uint16_t>(i * 7u);
    return r;
}

}  // namespace

int main() {
    constexpr std::size_t N = 4;
    constexpr std::size_t K = 10;  // 10 rows / chunk-of-4 -> flushes at 4 and 8, then 2 left for finish()

    std::vector<std::pair<std::uint32_t, std::uint16_t>> drained;  // all rows the flush has seen, in order
    std::vector<std::size_t> flush_sizes;                          // size of each flushed chunk

    auto sink = soatins::make_column_sink<Row, N>([&](soatins::soa<Row, N>& chunk, std::string&) {
        flush_sizes.push_back(chunk.size());
        for (std::size_t i = 0; i < chunk.size(); ++i) {
            drained.emplace_back(chunk.template column<0>()[i], chunk.template column<1>()[i]);
        }
        return true;
    });

    std::string err;
    for (std::size_t i = 0; i < K; ++i) {
        CHECK(sink.push(make_row(static_cast<std::uint32_t>(i)), err));
        // After pushing rows [0..i], flushes have fired for every full chunk so far.
        const std::size_t expected_flushes = (i + 1) / N;
        CHECK(flush_sizes.size() == expected_flushes);
        CHECK(sink.pending() == (i + 1) % N);
    }

    // Two full flushes (rows 0-3, 4-7); 2 rows still buffered.
    CHECK(flush_sizes.size() == 2);
    CHECK(flush_sizes[0] == N);
    CHECK(flush_sizes[1] == N);
    CHECK(sink.pending() == 2);
    CHECK(drained.size() == 8);

    // Drain the tail.
    CHECK(sink.finish(err));
    CHECK(flush_sizes.size() == 3);
    CHECK(flush_sizes[2] == 2);
    CHECK(sink.pending() == 0);
    CHECK(drained.size() == K);

    // finish() again is a no-op (empty chunk).
    CHECK(sink.finish(err));
    CHECK(flush_sizes.size() == 3);

    // Every row arrived exactly once, in push order, with correct values.
    for (std::size_t i = 0; i < K; ++i) {
        CHECK(drained[i].first == i * 5u + 2u);
        CHECK(drained[i].second == static_cast<std::uint16_t>(i * 7u));
    }

    std::printf("column_sink: ok (auto-flush at N=%zu over %zu rows -> 3 chunks, order + values preserved)\n",
                N, K);
    return 0;
}
