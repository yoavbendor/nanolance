#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Generate ipv4_options_sample.pcap — the IPv4-options fixture for the tshark golden test
(test_nlance2table_tshark_ipv4_options.py). Built with scapy so the capture is encoded by an INDEPENDENT
implementation (not nanotins), then dissected by tshark (another independent implementation) and compared
to nanotins' ipv4_option child table. Regenerate with:  python gen_ipv4_options_fixture.py

Every packet's option area is already a multiple of 4 bytes (no scapy auto-padding), and the single packet
that uses End-of-Option-List puts EOL as the final byte (no trailing padding), so the option sequence is
unambiguous across scapy / tshark / nanotins. Type 30 (0x1e) is an unassigned option type — scapy leaves it
opaque (no per-type dissection quirks) and tshark still reports its type byte under ip.opt.type.

Contents:
  p0  IPv4 + Router Alert (type 148, len 4) + UDP
  p1  IPv4 + no options + TCP                            (control: an IPv4 row but zero option rows)
  p2  IPv4 + a 4-byte option (type 30) + UDP
  p3  IPv4 + NOP, NOP, NOP, EOL (4 bytes) + TCP          (single-byte markers + EOL terminator)
  p4  IPv4 + Router Alert (4) + an 8-byte option (type 30) + UDP   (two options in one header)
"""
from pathlib import Path

from scapy.layers.l2 import Ether
from scapy.layers.inet import IP, UDP, TCP, IPOption, IPOption_Router_Alert, IPOption_NOP, IPOption_EOL
from scapy.utils import wrpcap

EM = dict(src="02:00:00:00:00:01", dst="02:00:00:00:00:02")  # explicit MACs (no interface lookup)
S, D = "10.0.0.1", "10.0.0.2"

OPT4 = IPOption(b"\x1e\x04\xaa\xbb")                  # type 30, len 4 (opaque -> 2 value bytes)
OPT8 = IPOption(b"\x1e\x08\x01\x02\x03\x04\x05\x06")  # type 30, len 8 (opaque -> 6 value bytes)


def main() -> None:
    pkts = [
        Ether(**EM) / IP(src=S, dst=D, options=[IPOption_Router_Alert()]) / UDP(sport=1111, dport=2222),
        Ether(**EM) / IP(src=S, dst=D) / TCP(sport=3333, dport=80),
        Ether(**EM) / IP(src=S, dst=D, options=[OPT4]) / UDP(sport=53, dport=54),
        Ether(**EM) / IP(src=S, dst=D,
                         options=[IPOption_NOP(), IPOption_NOP(), IPOption_NOP(), IPOption_EOL()]) /
            TCP(sport=9, dport=10),
        Ether(**EM) / IP(src=S, dst=D, options=[IPOption_Router_Alert(), OPT8]) / UDP(sport=21, dport=22),
    ]
    out = Path(__file__).with_name("ipv4_options_sample.pcap")
    wrpcap(str(out), [bytes(p) and p for p in pkts])  # force-build each packet, then write
    print(f"wrote {len(pkts)} packets -> {out}")


if __name__ == "__main__":
    main()
