#!/usr/bin/env python3
"""nlance2table smoke: convert a crafted Ethernet pcapng with --decode-l2l3, then dump the per-PDU Lance
tables with nlance2table and validate the CSV / NDJSON text directly (no pylance needed).

Checks: CSV header columns, row counts, --limit caps the row count, fixed_size_binary renders as lowercase
hex, the nested payload_ref struct flattens to dotted CSV columns, and every NDJSON line parses as JSON.
argv[1] = nlance2table exe, argv[2] = pcapng2lance exe. Pure self-check — returns 0/1 (no 77 skip)."""

import json
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
    total = 20 + payload_len
    return (bytes([0x45, 0x00]) + struct.pack(">HHH", total, 0xABCD, flags << 13) + bytes([0x40, proto]) +
            struct.pack(">H", 0) + src + dst)


def udp(sport: int, dport: int, payload: bytes = b"") -> bytes:
    return struct.pack(">HHHH", sport, dport, 8 + len(payload), 0) + payload


def build() -> bytes:
    shb = block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1) + struct.pack("<HH", 0, 0))
    idb = block(0x00000001, struct.pack("<HHI", 1, 0, 65535) + struct.pack("<HH", 0, 0))  # Ethernet
    m1, m2 = bytes(range(0x10, 0x16)), bytes(range(0x20, 0x26))
    frames = [
        eth(m1, m2, 0x0800) + ipv4(17, b"\x01\x02\x03\x04", b"\x05\x06\x07\x08", 8) + udp(1000, 2000),
        eth(m1, m2, 0x0800) + ipv4(17, b"\x0a\x00\x00\x01", b"\x0a\x00\x00\x02", 8) + udp(1001, 2001),
        eth(m1, m2, 0x0800) + ipv4(17, b"\xc0\xa8\x01\x01", b"\xc0\xa8\x01\x02", 8) + udp(1002, 2002),
    ]
    return shb + idb + b"".join(epb(f) for f in frames)


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("usage: test_nlance2table.py <nlance2table> <pcapng2lance>", file=sys.stderr)
        return 2
    n2t, conv = argv[1], argv[2]

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        fixture = tmp_path / "eth.pcapng"
        fixture.write_bytes(build())
        out = tmp_path / "out.lance"
        r = subprocess.run([conv, "--decode-l2l3", str(fixture), str(out)], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"converter failed: {r.stderr}", file=sys.stderr)
            return 1

        ipv4_tbl = str(tmp_path / "out_ipv4.lance")

        # --- CSV: header, row count, fixed_size_binary hex ---
        csv = subprocess.run([n2t, "-f", "csv", ipv4_tbl], capture_output=True, text=True, check=True).stdout
        lines = [ln for ln in csv.splitlines() if ln]
        header = lines[0].split(",")
        assert header[0] == "packet_id", header
        assert "src" in header and "dst" in header and "protocol" in header, header
        assert len(lines) - 1 == 3, f"expected 3 ipv4 rows, got {len(lines) - 1}"
        rows = [dict(zip(header, ln.split(","))) for ln in lines[1:]]
        assert rows[0]["src"] == "01020304" and rows[0]["dst"] == "05060708", rows[0]
        assert rows[2]["dst"] == "c0a80102", rows[2]  # 192.168.1.2 -> hex
        assert all(r["protocol"] == "17" for r in rows), rows
        assert [r["packet_id"] for r in rows] == ["0", "1", "2"], rows

        # --- CSV --limit caps rows ---
        csv2 = subprocess.run([n2t, "-f", "csv", "-n", "2", ipv4_tbl], capture_output=True, text=True,
                              check=True).stdout
        assert len([ln for ln in csv2.splitlines() if ln]) - 1 == 2, "limit should cap to 2 rows"

        # --- NDJSON: each line valid JSON, values typed ---
        nd = subprocess.run([n2t, "-f", "ndjson", ipv4_tbl], capture_output=True, text=True, check=True).stdout
        objs = [json.loads(ln) for ln in nd.splitlines() if ln]
        assert len(objs) == 3, len(objs)
        assert objs[0]["src"] == "01020304" and objs[0]["protocol"] == 17, objs[0]
        assert objs[0]["version"] == 4 and objs[0]["ihl"] == 5, objs[0]

        # --- nested payload_ref struct: dotted CSV columns + nested NDJSON object on the L1 table ---
        l1 = subprocess.run([n2t, "-f", "csv", "-n", "1", str(out)], capture_output=True, text=True,
                            check=True).stdout
        l1_header = l1.splitlines()[0].split(",")
        assert "payload_ref.uri" in l1_header and "payload_ref.size" in l1_header, l1_header
        l1nd = subprocess.run([n2t, "-f", "ndjson", "-n", "1", str(out)], capture_output=True, text=True,
                              check=True).stdout
        o = json.loads(l1nd.splitlines()[0])
        assert isinstance(o["payload_ref"], dict) and o["payload_ref"]["data"] is None, o

    print("nlance2table smoke ok (csv/ndjson, fixed_size_binary hex, --limit, nested struct flatten)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
