#!/usr/bin/env python3
"""Chunked enrich (sized from a memory budget, big-tile fetch + carve) must equal single-chunk enrich.

Builds a multi-protocol Ethernet capture, runs l1 once, then enriches it two ways: with the default
(one-chunk) budget and with a tiny --mem-bytes + --read-tile-bytes that forces many small chunks/tiles
(hence many fragments). Asserts every per-PDU table and the remainder table are identical between the two
runs, and that the chunked run really did split into multiple fragments. argv[1] = exe. CTest 77 = skip."""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def block(bt, body):
    total = 12 + ((len(body) + 3) // 4) * 4
    return struct.pack("<II", bt, total) + body + b"\x00" * (total - 12 - len(body)) + struct.pack("<I", total)


def epb(data):
    dpad = b"\x00" * ((((len(data) + 3) // 4) * 4) - len(data))
    return block(0x00000006, struct.pack("<IIIII", 0, 0, 0, len(data), len(data)) + data + dpad)


def eth(ethertype):
    return bytes(range(0x10, 0x16)) + bytes(range(0x20, 0x26)) + struct.pack(">H", ethertype)


def ipv4(proto, payload_len, i):
    src = struct.pack(">I", 0x0A000000 + i)
    dst = struct.pack(">I", 0xC0A80000 + i)
    return (bytes([0x45, 0x00]) + struct.pack(">HHH", 20 + payload_len, i & 0xFFFF, 0x4000) + bytes([0x40, proto]) +
            struct.pack(">H", 0) + src + dst)


def ipv6(nxt, payload_len):
    s = bytes([0x20, 0x01]) + b"\x00" * 13 + b"\x01"
    d = bytes([0x20, 0x01]) + b"\x00" * 13 + b"\x02"
    return struct.pack(">I", 0x60000000) + struct.pack(">H", payload_len) + bytes([nxt, 0x40]) + s + d


def udp(sport, dport, payload):
    return struct.pack(">HHHH", sport, dport, 8 + len(payload), 0) + payload


def tcp(sport, dport, payload):
    return struct.pack(">HHII", sport, dport, 1, 2) + struct.pack(">HHHH", 0x5018, 0x1F40, 0, 0) + payload


def vlan(vid, inner):
    return struct.pack(">HH", (5 << 13) | (1 << 12) | vid, inner)


def build(count=40):
    shb = block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1) + struct.pack("<HH", 0, 0))
    idb = block(0x00000001, struct.pack("<HHI", 1, 0, 65535) + struct.pack("<HH", 0, 0))
    out = [shb, idb]
    for i in range(count):
        pl = bytes([i & 0xFF]) * (i % 7)  # variable-length app payloads
        kind = i % 4
        if kind == 0:
            f = eth(0x0800) + ipv4(17, 8 + len(pl), i) + udp(1000 + i, 2000, pl)
        elif kind == 1:
            f = eth(0x0800) + ipv4(6, 20 + len(pl), i) + tcp(1100 + i, 80, pl)
        elif kind == 2:
            f = eth(0x86DD) + ipv6(17, 8 + len(pl)) + udp(3000 + i, 4000, pl)
        else:
            f = eth(0x8100) + vlan(100 + i, 0x0800) + ipv4(17, 8 + len(pl), i) + udp(5000 + i, 53, pl)
        out.append(epb(f))
    return b"".join(out)


def main(argv):
    if len(argv) < 2:
        print("usage: test_pcapng2lance_enrich_chunking.py <exe>", file=sys.stderr)
        return 2
    exe = argv[1]
    try:
        import lance
    except Exception as exc:  # pragma: no cover - environment dependent
        print(f"enrich-chunking interop skipped: {exc}", file=sys.stderr)
        return 77

    def run(args):
        r = subprocess.run([exe, *args], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"{args} failed: {r.stderr}")

    def snapshot(table_dir: Path):
        t = lance.dataset(str(table_dir)).to_table()
        cols = {name: t.column(name).to_pylist() for name in t.schema.names if name != "payload_ref"}
        rows = [tuple(cols[name][i] for name in sorted(cols)) for i in range(t.num_rows)]
        if "payload_ref" in t.schema.names:
            p = t.column("payload_ref").combine_chunks()
            pos, sz = p.field("position").to_pylist(), p.field("size").to_pylist()
            pid = t.column("packet_id").to_pylist()
            rows = [r + (pid[i], pos[i], sz[i]) for i, r in enumerate(rows)]
        return sorted(rows)  # order-independent compare

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        fixture = tmp_path / "cap.pcapng"
        fixture.write_bytes(build())
        whole = tmp_path / "whole"
        chunked = tmp_path / "chunked"

        for data in (whole, chunked):
            run(["--stage", "l1", str(fixture), str(data)])
        for st in ("l2", "l3", "l4"):
            run(["--stage", st, str(whole)])  # default budget -> one chunk
            # Tiny budget + tile -> N=1 row/chunk, per-row fetch -> many fragments.
            run(["--stage", st, "--mem-bytes", "1", "--read-tile-bytes", "8", str(chunked)])

        tables = ["ethernet.lance", "vlan.lance", "ipv4.lance", "ipv6.lance", "tcp.lance", "udp.lance",
                  "remainder_after_l2.lance", "remainder_after_l3.lance", "remainder_after_l4.lance"]
        multi_fragment = 0
        for name in tables:
            w, c = whole / name, chunked / name
            assert w.exists() and c.exists(), f"missing {name}"
            assert snapshot(w) == snapshot(c), f"chunked enrich differs from whole for {name}"
            frags = len(lance.dataset(str(c)).get_fragments())
            if frags > 1:
                multi_fragment += 1
        assert multi_fragment > 0, "chunked enrich never produced multiple fragments"

    print(f"pcapng2lance enrich-chunking ok (per-PDU + remainder tables identical; "
          f"{multi_fragment} tables multi-fragment when chunked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
