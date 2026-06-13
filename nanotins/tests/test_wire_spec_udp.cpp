// wire_spec Milestone 1 — the UDP header (4x uint16, big-endian), the simplest possible wire PDU.
// Validates the promise chain end to end, all on CPU (the "device" reader runs here too and must match):
//   T1 one spec, two faces : struct_view == read_field (device-style) == be<> overlay == hand-decoded
//   T2 deterministic bulk   : sequential scatter == bulk_for_each(pool) scatter, columns byte-identical
//   T3 SoA -> Arrow         : spec_soa -> Arrow batch; column names/types/values correct
//   T4 no firehose regress  : time the spec read vs a be<> packed-overlay read (informational)

#include "nanotins/bulk.hpp"
#include "nanotins/wire_spec.hpp"
#include "nanotins/wire_spec_soa.hpp"

#include "soatins/endian.hpp"

#include <nanoarrow/nanoarrow.h>

#include <boost/describe.hpp>
#include <exec/static_thread_pool.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace nanotins::literals;

namespace {

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

// The UDP header as a wire spec: 4x uint16, big-endian, at byte offsets 0/2/4/6.
using UdpHdrSpec = nanotins::WireSpec<
    nanotins::named_field<decltype("src_port"_fld), 0, std::uint16_t, nanotins::wire_endian::big>,
    nanotins::named_field<decltype("dst_port"_fld), 2, std::uint16_t, nanotins::wire_endian::big>,
    nanotins::named_field<decltype("length"_fld), 4, std::uint16_t, nanotins::wire_endian::big>,
    nanotins::named_field<decltype("checksum"_fld), 6, std::uint16_t, nanotins::wire_endian::big>>;

// The same header as a soatins-described packed overlay struct — the cross-check oracle.
struct UdpHdr {
    soatins::be<std::uint16_t> src_port;
    soatins::be<std::uint16_t> dst_port;
    soatins::be<std::uint16_t> length;
    soatins::be<std::uint16_t> checksum;
};

// Write one UDP header (host values) big-endian into buf.
void put_udp(std::uint8_t* buf, std::uint16_t s, std::uint16_t d, std::uint16_t l, std::uint16_t c) {
    auto be16 = [](std::uint8_t* p, std::uint16_t v) {
        p[0] = static_cast<std::uint8_t>(v >> 8);
        p[1] = static_cast<std::uint8_t>(v);
    };
    be16(buf + 0, s);
    be16(buf + 2, d);
    be16(buf + 4, l);
    be16(buf + 6, c);
}

}  // namespace

BOOST_DESCRIBE_STRUCT(UdpHdr, (), (src_port, dst_port, length, checksum))

int main() {
    // ---- T1: one spec, two faces, vs overlay, vs hand-decoded ----
    std::uint8_t buf[8];
    put_udp(buf, 0x1234, 0x5678, 0x0010, 0xABCD);

    nanotins::struct_view<UdpHdrSpec> v(buf);
    CHECK(v("src_port"_fld) == 0x1234);
    CHECK(v("dst_port"_fld) == 0x5678);
    CHECK(v("length"_fld) == 0x0010);
    CHECK(v("checksum"_fld) == 0xABCD);

    // device-style direct reader (same code path the GPU kernel uses)
    using F0 = std::tuple_element_t<0, nanotins::spec_fields_t<UdpHdrSpec>>;
    using F3 = std::tuple_element_t<3, nanotins::spec_fields_t<UdpHdrSpec>>;
    CHECK(nanotins::read_field<F0>(buf) == 0x1234);
    CHECK(nanotins::read_field<F3>(buf) == 0xABCD);

    // cross-check against the be<> packed overlay
    const UdpHdr* ov = reinterpret_cast<const UdpHdr*>(buf);
    CHECK(ov->src_port.host() == 0x1234);
    CHECK(ov->dst_port.host() == 0x5678);
    CHECK(ov->length.host() == 0x0010);
    CHECK(ov->checksum.host() == 0xABCD);

    // ---- T2: deterministic bulk — sequential scatter == bulk_for_each scatter ----
    constexpr std::size_t N = 256;
    std::vector<std::uint8_t> hdrs(N * 8);
    for (std::size_t i = 0; i < N; ++i) {
        put_udp(&hdrs[i * 8], static_cast<std::uint16_t>(i), static_cast<std::uint16_t>(i * 3 + 1),
                static_cast<std::uint16_t>(8 + i), static_cast<std::uint16_t>(i ^ 0xFFFF));
    }

    nanotins::spec_soa<UdpHdrSpec, N> seq;
    for (std::size_t i = 0; i < N; ++i) {
        seq.append(&hdrs[i * 8]);
    }
    CHECK(seq.size() == N);

    nanotins::spec_soa<UdpHdrSpec, N> blk;
    {
        exec::static_thread_pool pool(8);
        auto p = blk.raw();
        const std::uint8_t* base = hdrs.data();
        nanotins::bulk_for_each(pool.get_scheduler(), 16, N, [=](std::size_t i) {
            nanotins::scatter_spec<UdpHdrSpec>(p, i, base + i * 8);
        });
    }

    // columns byte-identical, and equal to the expected host values
    for (std::size_t i = 0; i < N; ++i) {
        CHECK(seq.column<0>()[i] == blk.column<0>()[i]);
        CHECK(seq.column<1>()[i] == blk.column<1>()[i]);
        CHECK(seq.column<2>()[i] == blk.column<2>()[i]);
        CHECK(seq.column<3>()[i] == blk.column<3>()[i]);
        CHECK(seq.column<0>()[i] == static_cast<std::uint16_t>(i));
        CHECK(seq.column<1>()[i] == static_cast<std::uint16_t>(i * 3 + 1));
        CHECK(seq.column<2>()[i] == static_cast<std::uint16_t>(8 + i));
        CHECK(seq.column<3>()[i] == static_cast<std::uint16_t>(i ^ 0xFFFF));
    }

    // ---- T3: spec_soa -> Arrow batch ----
    ArrowArray arr{};
    std::string err;
    CHECK(nanotins::to_arrow_spec(seq, arr, err));
    CHECK(arr.length == static_cast<std::int64_t>(N));
    CHECK(arr.n_children == 4);
    const auto* a0 = static_cast<const std::uint16_t*>(arr.children[0]->buffers[1]);
    const auto* a2 = static_cast<const std::uint16_t*>(arr.children[2]->buffers[1]);
    for (std::size_t i = 0; i < N; ++i) {
        CHECK(a0[i] == static_cast<std::uint16_t>(i));
        CHECK(a2[i] == static_cast<std::uint16_t>(8 + i));
    }
    if (arr.release) arr.release(&arr);

    // ---- T4: perf signal — spec read vs be<> overlay read (informational, not asserted) ----
    {
        constexpr int iters = 2000;
        volatile std::uint64_t sink = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < iters; ++r)
            for (std::size_t i = 0; i < N; ++i) {
                nanotins::struct_view<UdpHdrSpec> sv(&hdrs[i * 8]);
                sink += sv("src_port"_fld) + sv("dst_port"_fld) + sv("length"_fld) + sv("checksum"_fld);
            }
        auto t1 = std::chrono::steady_clock::now();
        for (int r = 0; r < iters; ++r)
            for (std::size_t i = 0; i < N; ++i) {
                const UdpHdr* h = reinterpret_cast<const UdpHdr*>(&hdrs[i * 8]);
                sink += h->src_port.host() + h->dst_port.host() + h->length.host() + h->checksum.host();
            }
        auto t2 = std::chrono::steady_clock::now();
        const double spec_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (iters * N);
        const double ov_ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / (iters * N);
        std::fprintf(stderr, "T4 perf: spec %.2f ns/hdr vs be<> overlay %.2f ns/hdr (sink=%llu)\n", spec_ns,
                     ov_ns, static_cast<unsigned long long>(sink));
    }

    std::printf("wire_spec_udp: ok (T1 oracle, T2 bulk==sequential over %zu hdrs, T3 arrow)\n", N);
    return 0;
}
