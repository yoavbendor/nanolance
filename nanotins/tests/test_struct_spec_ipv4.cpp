// struct_spec Milestone 2 — the IPv4 header (20 bytes, bit-fields + scalars), the first wire PDU with
// named_bit_field (version:4/ihl:4, dscp:6/ecn:2, flags:3/frag_offset:13) and an IHL-driven extent.
//   T1 bit-field oracle : struct_view / device read_field == hand-decoded == soatins bits<> overlay
//   T2 deterministic bulk: sequential scatter == bulk_for_each, bit-field + scalar columns identical
//   T3 SoA -> Arrow      : spec_soa with bit-field columns -> Arrow batch
//   T4 field-driven extent: header length = ihl * 4 (a parsed field decides the structure size)

#include "nanotins/bulk.hpp"
#include "nanotins/protocols.hpp"  // protocols::Ipv4 (soatins bits<> overlay) — the cross-check oracle
#include "nanotins/struct_spec.hpp"
#include "nanotins/struct_spec_soa.hpp"

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

using Ipv4Spec = nanotins::StructSpec<
    nanotins::named_bit_field<decltype("version"_fld), 0, std::uint8_t, 0, 4, wire_endian::big>,
    nanotins::named_bit_field<decltype("ihl"_fld), 0, std::uint8_t, 4, 4, wire_endian::big>,
    nanotins::named_bit_field<decltype("dscp"_fld), 1, std::uint8_t, 0, 6, wire_endian::big>,
    nanotins::named_bit_field<decltype("ecn"_fld), 1, std::uint8_t, 6, 2, wire_endian::big>,
    nanotins::named_field<decltype("total_length"_fld), 2, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("identification"_fld), 4, std::uint16_t, wire_endian::big>,
    nanotins::named_bit_field<decltype("flags"_fld), 6, std::uint16_t, 0, 3, wire_endian::big>,
    nanotins::named_bit_field<decltype("frag_offset"_fld), 6, std::uint16_t, 3, 13, wire_endian::big>,
    nanotins::named_field<decltype("ttl"_fld), 8, std::uint8_t, wire_endian::big>,
    nanotins::named_field<decltype("protocol"_fld), 9, std::uint8_t, wire_endian::big>,
    nanotins::named_field<decltype("checksum"_fld), 10, std::uint16_t, wire_endian::big>,
    nanotins::named_field<decltype("src_addr"_fld), 12, std::uint32_t, wire_endian::big>,
    nanotins::named_field<decltype("dst_addr"_fld), 16, std::uint32_t, wire_endian::big>>;

// A real IPv4 header: 45 00 00 3c 1c 46 40 00 40 06 b1 e6 c0 a8 00 68 c0 a8 00 01
// version=4 ihl=5 dscp=0 ecn=0 total=60 id=0x1c46 flags=2(DF) frag=0 ttl=64 proto=6 csum=0xb1e6
// src=192.168.0.104 dst=192.168.0.1
const std::uint8_t kGolden[20] = {0x45, 0x00, 0x00, 0x3c, 0x1c, 0x46, 0x40, 0x00, 0x40, 0x06,
                                  0xb1, 0xe6, 0xc0, 0xa8, 0x00, 0x68, 0xc0, 0xa8, 0x00, 0x01};

}  // namespace

int main() {
    // ---- T1: bit-field + scalar oracle ----
    nanotins::struct_view<Ipv4Spec> v(kGolden);
    CHECK(v("version"_fld) == 4);
    CHECK(v("ihl"_fld) == 5);
    CHECK(v("dscp"_fld) == 0);
    CHECK(v("ecn"_fld) == 0);
    CHECK(v("total_length"_fld) == 60);
    CHECK(v("identification"_fld) == 0x1c46);
    CHECK(v("flags"_fld) == 2);
    CHECK(v("frag_offset"_fld) == 0);
    CHECK(v("ttl"_fld) == 64);
    CHECK(v("protocol"_fld) == 6);
    CHECK(v("checksum"_fld) == 0xb1e6);
    CHECK(v("src_addr"_fld) == 0xc0a80068u);
    CHECK(v("dst_addr"_fld) == 0xc0a80001u);

    // device-style direct reads (the GPU path), bit-field + scalar
    using FVer = std::tuple_element_t<0, nanotins::spec_fields_t<Ipv4Spec>>;
    using FFlags = std::tuple_element_t<6, nanotins::spec_fields_t<Ipv4Spec>>;
    CHECK(nanotins::read_field<FVer>(kGolden) == 4);
    CHECK(nanotins::read_field<FFlags>(kGolden) == 2);

    // cross-check against the existing soatins bits<>/be<> overlay on the same bytes
    const protocols::Ipv4* oh = reinterpret_cast<const protocols::Ipv4*>(kGolden);
    CHECK(((oh->ver_ihl.word_host() >> 4) & 0x0F) == v("version"_fld));
    CHECK((oh->ver_ihl.word_host() & 0x0F) == v("ihl"_fld));
    CHECK(((oh->flags_frag.word_host() >> 13) & 0x07) == v("flags"_fld));
    CHECK((oh->flags_frag.word_host() & 0x1FFF) == v("frag_offset"_fld));
    CHECK(oh->total_length.host() == v("total_length"_fld));
    CHECK(oh->identification.host() == v("identification"_fld));
    CHECK(oh->ttl == v("ttl"_fld));
    CHECK(oh->protocol == v("protocol"_fld));
    CHECK(oh->checksum.host() == v("checksum"_fld));
    CHECK(std::memcmp(oh->src.data(), kGolden + 12, 4) == 0);

    // ---- T2: deterministic bulk over varied headers (exercise bit-field columns) ----
    constexpr std::size_t N = 256;
    std::vector<std::uint8_t> hdrs(N * 20);
    for (std::size_t i = 0; i < N; ++i) {
        std::uint8_t* h = &hdrs[i * 20];
        std::memcpy(h, kGolden, 20);
        const std::uint8_t ihl = static_cast<std::uint8_t>(5 + (i % 3));  // 5/6/7 -> varies the low nibble
        h[0] = static_cast<std::uint8_t>(0x40 | ihl);                     // version stays 4
        h[4] = static_cast<std::uint8_t>(i >> 8);                         // identification = i (BE)
        h[5] = static_cast<std::uint8_t>(i);
        h[9] = static_cast<std::uint8_t>(i & 0xFF);                       // protocol = i & 0xFF
    }

    nanotins::spec_soa<Ipv4Spec, N> seq;
    for (std::size_t i = 0; i < N; ++i) {
        seq.append(&hdrs[i * 20]);
    }
    nanotins::spec_soa<Ipv4Spec, N> blk;
    {
        exec::static_thread_pool pool(8);
        auto p = blk.raw();
        const std::uint8_t* base = hdrs.data();
        nanotins::bulk_for_each(pool.get_scheduler(), 16, N, [=](std::size_t i) {
            nanotins::scatter_spec<Ipv4Spec>(p, i, base + i * 20);
        });
    }
    for (std::size_t i = 0; i < N; ++i) {
        CHECK(seq.column<0>()[i] == blk.column<0>()[i]);  // version
        CHECK(seq.column<1>()[i] == blk.column<1>()[i]);  // ihl
        CHECK(seq.column<5>()[i] == blk.column<5>()[i]);  // identification
        CHECK(seq.column<9>()[i] == blk.column<9>()[i]);  // protocol
        CHECK(seq.column<0>()[i] == 4);
        CHECK(seq.column<1>()[i] == static_cast<std::uint8_t>(5 + (i % 3)));
        CHECK(seq.column<5>()[i] == static_cast<std::uint16_t>(i));
        CHECK(seq.column<9>()[i] == static_cast<std::uint8_t>(i & 0xFF));
    }

    // ---- T3: spec_soa (with bit-field columns) -> Arrow batch ----
    ArrowArray arr{};
    std::string err;
    CHECK(nanotins::to_arrow_spec(seq, arr, err));
    CHECK(arr.length == static_cast<std::int64_t>(N));
    CHECK(arr.n_children == 13);
    const auto* c_ver = static_cast<const std::uint8_t*>(arr.children[0]->buffers[1]);   // version (u8)
    const auto* c_ihl = static_cast<const std::uint8_t*>(arr.children[1]->buffers[1]);   // ihl (u8)
    const auto* c_id = static_cast<const std::uint16_t*>(arr.children[5]->buffers[1]);   // identification (u16)
    for (std::size_t i = 0; i < N; ++i) {
        CHECK(c_ver[i] == 4);
        CHECK(c_ihl[i] == static_cast<std::uint8_t>(5 + (i % 3)));
        CHECK(c_id[i] == static_cast<std::uint16_t>(i));
    }
    if (arr.release) arr.release(&arr);

    // ---- T4: a parsed field decides the extent — header length = ihl * 4 ----
    CHECK(v("ihl"_fld) * 4u == 20u);  // no options
    std::uint8_t with_opts[24];
    std::memcpy(with_opts, kGolden, 20);
    with_opts[0] = 0x46;  // ihl = 6 -> 24-byte header
    nanotins::struct_view<Ipv4Spec> vo(with_opts);
    CHECK(vo("ihl"_fld) * 4u == 24u);

    std::printf("struct_spec_ipv4: ok (T1 bitfield oracle vs soatins, T2 bulk==seq, T3 arrow, T4 ihl extent)\n");
    return 0;
}
