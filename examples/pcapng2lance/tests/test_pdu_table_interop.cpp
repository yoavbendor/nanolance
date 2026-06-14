// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// M6: the DAG-driven PDU tables must be BYTE-IDENTICAL to the current protocols::PduColumn tables — same
// columns (name + Arrow type + order) and the same data buffers — so swapping the driver's decode path to
// the DAG produces the same Lance files. We build both Arrow record batches for the same packets (the old
// path via build_pdu_batch<T>, the spec path via build_dag_pdu_batch<Spec>) and compare schema + data
// buffers via ArrowArrayView. If the buffers match, the Lance output matches.

#include "dag_table_writer.hpp"
#include "pdu_table_writer.hpp"

#include "nanotins/dag_decode.hpp"
#include "nanotins/protocol_decode.hpp"
#include "nanotins/protocol_specs.hpp"
#include "nanotins/spec_dag.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using G = nanotins::L2L3Graph;

namespace {

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    b[off] = static_cast<std::uint8_t>(v >> 8);
    b[off + 1] = static_cast<std::uint8_t>(v & 0xFF);
}

// Two packets covering all six PDU types, with non-trivial field values so the data buffers are meaningful.
std::vector<std::uint8_t> pkt_eth_ipv4_udp() {
    std::vector<std::uint8_t> b(42, 0);
    for (std::size_t i = 0; i < 12; ++i) b[i] = static_cast<std::uint8_t>(0x10 + i);  // MACs
    put16(b, 12, 0x0800);
    b[14] = 0x45;          // version/ihl
    b[14 + 1] = 0xB8;      // dscp/ecn
    put16(b, 14 + 2, 1500);
    put16(b, 14 + 4, 0xABCD);  // identification
    b[14 + 8] = 64;            // ttl
    b[14 + 9] = 17;            // protocol UDP
    for (std::size_t i = 0; i < 4; ++i) b[14 + 12 + i] = static_cast<std::uint8_t>(192 + i);  // src
    for (std::size_t i = 0; i < 4; ++i) b[14 + 16 + i] = static_cast<std::uint8_t>(10 + i);   // dst
    put16(b, 34, 0x1234);  // udp src
    put16(b, 36, 0x5678);  // udp dst
    put16(b, 38, 8);       // udp length
    return b;
}
std::vector<std::uint8_t> pkt_eth_vlan_ipv6_tcp() {
    std::vector<std::uint8_t> b(78, 0);
    put16(b, 12, 0x8100);
    put16(b, 14, 0x6005);  // VLAN TCI (pcp/dei/vid)
    put16(b, 16, 0x86DD);  // inner IPv6
    b[18] = 0x60;          // version 6
    b[18 + 6] = 6;         // next_header TCP
    b[18 + 7] = 64;        // hop limit
    for (std::size_t i = 0; i < 16; ++i) b[18 + 8 + i] = static_cast<std::uint8_t>(0x20 + i);   // src
    for (std::size_t i = 0; i < 16; ++i) b[18 + 24 + i] = static_cast<std::uint8_t>(0x40 + i);  // dst
    put16(b, 58, 0xABCD);  // tcp src
    put16(b, 60, 0xDCBA);  // tcp dst
    b[58 + 12] = 0x50;     // data_offset 5
    return b;
}

// Compare two finished record batches: same children count/length, same schema name+format per column, and
// the same data buffer (buffer index 1) byte-for-byte. Validity buffers are ignored (both are non-null).
bool batches_equal(const ArrowSchema& sa, const ArrowArray& aa, const ArrowSchema& sb, const ArrowArray& ab,
                   std::string& msg) {
    if (aa.n_children != ab.n_children) {
        msg = "n_children differ";
        return false;
    }
    if (aa.length != ab.length) {
        msg = "length differ";
        return false;
    }
    ArrowArrayView va{}, vb{};
    ArrowError err;
    if (ArrowArrayViewInitFromSchema(&va, const_cast<ArrowSchema*>(&sa), &err) != NANOARROW_OK) {
        msg = "view init a";
        return false;
    }
    if (ArrowArrayViewInitFromSchema(&vb, const_cast<ArrowSchema*>(&sb), &err) != NANOARROW_OK) {
        ArrowArrayViewReset(&va);
        msg = "view init b";
        return false;
    }
    bool ok = true;
    if (ArrowArrayViewSetArray(&va, const_cast<ArrowArray*>(&aa), &err) != NANOARROW_OK) {
        msg = "set array a";
        ok = false;
    }
    if (ok && ArrowArrayViewSetArray(&vb, const_cast<ArrowArray*>(&ab), &err) != NANOARROW_OK) {
        msg = "set array b";
        ok = false;
    }
    for (std::int64_t c = 0; ok && c < aa.n_children; ++c) {
        const char* na = sa.children[c]->name ? sa.children[c]->name : "";
        const char* nb = sb.children[c]->name ? sb.children[c]->name : "";
        if (std::strcmp(na, nb) != 0) {
            msg = std::string("column ") + std::to_string(c) + " name: '" + na + "' vs '" + nb + "'";
            ok = false;
            break;
        }
        if (std::strcmp(sa.children[c]->format, sb.children[c]->format) != 0) {
            msg = std::string("column '") + na + "' format: '" + sa.children[c]->format + "' vs '" +
                  sb.children[c]->format + "'";
            ok = false;
            break;
        }
        const ArrowBufferView da = va.children[c]->buffer_views[1];
        const ArrowBufferView db = vb.children[c]->buffer_views[1];
        if (da.size_bytes != db.size_bytes) {
            msg = std::string("column '") + na + "' data size: " + std::to_string(da.size_bytes) + " vs " +
                  std::to_string(db.size_bytes);
            ok = false;
            break;
        }
        if (da.size_bytes > 0 &&
            std::memcmp(da.data.data, db.data.data, static_cast<std::size_t>(da.size_bytes)) != 0) {
            msg = std::string("column '") + na + "' data bytes differ";
            ok = false;
            break;
        }
    }
    ArrowArrayViewReset(&va);
    ArrowArrayViewReset(&vb);
    return ok;
}

// Build the protocols::PduColumn<T> batch and the dag_pdu_table<Spec> batch and assert they are identical.
template <class T, class Spec>
void check_type(const char* name, const protocols::PduColumn<T>& col,
                const nanotins::dag_pdu_table<Spec>& tab) {
    CHECK(col.size() == tab.size());

    ArrowSchema sa{}, sb{};
    ArrowArray aa{}, ab{};
    std::string err;
    CHECK(pdu_io::build_pdu_schema<T>(sa, err));
    CHECK(pdu_io::build_pdu_batch<T>(sa, col, aa, err));
    CHECK(pdu_io::build_dag_pdu_schema<Spec>(sb, err));
    CHECK(pdu_io::build_dag_pdu_batch<Spec>(sb, tab, ab, err));

    std::string msg;
    if (!batches_equal(sa, aa, sb, ab, msg)) {
        std::fprintf(stderr, "FAIL %s: %s\n", name, msg.c_str());
        std::exit(1);
    }
    std::printf("  %-9s ok (%zu rows, %lld cols, identical schema + buffers)\n", name, tab.size(),
                static_cast<long long>(aa.n_children));

    aa.release(&aa);
    ab.release(&ab);
    sa.release(&sa);
    sb.release(&sb);
}

constexpr std::size_t kEth = nanotins::node_id_v<nanotins::EthNode, G>;
constexpr std::size_t kVlan = nanotins::node_id_v<nanotins::VlanNode, G>;
constexpr std::size_t kIpv4 = nanotins::node_id_v<nanotins::Ipv4Node, G>;
constexpr std::size_t kIpv6 = nanotins::node_id_v<nanotins::Ipv6Node, G>;
constexpr std::size_t kTcp = nanotins::node_id_v<nanotins::TcpNode, G>;
constexpr std::size_t kUdp = nanotins::node_id_v<nanotins::UdpNode, G>;

}  // namespace

int main() {
    const auto p0 = pkt_eth_ipv4_udp();
    const auto p1 = pkt_eth_vlan_ipv6_tcp();

    // old path: protocols::DecodedPdus
    protocols::DecodedPdus pdus;
    protocols::decode_packet(7, protocols::kLinkTypeEthernet, protocols::Bytes(p0.data(), p0.size()), pdus);
    protocols::decode_packet(8, protocols::kLinkTypeEthernet, protocols::Bytes(p1.data(), p1.size()), pdus);

    // spec path: DAG tables
    nanotins::dag_tables<G> tabs;
    nanotins::dag_decode_packet<G>(7, p0.data(), p0.size(), tabs, nanotins::kEthRoot);
    nanotins::dag_decode_packet<G>(8, p1.data(), p1.size(), tabs, nanotins::kEthRoot);

    check_type<protocols::Ethernet, nanotins::EthernetSpec>("ethernet", pdus.ethernet, std::get<kEth>(tabs));
    check_type<protocols::VlanTag, nanotins::VlanTagSpec>("vlan", pdus.vlan, std::get<kVlan>(tabs));
    check_type<protocols::Ipv4, nanotins::Ipv4Spec>("ipv4", pdus.ipv4, std::get<kIpv4>(tabs));
    check_type<protocols::Ipv6, nanotins::Ipv6Spec>("ipv6", pdus.ipv6, std::get<kIpv6>(tabs));
    check_type<protocols::Tcp, nanotins::TcpSpec>("tcp", pdus.tcp, std::get<kTcp>(tabs));
    check_type<protocols::Udp, nanotins::UdpSpec>("udp", pdus.udp, std::get<kUdp>(tabs));

    std::printf("pdu_table_interop: ok (DAG PDU tables byte-identical to protocols:: tables)\n");
    return 0;
}
