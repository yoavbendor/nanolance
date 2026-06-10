// M3a: prove the L2/L3 wire structs overlay real packet bytes correctly — endianness, byte-straddling
// bitfields (IPv4 flags/frag, IPv6 version/tc/flow, TCP data_offset/flags), and that the reflection
// core expands each struct into the expected flat column list.

#include "nanotins/reflect.hpp"
#include "nanotins/protocols.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace protocols;

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "protocols test failed: %s\n", msg);
        std::exit(1);
    }
}

template <class T>
std::vector<std::string> column_names() {
    std::vector<std::string> names;
    nanotins::for_each_column<T>([&]<std::size_t I, class Col>() { names.emplace_back(Col::name()); });
    return names;
}

}  // namespace

int main() {
    // Ethernet + IPv4(UDP) frame.
    const std::vector<std::uint8_t> eth_ipv4_udp = {
        // Ethernet: dst, src, ethertype 0x0800
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x08, 0x00,
        // IPv4: ver/ihl=0x45, dscp/ecn=0xB8, total_len=0x0023, id=0xABCD, flags/frag=0x4000 (DF),
        //       ttl=0x40, proto=0x11(UDP), csum=0x0000, src=192.168.0.1, dst=10.0.0.2
        0x45, 0xB8, 0x00, 0x23, 0xAB, 0xCD, 0x40, 0x00, 0x40, 0x11, 0x00, 0x00, 0xC0, 0xA8, 0x00, 0x01,
        0x0A, 0x00, 0x00, 0x02,
        // UDP: src=0x04D2(1234), dst=0x0035(53), len=0x000F, csum=0x1234
        0x04, 0xD2, 0x00, 0x35, 0x00, 0x0F, 0x12, 0x34};
    Bytes f(eth_ipv4_udp.data(), eth_ipv4_udp.size());

    Ethernet eth{};
    require(overlay(f, 0, eth), "overlay eth");
    require(eth.dst[0] == 0x01 && eth.dst[5] == 0x06, "eth dst");
    require(eth.src[0] == 0xAA && eth.src[5] == 0xFF, "eth src");
    require(eth.ethertype.host() == kEtherTypeIpv4, "eth ethertype");

    Ipv4 ip{};
    require(overlay(f, 14, ip), "overlay ipv4");
    require(ip.ver_ihl.word_host() == 0x45, "ipv4 word");
    require(static_cast<unsigned>((ip.ver_ihl.word_host() >> 4) & 0xF) == 4, "ipv4 version");  // sanity
    require(ip.total_length.host() == 0x23, "ipv4 total_length");
    require(ip.identification.host() == 0xABCD, "ipv4 id");
    require(ip.ttl == 0x40, "ipv4 ttl");
    require(ip.protocol == kIpProtoUdp, "ipv4 protocol");
    require(ip.src[0] == 192 && ip.src[3] == 1, "ipv4 src");
    require(ip.dst[0] == 10 && ip.dst[3] == 2, "ipv4 dst");
    // bitfields via the column extractors (the path that feeds the SoA)
    {
        const auto cols = column_names<Ipv4>();
        const std::vector<std::string> expect = {"version", "ihl", "dscp", "ecn", "total_length",
                                                 "identification", "flags", "frag_offset", "ttl", "protocol",
                                                 "checksum", "src", "dst"};
        require(cols == expect, "ipv4 column expansion");
    }
    using nanotins::col_at;
    require(col_at<Ipv4, 0>::get(ip) == 4, "ipv4 version extract");      // version
    require(col_at<Ipv4, 1>::get(ip) == 5, "ipv4 ihl extract");         // ihl
    require(col_at<Ipv4, 2>::get(ip) == 46, "ipv4 dscp extract");       // dscp (EF)
    require(col_at<Ipv4, 3>::get(ip) == 0, "ipv4 ecn extract");         // ecn
    require(col_at<Ipv4, 6>::get(ip) == 2, "ipv4 flags extract");       // flags (DF)
    require(col_at<Ipv4, 7>::get(ip) == 0, "ipv4 frag extract");        // frag_offset

    Udp udp{};
    require(overlay(f, 14 + 20, udp), "overlay udp");
    require(udp.src_port.host() == 1234, "udp src_port");
    require(udp.dst_port.host() == 53, "udp dst_port");
    require(udp.length.host() == 0x000F, "udp length");
    require(udp.checksum.host() == 0x1234, "udp checksum");

    // IPv6 header: version=6, tc=0, flow=0x12345, payload_len=0x0010, next=UDP, hop=0x40.
    const std::vector<std::uint8_t> ipv6 = {
        0x60, 0x01, 0x23, 0x45, 0x00, 0x10, 0x11, 0x40,
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,  // src
        0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};  // dst
    Ipv6 v6{};
    require(overlay(Bytes(ipv6.data(), ipv6.size()), 0, v6), "overlay ipv6");
    require(col_at<Ipv6, 0>::get(v6) == 6, "ipv6 version");
    require(col_at<Ipv6, 1>::get(v6) == 0, "ipv6 traffic_class");
    require(col_at<Ipv6, 2>::get(v6) == 0x12345, "ipv6 flow_label");
    require(v6.payload_length.host() == 0x10, "ipv6 payload_length");
    require(v6.next_header == kIpProtoUdp, "ipv6 next_header");
    require(v6.src[0] == 0x20 && v6.src[15] == 1 && v6.dst[15] == 2, "ipv6 addrs");

    // TCP header: ports, seq, ack, data_offset=5, flags=0x018 (PSH|ACK), window=0x1F40.
    const std::vector<std::uint8_t> tcp = {0x04, 0xD2, 0x01, 0xBB, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
                                           0x00, 0x02, 0x50, 0x18, 0x1F, 0x40, 0xAB, 0xCD, 0x00, 0x00};
    Tcp t{};
    require(overlay(Bytes(tcp.data(), tcp.size()), 0, t), "overlay tcp");
    require(t.src_port.host() == 1234, "tcp src_port");
    require(t.dst_port.host() == 443, "tcp dst_port");
    require(t.seq.host() == 1, "tcp seq");
    require(t.ack.host() == 2, "tcp ack");
    require(col_at<Tcp, 4>::get(t) == 5, "tcp data_offset");   // data_offset
    require(col_at<Tcp, 6>::get(t) == 0x018, "tcp flags");     // flags (index: src,dst,seq,ack,data_offset,reserved,flags)
    require(t.window.host() == 0x1F40, "tcp window");

    // VLAN tag: pcp=5,dei=1,vid=0x064 over 0xBxxx, inner ethertype 0x0800.
    const std::vector<std::uint8_t> vlan = {0xB0, 0x64, 0x08, 0x00};
    VlanTag vt{};
    require(overlay(Bytes(vlan.data(), vlan.size()), 0, vt), "overlay vlan");
    require(col_at<VlanTag, 0>::get(vt) == 5, "vlan pcp");       // pcp
    require(col_at<VlanTag, 1>::get(vt) == 1, "vlan dei");       // dei
    require(col_at<VlanTag, 2>::get(vt) == 0x064, "vlan vid");   // vid
    require(vt.inner_ethertype.host() == kEtherTypeIpv4, "vlan inner ethertype");

    std::puts("protocols overlay/reflection test ok");
    return 0;
}
