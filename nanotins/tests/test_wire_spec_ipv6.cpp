// wire_spec Milestone 3 — the IPv6 header (40 bytes): the hardest bit-field (version:4 /
// traffic_class:8 / flow_label:20 packed in one big-endian u32, crossing byte boundaries), fixed-size
// byte-array fields (the 16-byte addresses), and next_header — the field that decides the next PDU (the
// dispatch seam). Cross-checked against protocols::Ipv6 (soatins bits<>).
//   T1 hard bit-field + bytes oracle    T2 deterministic bulk    T3 SoA->Arrow (u32 + FSB columns)
//   T4 next_header is the dispatch key

#include "nanotins/bulk.hpp"
#include "nanotins/protocols.hpp"
#include "nanotins/wire_spec.hpp"
#include "nanotins/wire_spec_soa.hpp"

#include <nanoarrow/nanoarrow.h>

#include <exec/static_thread_pool.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace nanotins::literals;
using nanotins::wire_endian;

namespace {

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

using Ipv6Spec = nanotins::WireSpec<
    nanotins::named_bit_field<decltype("version"_fld), 0, std::uint32_t, 0, 4, wire_endian::big>,
    nanotins::named_bit_field<decltype("traffic_class"_fld), 0, std::uint32_t, 4, 8, wire_endian::big>,
    nanotins::named_bit_field<decltype("flow_label"_fld), 0, std::uint32_t, 12, 20, wire_endian::big>,
    nanotins::named_field<decltype("payload_length"_fld), 4, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("next_header"_fld), 6, std::uint8_t, wire_endian::big>,
    nanotins::named_field<decltype("hop_limit"_fld), 7, std::uint8_t, wire_endian::big>,
    nanotins::named_bytes_field<decltype("src_addr"_fld), 8, 16>,
    nanotins::named_bytes_field<decltype("dst_addr"_fld), 24, 16>>;

// vtf = 0x60012345 -> version=6, traffic_class=0, flow_label=0x12345; payload=40, next_header=6 (TCP),
// hop=64; src=2001:db8::1, dst=2001:db8::2.
const std::uint8_t kGolden[40] = {
    0x60, 0x01, 0x23, 0x45, 0x00, 0x28, 0x06, 0x40,
    0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};

}  // namespace

int main() {
    // ---- T1: the byte-crossing bit-field + byte-array oracle ----
    nanotins::struct_view<Ipv6Spec> v(kGolden);
    CHECK(v("version"_fld) == 6);
    CHECK(v("traffic_class"_fld) == 0);
    CHECK(v("flow_label"_fld) == 0x12345u);
    CHECK(v("payload_length"_fld) == 40);
    CHECK(v("next_header"_fld) == 6);
    CHECK(v("hop_limit"_fld) == 64);
    CHECK(std::memcmp(v("src_addr"_fld).data(), kGolden + 8, 16) == 0);
    CHECK(std::memcmp(v("dst_addr"_fld).data(), kGolden + 24, 16) == 0);

    // device-style direct read of the crossing field
    using FFlow = std::tuple_element_t<2, nanotins::spec_fields_t<Ipv6Spec>>;
    CHECK(nanotins::read_field<FFlow>(kGolden) == 0x12345u);

    // cross-check vs soatins bits<>/be<> overlay on the same bytes
    const protocols::Ipv6* oh = reinterpret_cast<const protocols::Ipv6*>(kGolden);
    CHECK(((oh->vtf.word_host() >> 28) & 0x0F) == v("version"_fld));
    CHECK(((oh->vtf.word_host() >> 20) & 0xFF) == v("traffic_class"_fld));
    CHECK((oh->vtf.word_host() & 0xFFFFF) == v("flow_label"_fld));
    CHECK(oh->payload_length.host() == v("payload_length"_fld));
    CHECK(oh->next_header == v("next_header"_fld));
    CHECK(oh->hop_limit == v("hop_limit"_fld));
    CHECK(std::memcmp(oh->src.data(), kGolden + 8, 16) == 0);

    // ---- T2: deterministic bulk over varied headers (flow_label crosses byte 1's nibble) ----
    constexpr std::size_t N = 256;
    std::vector<std::uint8_t> hdrs(N * 40);
    for (std::size_t i = 0; i < N; ++i) {
        std::uint8_t* h = &hdrs[i * 40];
        std::memcpy(h, kGolden, 40);
        const std::uint32_t flow = static_cast<std::uint32_t>(i * 7 + 1) & 0xFFFFFu;
        const std::uint32_t vtf = 0x60000000u | flow;  // version=6, tc=0, flow varies
        h[0] = static_cast<std::uint8_t>(vtf >> 24);
        h[1] = static_cast<std::uint8_t>(vtf >> 16);
        h[2] = static_cast<std::uint8_t>(vtf >> 8);
        h[3] = static_cast<std::uint8_t>(vtf);
        h[4] = static_cast<std::uint8_t>(i >> 8);  // payload_length = i
        h[5] = static_cast<std::uint8_t>(i);
        h[6] = static_cast<std::uint8_t>(i & 0xFF);  // next_header = i & 0xFF
    }

    nanotins::spec_soa<Ipv6Spec, N> seq;
    for (std::size_t i = 0; i < N; ++i) {
        seq.append(&hdrs[i * 40]);
    }
    nanotins::spec_soa<Ipv6Spec, N> blk;
    {
        exec::static_thread_pool pool(8);
        auto p = blk.raw();
        const std::uint8_t* base = hdrs.data();
        nanotins::bulk_for_each(pool.get_scheduler(), 16, N, [=](std::size_t i) {
            nanotins::scatter_spec<Ipv6Spec>(p, i, base + i * 40);
        });
    }
    for (std::size_t i = 0; i < N; ++i) {
        const std::uint32_t flow = static_cast<std::uint32_t>(i * 7 + 1) & 0xFFFFFu;
        CHECK(seq.column<2>()[i] == blk.column<2>()[i]);  // flow_label
        CHECK(seq.column<4>()[i] == blk.column<4>()[i]);  // next_header
        CHECK(seq.column<6>()[i] == blk.column<6>()[i]);  // src_addr (FSB)
        CHECK(seq.column<0>()[i] == 6);
        CHECK(seq.column<2>()[i] == flow);
        CHECK(seq.column<4>()[i] == static_cast<std::uint8_t>(i & 0xFF));
    }

    // ---- T3: spec_soa -> Arrow (u32 bit-field column + fixed-size-binary address column) ----
    ArrowArray arr{};
    std::string err;
    CHECK(nanotins::to_arrow_spec(seq, arr, err));
    CHECK(arr.length == static_cast<std::int64_t>(N));
    CHECK(arr.n_children == 8);
    const auto* c_flow = static_cast<const std::uint32_t*>(arr.children[2]->buffers[1]);
    const auto* c_src = static_cast<const std::uint8_t*>(arr.children[6]->buffers[1]);  // FSB: 16 bytes/row
    for (std::size_t i = 0; i < N; ++i) {
        CHECK(c_flow[i] == (static_cast<std::uint32_t>(i * 7 + 1) & 0xFFFFFu));
    }
    CHECK(std::memcmp(c_src, kGolden + 8, 16) == 0);  // row 0's src address
    if (arr.release) arr.release(&arr);

    // ---- T4: next_header is the dispatch key (the seam into the PDU DAG) ----
    CHECK(v("next_header"_fld) == 6);  // -> would dispatch to {l4_table, key=6} = TCP
    CHECK(seq.column<4>()[0] == static_cast<std::uint8_t>(0));  // i=0 -> next_header 0 (hop-by-hop ext)

    std::printf("wire_spec_ipv6: ok (T1 crossing bitfield + FSB vs soatins, T2 bulk==seq, T3 arrow, T4 next_header)\n");
    return 0;
}
