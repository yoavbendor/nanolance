#!/usr/bin/env python3
"""Staged / incremental parsing: prove work can be done in parts and added to the data folder later.

Runs the converter four times on the SAME data folder:
  --stage l1  -> packets.lance (+ payload_ref external blob = whole payload)
  --stage l2  -> ethernet/vlan tables + remainder_after_l2 (advanced past L2)
  --stage l3  -> ipv4/ipv6 tables + remainder_after_l3 (advanced past L3)
  --stage l4  -> tcp/udp tables + remainder_after_l4 (= the application payload, still external)
Each stage only reads the previous tables + fetches referenced bytes; nothing is recomputed and no
payload bytes are copied. Verifies the per-stage tables and that the FINAL remainder references resolve
to exactly the application payloads in the original capture. argv[1] = exe. CTest 77 = skipped."""

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


def eth(ethertype: int) -> bytes:
    return bytes(range(0x10, 0x16)) + bytes(range(0x20, 0x26)) + struct.pack(">H", ethertype)


def ipv4(proto: int, src: bytes, dst: bytes, payload_len: int) -> bytes:
    return (bytes([0x45, 0x00]) + struct.pack(">HHH", 20 + payload_len, 0xABCD, 0x4000) + bytes([0x40, proto]) +
            struct.pack(">H", 0) + src + dst)


def ipv6(nxt: int, payload_len: int) -> bytes:
    s = bytes([0x20, 0x01]) + b"\x00" * 13 + b"\x01"
    d = bytes([0x20, 0x01]) + b"\x00" * 13 + b"\x02"
    return struct.pack(">I", 0x60000000) + struct.pack(">H", payload_len) + bytes([nxt, 0x40]) + s + d


def udp(sport: int, dport: int, payload: bytes) -> bytes:
    return struct.pack(">HHHH", sport, dport, 8 + len(payload), 0) + payload


def tcp(sport: int, dport: int, payload: bytes) -> bytes:
    return struct.pack(">HHII", sport, dport, 1, 2) + struct.pack(">HHHH", 0x5018, 0x1F40, 0, 0) + payload


def vlan(vid: int, inner: int) -> bytes:
    return struct.pack(">HH", (5 << 13) | (1 << 12) | vid, inner)


def build():
    shb = block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1) + struct.pack("<HH", 0, 0))
    idb = block(0x00000001, struct.pack("<HHI", 1, 0, 65535) + struct.pack("<HH", 0, 0))
    a_pl, c_pl, d_pl = b"HELLO", b"WORLD!", b"VLANPAYLOAD"
    fa = eth(0x0800) + ipv4(17, b"\x01\x02\x03\x04", b"\x05\x06\x07\x08", 8 + len(a_pl)) + udp(1000, 2000, a_pl)
    fb = eth(0x0800) + ipv4(6, b"\x0a\x00\x00\x01", b"\x0a\x00\x00\x02", 20) + tcp(1111, 80, b"")
    fc = eth(0x86DD) + ipv6(17, 8 + len(c_pl)) + udp(3000, 4000, c_pl)
    fd = (eth(0x8100) + vlan(100, 0x0800) + ipv4(17, b"\xc0\xa8\x01\x01", b"\xc0\xa8\x01\x02", 8 + len(d_pl)) +
          udp(5000, 53, d_pl))
    cap = shb + idb + epb(fa) + epb(fb) + epb(fc) + epb(fd)
    # Expected application payload per packet after L4. Packet B (TCP) is fully consumed -> no remainder
    # row (a blob.v2 external ref must be non-empty), so it is absent from remainder_after_l4.
    return cap, {0: a_pl, 2: c_pl, 3: d_pl}


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: test_pcapng2lance_staged.py <exe>", file=sys.stderr)
        return 2
    exe = argv[1]
    try:
        import lance  # noqa: F401
    except Exception as exc:  # pragma: no cover - environment dependent
        print(f"staged interop skipped: {exc}", file=sys.stderr)
        return 77

    cap, expect_payload = build()
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        fixture = tmp_path / "cap.pcapng"
        fixture.write_bytes(cap)
        data = tmp_path / "data"

        for st, args in (("l1", [str(fixture), str(data)]), ("l2", [str(data)]), ("l3", [str(data)]),
                         ("l4", [str(data)])):
            r = subprocess.run([exe, "--stage", st] + args, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"stage {st} failed: {r.stderr}", file=sys.stderr)
                return 1

        def table(name):
            return lance.dataset(str(data / f"{name}.lance")).to_table()

        # L1
        pkts = table("packets")
        assert pkts.num_rows == 4 and pkts.column("packet_id").to_pylist() == [0, 1, 2, 3]

        # L2
        assert table("ethernet").num_rows == 4
        assert table("vlan").num_rows == 1 and table("vlan").column("packet_id").to_pylist() == [3]
        r2 = table("remainder_after_l2")
        # next_protocol = ethertype per packet (rows ordered as decoded: A,B,C,D)
        assert dict(zip(r2.column("packet_id").to_pylist(), r2.column("next_protocol").to_pylist())) == {
            0: 0x0800, 1: 0x0800, 2: 0x86DD, 3: 0x0800}

        # L3
        assert table("ipv4").num_rows == 3 and table("ipv4").column("packet_id").to_pylist() == [0, 1, 3]
        assert table("ipv6").num_rows == 1 and table("ipv6").column("packet_id").to_pylist() == [2]
        r3 = table("remainder_after_l3")
        assert dict(zip(r3.column("packet_id").to_pylist(), r3.column("next_protocol").to_pylist())) == {
            0: 17, 1: 6, 2: 17, 3: 17}

        # L4
        assert table("tcp").num_rows == 1 and table("tcp").column("packet_id").to_pylist() == [1]
        assert table("udp").num_rows == 3 and table("udp").column("packet_id").to_pylist() == [0, 2, 3]

        # Final remainder = application payload, still external in the ORIGINAL capture file.
        r4 = table("remainder_after_l4")
        payload = r4.column("payload_ref").combine_chunks()
        pid = r4.column("packet_id").to_pylist()
        pos = payload.field("position").to_pylist()
        sz = payload.field("size").to_pylist()
        raw = fixture.read_bytes()
        got = {pid[i]: raw[pos[i]:pos[i] + sz[i]] for i in range(r4.num_rows)}
        assert got == expect_payload, f"final payloads {got} != {expect_payload}"

    print("pcapng2lance staged ok (l1->l2->l3->l4 incremental; final payloads external + exact)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
