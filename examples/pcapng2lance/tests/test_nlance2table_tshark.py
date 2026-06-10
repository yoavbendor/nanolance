#!/usr/bin/env python3
"""Alignment harness: our --decode-l2l3 PDU tables (dumped via nlance2table) must agree field-for-field
with Wireshark's `tshark` dissection of the SAME pcapng. This is the cross-check that proves the nanotins
decode matches an independent, authoritative dissector.

Crafts a small Ethernet pcapng (IPv4/UDP, IPv4/TCP, IPv6/UDP, VLAN+IPv4/UDP), runs pcapng2lance
--decode-l2l3, dumps each per-PDU table as NDJSON with nlance2table (keyed by packet_id), runs
`tshark -T fields` on the same file, normalizes both sides to a common representation (IP/MAC -> lowercase
hex), and asserts equality for a curated field set per packet.

argv[1] = nlance2table exe, argv[2] = pcapng2lance exe. Skips (77) if tshark is not found on PATH."""

import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def block(bt: int, body: bytes) -> bytes:
    total = 12 + ((len(body) + 3) // 4) * 4
    return struct.pack("<II", bt, total) + body + b"\x00" * (total - 12 - len(body)) + struct.pack("<I", total)


def epb(data: bytes) -> bytes:
    dpad = b"\x00" * ((((len(data) + 3) // 4) * 4) - len(data))
    return block(0x00000006, struct.pack("<IIIII", 0, 0, 0, len(data), len(data)) + data + dpad)


def eth(dst: bytes, src: bytes, ethertype: int) -> bytes:
    return dst + src + struct.pack(">H", ethertype)


def ipv4(proto: int, src: bytes, dst: bytes, payload_len: int) -> bytes:
    total = 20 + payload_len
    return (bytes([0x45, 0x00]) + struct.pack(">HHH", total, 0xABCD, 2 << 13) + bytes([0x40, proto]) +
            struct.pack(">H", 0) + src + dst)


def ipv6(nxt: int, src: bytes, dst: bytes, payload_len: int) -> bytes:
    return struct.pack(">I", 0x60000000) + struct.pack(">H", payload_len) + bytes([nxt, 0x40]) + src + dst


def udp(sport: int, dport: int, payload: bytes = b"") -> bytes:
    return struct.pack(">HHHH", sport, dport, 8 + len(payload), 0) + payload


def tcp(sport: int, dport: int) -> bytes:
    return struct.pack(">HHII", sport, dport, 1, 2) + struct.pack(">HHH", 0x5018, 0x1F40, 0) + struct.pack(">H", 0)


def vlan(vid: int, inner: int) -> bytes:
    tci = (5 << 13) | (1 << 12) | (vid & 0x0FFF)
    return struct.pack(">HH", tci, inner)


M1, M2 = bytes(range(0x10, 0x16)), bytes(range(0x20, 0x26))
IP_A = (b"\x01\x02\x03\x04", b"\x05\x06\x07\x08")
IP_B = (b"\x0a\x00\x00\x01", b"\x0a\x00\x00\x02")
IP_D = (b"\xc0\xa8\x01\x01", b"\xc0\xa8\x01\x02")
V6 = (bytes([0x20, 0x01]) + b"\x00" * 13 + b"\x01", bytes([0x20, 0x01]) + b"\x00" * 13 + b"\x02")


def build() -> bytes:
    shb = block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1) + struct.pack("<HH", 0, 0))
    idb = block(0x00000001, struct.pack("<HHI", 1, 0, 65535) + struct.pack("<HH", 0, 0))
    fa = eth(M1, M2, 0x0800) + ipv4(17, *IP_A, 8) + udp(1000, 2000)
    fb = eth(M1, M2, 0x0800) + ipv4(6, *IP_B, 20) + tcp(1111, 80)
    fc = eth(M1, M2, 0x86DD) + ipv6(17, *V6, 8) + udp(3000, 4000)
    fd = eth(M1, M2, 0x8100) + vlan(100, 0x0800) + ipv4(17, *IP_D, 8) + udp(5000, 53)
    return shb + idb + epb(fa) + epb(fb) + epb(fc) + epb(fd)


def ip4_hex(dotted: str) -> str:
    return "".join(f"{int(p):02x}" for p in dotted.split("."))


def mac_hex(colon: str) -> str:
    return colon.replace(":", "").lower()


def ip6_hex(addr: str) -> str:
    # tshark prints the full/compressed IPv6; expand via socket for a canonical 16-byte form.
    import socket
    return socket.inet_pton(socket.AF_INET6, addr).hex()


def n2t_rows(n2t: str, table: Path) -> dict:
    """Dump a PDU table as NDJSON, keyed by packet_id (last write wins; our tables are 1 PDU/packet here)."""
    if not table.exists():
        return {}
    nd = subprocess.run([n2t, "-f", "ndjson", str(table)], capture_output=True, text=True, check=True).stdout
    out = {}
    for ln in nd.splitlines():
        if ln:
            o = json.loads(ln)
            out[o["packet_id"]] = o
    return out


def tshark_fields(tshark: str, pcap: Path) -> list[dict]:
    fields = ["frame.number", "eth.dst", "eth.src", "eth.type", "vlan.id", "vlan.etype", "ip.src", "ip.dst",
              "ip.proto", "ipv6.src", "ipv6.dst", "ipv6.nxt", "udp.srcport", "udp.dstport", "tcp.srcport",
              "tcp.dstport"]
    cmd = [tshark, "-r", str(pcap), "-T", "fields"] + sum([["-e", f] for f in fields], []) + \
          ["-E", "separator=|", "-E", "occurrence=f"]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    rows = []
    for ln in out.splitlines():
        if not ln.strip():
            continue
        vals = ln.split("|")
        rows.append(dict(zip(fields, vals)))
    return rows


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("usage: test_nlance2table_tshark.py <nlance2table> <pcapng2lance>", file=sys.stderr)
        return 2
    n2t, conv = argv[1], argv[2]
    tshark = shutil.which("tshark") or (
        r"C:\Program Files\Wireshark\tshark.exe" if Path(r"C:\Program Files\Wireshark\tshark.exe").exists() else None)
    if not tshark:
        print("nlance2table/tshark alignment skipped: tshark not found", file=sys.stderr)
        return 77

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        fixture = tmp_path / "eth.pcapng"
        fixture.write_bytes(build())
        out = tmp_path / "out.lance"
        r = subprocess.run([conv, "--decode-l2l3", str(fixture), str(out)], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"converter failed: {r.stderr}", file=sys.stderr)
            return 1

        eth_t = n2t_rows(n2t, tmp_path / "out_ethernet.lance")
        ip4_t = n2t_rows(n2t, tmp_path / "out_ipv4.lance")
        ip6_t = n2t_rows(n2t, tmp_path / "out_ipv6.lance")
        udp_t = n2t_rows(n2t, tmp_path / "out_udp.lance")
        tcp_t = n2t_rows(n2t, tmp_path / "out_tcp.lance")
        vlan_t = n2t_rows(n2t, tmp_path / "out_vlan.lance")

        ref = tshark_fields(tshark, fixture)
        assert len(ref) == 4, f"expected 4 frames from tshark, got {len(ref)}"

        mismatches = []

        def check(cond, label):
            if not cond:
                mismatches.append(label)

        for tr in ref:
            pid = int(tr["frame.number"]) - 1  # tshark 1-based -> our 0-based packet_id

            # L2: Ethernet present for every frame; MAC + outer ethertype must match.
            assert pid in eth_t, f"no ethernet row for packet {pid}"
            e = eth_t[pid]
            check(e["dst"] == mac_hex(tr["eth.dst"]), f"pkt{pid} eth.dst {e['dst']} != {tr['eth.dst']}")
            check(e["src"] == mac_hex(tr["eth.src"]), f"pkt{pid} eth.src {e['src']} != {tr['eth.src']}")
            check(e["ethertype"] == int(tr["eth.type"], 16),
                  f"pkt{pid} ethertype {e['ethertype']} != {tr['eth.type']}")

            # VLAN (only the tagged frame).
            if tr.get("vlan.id"):
                assert pid in vlan_t, f"no vlan row for packet {pid}"
                check(vlan_t[pid]["vid"] == int(tr["vlan.id"]), f"pkt{pid} vlan.id")

            # L3 IPv4 / IPv6.
            if tr.get("ip.src"):
                assert pid in ip4_t, f"no ipv4 row for packet {pid}"
                v = ip4_t[pid]
                check(v["src"] == ip4_hex(tr["ip.src"]), f"pkt{pid} ip.src {v['src']} != {tr['ip.src']}")
                check(v["dst"] == ip4_hex(tr["ip.dst"]), f"pkt{pid} ip.dst {v['dst']} != {tr['ip.dst']}")
                check(v["protocol"] == int(tr["ip.proto"]), f"pkt{pid} ip.proto")
            if tr.get("ipv6.src"):
                assert pid in ip6_t, f"no ipv6 row for packet {pid}"
                v = ip6_t[pid]
                check(v["src"] == ip6_hex(tr["ipv6.src"]), f"pkt{pid} ipv6.src {v['src']} != {tr['ipv6.src']}")
                check(v["next_header"] == int(tr["ipv6.nxt"]), f"pkt{pid} ipv6.nxt")

            # L4 UDP / TCP.
            if tr.get("udp.dstport"):
                assert pid in udp_t, f"no udp row for packet {pid}"
                check(udp_t[pid]["dst_port"] == int(tr["udp.dstport"]), f"pkt{pid} udp.dstport")
                check(udp_t[pid]["src_port"] == int(tr["udp.srcport"]), f"pkt{pid} udp.srcport")
            if tr.get("tcp.dstport"):
                assert pid in tcp_t, f"no tcp row for packet {pid}"
                check(tcp_t[pid]["dst_port"] == int(tr["tcp.dstport"]), f"pkt{pid} tcp.dstport")

        if mismatches:
            print("FIELD MISMATCHES vs tshark:", file=sys.stderr)
            for m in mismatches:
                print("  " + m, file=sys.stderr)
            return 1

    print("nlance2table/tshark alignment ok (eth/vlan/ipv4/ipv6/tcp/udp fields match Wireshark)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
