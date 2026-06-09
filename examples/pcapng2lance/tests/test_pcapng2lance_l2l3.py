#!/usr/bin/env python3
"""M3 end-to-end: convert an Ethernet pcapng with --decode-l2l3 and verify the per-PDU Lance tables.

Crafts four frames (IPv4/UDP, IPv4/TCP, IPv6/UDP, VLAN+IPv4/UDP), runs the converter, then reads the
<stem>_<pdu>.lance tables with stock pylance and checks row counts, packet_id linkage, decoded scalar
fields and bitfields, and fixed-size-binary address columns. argv[1] = exe. CTest 77 = skipped."""

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


def ipv4(proto: int, src: bytes, dst: bytes, payload_len: int, flags: int = 2) -> bytes:
    ver_ihl = 0x45
    total = 20 + payload_len
    flags_frag = (flags << 13) | 0
    return (bytes([ver_ihl, 0x00]) + struct.pack(">HHH", total, 0xABCD, flags_frag) + bytes([0x40, proto]) +
            struct.pack(">H", 0) + src + dst)


def ipv6(nxt: int, src: bytes, dst: bytes, payload_len: int) -> bytes:
    return struct.pack(">I", 0x60000000) + struct.pack(">H", payload_len) + bytes([nxt, 0x40]) + src + dst


def udp(sport: int, dport: int, payload: bytes = b"") -> bytes:
    return struct.pack(">HHHH", sport, dport, 8 + len(payload), 0) + payload


def tcp(sport: int, dport: int) -> bytes:
    return (struct.pack(">HHII", sport, dport, 1, 2) + struct.pack(">HHH", 0x5018, 0x1F40, 0) + struct.pack(">H", 0))


def vlan(vid: int, inner: int) -> bytes:
    tci = (5 << 13) | (1 << 12) | (vid & 0x0FFF)  # pcp=5, dei=1
    return struct.pack(">HH", tci, inner)


def build() -> bytes:
    shb = block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1) + struct.pack("<HH", 0, 0))
    idb = block(0x00000001, struct.pack("<HHI", 1, 0, 65535) + struct.pack("<HH", 0, 0))  # link_type 1 = Ethernet
    m1, m2 = bytes(range(0x10, 0x16)), bytes(range(0x20, 0x26))
    ip_a = (b"\x01\x02\x03\x04", b"\x05\x06\x07\x08")
    ip_b = (b"\x0a\x00\x00\x01", b"\x0a\x00\x00\x02")
    ip_d = (b"\xc0\xa8\x01\x01", b"\xc0\xa8\x01\x02")
    v6 = (bytes([0x20, 0x01]) + b"\x00" * 13 + b"\x01", bytes([0x20, 0x01]) + b"\x00" * 13 + b"\x02")

    fa = eth(m1, m2, 0x0800) + ipv4(17, *ip_a, 8) + udp(1000, 2000)
    fb = eth(m1, m2, 0x0800) + ipv4(6, *ip_b, 20) + tcp(1111, 80)
    fc = eth(m1, m2, 0x86DD) + ipv6(17, *v6, 8) + udp(3000, 4000)
    fd = eth(m1, m2, 0x8100) + vlan(100, 0x0800) + ipv4(17, *ip_d, 8) + udp(5000, 53)
    return shb + idb + epb(fa) + epb(fb) + epb(fc) + epb(fd)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: test_pcapng2lance_l2l3.py <exe>", file=sys.stderr)
        return 2
    exe = argv[1]
    try:
        import lance  # noqa: F401
    except Exception as exc:  # pragma: no cover - environment dependent
        print(f"l2l3 interop skipped: {exc}", file=sys.stderr)
        return 77

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        fixture = tmp_path / "eth.pcapng"
        fixture.write_bytes(build())
        out = tmp_path / "out.lance"
        result = subprocess.run([exe, "--decode-l2l3", str(fixture), str(out)], capture_output=True, text=True)
        if result.returncode != 0:
            print(f"converter failed: {result.stderr}", file=sys.stderr)
            return 1

        def table(suffix):
            return lance.dataset(str(tmp_path / f"out_{suffix}.lance")).to_table()

        eth_t = table("ethernet")
        assert eth_t.num_rows == 4, eth_t.num_rows
        assert eth_t.column("ethertype").to_pylist() == [0x0800, 0x0800, 0x86DD, 0x8100]
        assert eth_t.column("packet_id").to_pylist() == [0, 1, 2, 3]
        assert eth_t.column("dst").to_pylist()[0] == bytes(range(0x10, 0x16))

        ipv4_t = table("ipv4")
        assert ipv4_t.num_rows == 3, ipv4_t.num_rows
        assert ipv4_t.column("packet_id").to_pylist() == [0, 1, 3]
        assert ipv4_t.column("protocol").to_pylist() == [17, 6, 17]
        assert ipv4_t.column("version").to_pylist() == [4, 4, 4]
        assert ipv4_t.column("ihl").to_pylist() == [5, 5, 5]
        assert ipv4_t.column("flags").to_pylist() == [2, 2, 2]  # DF
        assert ipv4_t.column("src").to_pylist() == [b"\x01\x02\x03\x04", b"\x0a\x00\x00\x01", b"\xc0\xa8\x01\x01"]

        ipv6_t = table("ipv6")
        assert ipv6_t.num_rows == 1 and ipv6_t.column("packet_id").to_pylist() == [2]
        assert ipv6_t.column("version").to_pylist() == [6]
        assert ipv6_t.column("next_header").to_pylist() == [17]

        tcp_t = table("tcp")
        assert tcp_t.num_rows == 1 and tcp_t.column("packet_id").to_pylist() == [1]
        assert tcp_t.column("dst_port").to_pylist() == [80]
        assert tcp_t.column("data_offset").to_pylist() == [5]

        udp_t = table("udp")
        assert udp_t.num_rows == 3 and udp_t.column("packet_id").to_pylist() == [0, 2, 3]
        assert udp_t.column("dst_port").to_pylist() == [2000, 4000, 53]

        vlan_t = table("vlan")
        assert vlan_t.num_rows == 1 and vlan_t.column("packet_id").to_pylist() == [3]
        assert vlan_t.column("vid").to_pylist() == [100]
        assert vlan_t.column("pcp").to_pylist() == [5]
        assert vlan_t.column("inner_ethertype").to_pylist() == [0x0800]

    print("pcapng2lance l2l3 ok (eth/vlan/ipv4/ipv6/tcp/udp tables verified via stock lance)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
