#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Generate srv6_sample.pcap — the IPv6 extension-header / SRv6 fixture for the tshark golden test
(test_nlance2table_tshark_srv6.py). Built with scapy so the capture is encoded by an INDEPENDENT
implementation (not nanotins), then dissected by tshark (another independent implementation) and compared
to nanotins' tables. Regenerate with:  python gen_srv6_fixture.py

Contents (no fragments — tshark withholds L4 on fragments pending reassembly; fragment fields are covered by
the nanotins unit tests instead):
  p0  IPv6 + SRH(2 segments, tag) + UDP
  p1  IPv6 + SRH(3 segments, tag) + UDP
  p2  IPv6 + Hop-by-Hop(PadN) + TCP
  p3  IPv6 + Destination-Options(PadN) + UDP
  p4  IPv6 + SRH(2 segments) + Hop-by-Hop(PadN) + TCP   (two ext headers chained)
"""
from pathlib import Path

from scapy.layers.l2 import Ether
from scapy.layers.inet import UDP, TCP
from scapy.layers.inet6 import (IPv6, IPv6ExtHdrSegmentRouting, IPv6ExtHdrHopByHop,
                                IPv6ExtHdrDestOpt, IPv6ExtHdrSegmentRoutingTLVHMAC, PadN)
from scapy.utils import wrpcap

EM = dict(src="02:00:00:00:00:01", dst="02:00:00:00:00:02")  # explicit MACs (no interface lookup)
S, D = "2001:db8::1", "2001:db8::2"


def main() -> None:
    pkts = [
        Ether(**EM) / IPv6(src=S, dst=D) /
            IPv6ExtHdrSegmentRouting(addresses=["2001:db8::aa", "2001:db8::bb"], segleft=2, lastentry=1,
                                     tag=0xbeef) / UDP(sport=1111, dport=2222),
        Ether(**EM) / IPv6(src=S, dst=D) /
            IPv6ExtHdrSegmentRouting(addresses=["2001:db8::a", "2001:db8::b", "2001:db8::c"], segleft=3,
                                     lastentry=2, tag=0x1234) / UDP(sport=53, dport=54),
        Ether(**EM) / IPv6(src=S, dst=D) /
            IPv6ExtHdrHopByHop(options=[PadN(optdata=b"\x00\x00")]) / TCP(sport=3333, dport=80),
        Ether(**EM) / IPv6(src=S, dst=D) /
            IPv6ExtHdrDestOpt(options=[PadN(optdata=b"\x00\x00")]) / UDP(sport=7, dport=8),
        Ether(**EM) / IPv6(src=S, dst=D) /
            IPv6ExtHdrSegmentRouting(addresses=["2001:db8::1a", "2001:db8::1b"], segleft=2, lastentry=1,
                                     tag=0x4321) /
            IPv6ExtHdrHopByHop(options=[PadN(optdata=b"\x00\x00")]) / TCP(sport=9, dport=10),
        # p5: SRH carrying an HMAC TLV (exercises the SRH-TLV path -> ipv6_option container 43).
        Ether(**EM) / IPv6(src=S, dst=D) /
            IPv6ExtHdrSegmentRouting(addresses=["2001:db8::aa", "2001:db8::bb"], segleft=2, lastentry=1,
                                     tag=0x55aa, hmac=1,
                                     tlv_objects=[IPv6ExtHdrSegmentRoutingTLVHMAC()]) / UDP(sport=21, dport=22),
        # p6: three stacked extension headers (Hop-by-Hop + Routing/SRH + Destination Options) then TCP.
        Ether(**EM) / IPv6(src=S, dst=D) /
            IPv6ExtHdrHopByHop(options=[PadN(optdata=b"\x00\x00")]) /
            IPv6ExtHdrSegmentRouting(addresses=["2001:db8::c1", "2001:db8::c2"], segleft=2, lastentry=1,
                                     tag=0x0c0c) /
            IPv6ExtHdrDestOpt(options=[PadN(optdata=b"\x00\x00")]) / TCP(sport=31, dport=32),
    ]
    out = Path(__file__).with_name("srv6_sample.pcap")
    wrpcap(str(out), [bytes(p) and p for p in pkts])  # force-build each packet, then write
    print(f"wrote {len(pkts)} packets -> {out}")


if __name__ == "__main__":
    main()
