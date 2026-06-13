// M4a: the backbone protocol_specs (Ethernet/VLAN/IPv4/IPv6/TCP/UDP) must read every field identically to
// the existing protocols:: be<>/bits<> overlay on the same bytes — the guarantee that a struct_spec DAG
// will emit byte-identical Lance tables. Cross-checks struct_view<Spec> vs the reinterpreted struct over
// deterministic bytes (the values are arbitrary; agreement is the point).

#include "nanotins/protocol_specs.hpp"
#include "nanotins/protocols.hpp"
#include "nanotins/struct_spec.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

std::vector<std::uint8_t> bytes(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
    }
    return v;
}

}  // namespace

int main() {
    // ---- Ethernet ----
    {
        const auto b = bytes(14);
        nanotins::struct_view<nanotins::EthernetSpec> v(b.data());
        const auto* o = reinterpret_cast<const protocols::Ethernet*>(b.data());
        CHECK(std::memcmp(v("dst"_fld).data(), o->dst.data(), 6) == 0);
        CHECK(std::memcmp(v("src"_fld).data(), o->src.data(), 6) == 0);
        CHECK(v("ethertype"_fld) == o->ethertype.host());
    }

    // ---- VLAN tag (bits) ----
    {
        const auto b = bytes(4);
        nanotins::struct_view<nanotins::VlanTagSpec> v(b.data());
        const auto* o = reinterpret_cast<const protocols::VlanTag*>(b.data());
        const std::uint16_t tci = o->tci.word_host();
        CHECK(v("pcp"_fld) == ((tci >> 13) & 0x7));
        CHECK(v("dei"_fld) == ((tci >> 12) & 0x1));
        CHECK(v("vid"_fld) == (tci & 0xFFF));
        CHECK(v("inner_ethertype"_fld) == o->inner_ethertype.host());
    }

    // ---- IPv4 (bits + FSB addrs) ----
    {
        const auto b = bytes(20);
        nanotins::struct_view<nanotins::Ipv4Spec> v(b.data());
        const auto* o = reinterpret_cast<const protocols::Ipv4*>(b.data());
        CHECK(v("version"_fld) == ((o->ver_ihl.word_host() >> 4) & 0xF));
        CHECK(v("ihl"_fld) == (o->ver_ihl.word_host() & 0xF));
        CHECK(v("dscp"_fld) == ((o->dscp_ecn.word_host() >> 2) & 0x3F));
        CHECK(v("ecn"_fld) == (o->dscp_ecn.word_host() & 0x3));
        CHECK(v("total_length"_fld) == o->total_length.host());
        CHECK(v("identification"_fld) == o->identification.host());
        CHECK(v("flags"_fld) == ((o->flags_frag.word_host() >> 13) & 0x7));
        CHECK(v("frag_offset"_fld) == (o->flags_frag.word_host() & 0x1FFF));
        CHECK(v("ttl"_fld) == o->ttl);
        CHECK(v("protocol"_fld) == o->protocol);
        CHECK(v("checksum"_fld) == o->checksum.host());
        CHECK(std::memcmp(v("src"_fld).data(), o->src.data(), 4) == 0);
        CHECK(std::memcmp(v("dst"_fld).data(), o->dst.data(), 4) == 0);
    }

    // ---- IPv6 (byte-crossing bits + 16B FSB addrs) ----
    {
        const auto b = bytes(40);
        nanotins::struct_view<nanotins::Ipv6Spec> v(b.data());
        const auto* o = reinterpret_cast<const protocols::Ipv6*>(b.data());
        const std::uint32_t vtf = o->vtf.word_host();
        CHECK(v("version"_fld) == ((vtf >> 28) & 0xF));
        CHECK(v("traffic_class"_fld) == ((vtf >> 20) & 0xFF));
        CHECK(v("flow_label"_fld) == (vtf & 0xFFFFF));
        CHECK(v("payload_length"_fld) == o->payload_length.host());
        CHECK(v("next_header"_fld) == o->next_header);
        CHECK(v("hop_limit"_fld) == o->hop_limit);
        CHECK(std::memcmp(v("src"_fld).data(), o->src.data(), 16) == 0);
        CHECK(std::memcmp(v("dst"_fld).data(), o->dst.data(), 16) == 0);
    }

    // ---- TCP (bits) ----
    {
        const auto b = bytes(20);
        nanotins::struct_view<nanotins::TcpSpec> v(b.data());
        const auto* o = reinterpret_cast<const protocols::Tcp*>(b.data());
        const std::uint16_t of = o->off_flags.word_host();
        CHECK(v("src_port"_fld) == o->src_port.host());
        CHECK(v("dst_port"_fld) == o->dst_port.host());
        CHECK(v("seq"_fld) == o->seq.host());
        CHECK(v("ack"_fld) == o->ack.host());
        CHECK(v("data_offset"_fld) == ((of >> 12) & 0xF));
        CHECK(v("reserved"_fld) == ((of >> 9) & 0x7));
        CHECK(v("flags"_fld) == (of & 0x1FF));
        CHECK(v("window"_fld) == o->window.host());
        CHECK(v("checksum"_fld) == o->checksum.host());
        CHECK(v("urgent_ptr"_fld) == o->urgent_ptr.host());
    }

    // ---- UDP ----
    {
        const auto b = bytes(8);
        nanotins::struct_view<nanotins::UdpSpec> v(b.data());
        const auto* o = reinterpret_cast<const protocols::Udp*>(b.data());
        CHECK(v("src_port"_fld) == o->src_port.host());
        CHECK(v("dst_port"_fld) == o->dst_port.host());
        CHECK(v("length"_fld) == o->length.host());
        CHECK(v("checksum"_fld) == o->checksum.host());
    }

    std::printf("protocol_specs: ok (eth/vlan/ipv4/ipv6/tcp/udp specs == protocols:: be<>/bits<> overlay)\n");
    return 0;
}
