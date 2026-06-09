#!/usr/bin/env python3
"""Stock-Lance (pylance) interop + on-disk offset verification for pcapng2lance.

Builds a pcapng fixture, runs the converter (argv[1] = exe path), then reads the dataset with the
Rust-backed `lance` package and checks:
  - scalar columns (ts_raw / caplen / link_type / ts_resol / epb_flags),
  - the external payload_ref: each row's stored (blob_uri, position, size) must resolve to exactly the
    source file's bytes (the whole premise of the external-blob design).

CTest treats exit code 77 as "skipped" (pylance not installed)."""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def build_pcapng() -> tuple[bytes, list[bytes]]:
    def block(btype: int, body: bytes) -> bytes:
        total = 12 + ((len(body) + 3) // 4) * 4
        pad = b"\x00" * (total - 12 - len(body))
        return struct.pack("<II", btype, total) + body + pad + struct.pack("<I", total)

    def epb(iface: int, ts: int, caplen: int, origlen: int, data: bytes, flags: int) -> bytes:
        dpad = b"\x00" * ((((len(data) + 3) // 4) * 4) - len(data))
        return (
            struct.pack("<IIIII", iface, ts >> 32, ts & 0xFFFFFFFF, caplen, origlen)
            + data
            + dpad
            + struct.pack("<HHI", 2, 4, flags)  # epb_flags option
            + struct.pack("<HH", 0, 0)  # opt_endofopt
        )

    shb = struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1) + struct.pack("<HH", 0, 0)
    idb = (
        struct.pack("<HHI", 1, 0, 65535)
        + struct.pack("<HH", 9, 1)
        + bytes([6, 0, 0, 0])  # if_tsresol = 6 (microseconds), padded
        + struct.pack("<HH", 0, 0)
    )
    p0 = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x01])
    p1 = bytes([0xCA, 0xFE, 0xBA, 0xBE, 0x02, 0x03, 0x04])
    out = (
        block(0x0A0D0D0A, shb)
        + block(0x00000001, idb)
        + block(0x00000006, epb(0, 0x0000000100000002, len(p0), len(p0), p0, 0x00000001))
        + block(0x00000006, epb(0, 0x00000003AABBCCDD, len(p1), 99, p1, 0))
    )
    return out, [p0, p1]


def build_multisection() -> tuple[bytes, list[tuple[int, int]]]:
    """Two sections with DIFFERENT link_type/ts_resol; each packet uses interface_id 0 (section-relative).
    This distinguishes correct per-section interface resolution from buggy global accumulation: with the
    old global table, the second section's packet would inherit section 0's link_type."""
    def block(bt: int, body: bytes) -> bytes:
        total = 12 + ((len(body) + 3) // 4) * 4
        return struct.pack("<II", bt, total) + body + b"\x00" * (total - 12 - len(body)) + struct.pack("<I", total)

    def shb() -> bytes:
        return block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1) + struct.pack("<HH", 0, 0))

    def idb(link: int, tsresol: int) -> bytes:
        body = (struct.pack("<HHI", link, 0, 65535) + struct.pack("<HH", 9, 1) + bytes([tsresol, 0, 0, 0]) +
                struct.pack("<HH", 0, 0))
        return block(0x00000001, body)

    def epb(data: bytes) -> bytes:
        dpad = b"\x00" * ((((len(data) + 3) // 4) * 4) - len(data))
        body = struct.pack("<IIIII", 0, 0, 0, len(data), len(data)) + data + dpad + struct.pack("<HH", 0, 0)
        return block(0x00000006, body)

    sec0 = shb() + idb(1, 6) + epb(b"\xaa\xbb") + epb(b"\xcc\xdd\xee")  # link 1, microseconds
    sec1 = shb() + idb(147, 9) + epb(b"\x11\x22\x33\x44")              # link 147, nanoseconds
    expect = [(1, 6), (1, 6), (147, 9)]  # (link_type, ts_resol) per packet, in order
    return sec0 + sec1, expect


def build_many(n: int) -> bytes:
    """A pcapng with `n` tiny EPBs (one section). With enough rows the blob column's packed total
    exceeds 65535, forcing the wide32 control-buffer path — a stock-Lance read then proves wide32 is
    on-disk-compatible (not just nanolance-internal)."""
    def block(bt: int, body: bytes) -> bytes:
        total = 12 + ((len(body) + 3) // 4) * 4
        return struct.pack("<II", bt, total) + body + b"\x00" * (total - 12 - len(body)) + struct.pack("<I", total)

    shb = block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1) + struct.pack("<HH", 0, 0))
    idb = block(0x00000001, struct.pack("<HHI", 1, 0, 65535) + struct.pack("<HH", 0, 0))
    out = [shb, idb]
    for i in range(n):
        data = bytes([i & 0xFF, (i >> 8) & 0xFF])
        body = struct.pack("<IIIII", 0, 0, i, len(data), len(data)) + data + b"\x00\x00" + struct.pack("<HH", 0, 0)
        out.append(block(0x00000006, body))
    return b"".join(out)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: test_pcapng2lance_interop.py <pcapng2lance-exe>", file=sys.stderr)
        return 2
    exe = argv[1]
    try:
        import lance  # noqa: F401
        import pyarrow as pa  # noqa: F401
    except Exception as exc:  # pragma: no cover - environment dependent
        print(f"pcapng2lance interop skipped: {exc}", file=sys.stderr)
        return 77

    fixture_bytes, payloads = build_pcapng()
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        fixture = tmp_path / "capture.pcapng"
        fixture.write_bytes(fixture_bytes)
        dataset = tmp_path / "out.lance"

        result = subprocess.run([exe, str(fixture), str(dataset)], capture_output=True, text=True)
        if result.returncode != 0:
            print(f"converter failed: {result.stderr}", file=sys.stderr)
            return 1

        ds = lance.dataset(str(dataset))
        table = ds.to_table()
        assert table.num_rows == 2, f"expected 2 rows, got {table.num_rows}"

        ts_raw = table.column("ts_raw").to_pylist()
        caplen = table.column("caplen").to_pylist()
        link_type = table.column("link_type").to_pylist()
        ts_resol = table.column("ts_resol").to_pylist()
        epb_flags = table.column("epb_flags").to_pylist()
        assert ts_raw[0] == 0x0000000100000002, ts_raw
        assert caplen == [len(payloads[0]), len(payloads[1])], caplen
        assert link_type == [1, 1], link_type
        assert ts_resol == [6, 6], ts_resol
        assert epb_flags == [1, 0], epb_flags

        payload = table.column("payload_ref").combine_chunks()
        positions = payload.field("position").to_pylist()
        sizes = payload.field("size").to_pylist()
        uris = payload.field("blob_uri").to_pylist()

        raw = fixture.read_bytes()
        for i, expected in enumerate(payloads):
            assert sizes[i] == len(expected), (i, sizes[i])
            got = raw[positions[i] : positions[i] + sizes[i]]
            assert got == expected, f"row {i} payload mismatch: {got!r} != {expected!r}"
            assert uris[i].endswith("capture.pcapng"), uris[i]

        # Multi-section: per-SHB interface reset; section-relative interface_id must denormalize per section.
        ms_bytes, expect = build_multisection()
        fixture2 = tmp_path / "multi.pcapng"
        fixture2.write_bytes(ms_bytes)
        dataset2 = tmp_path / "multi.lance"
        result2 = subprocess.run([exe, str(fixture2), str(dataset2)], capture_output=True, text=True)
        if result2.returncode != 0:
            print(f"multisection converter failed: {result2.stderr}", file=sys.stderr)
            return 1
        table2 = lance.dataset(str(dataset2)).to_table()
        got = list(zip(table2.column("link_type").to_pylist(), table2.column("ts_resol").to_pylist()))
        assert got == expect, f"per-section denormalization wrong: {got} != {expect}"

        # Many rows -> blob packed total > 65535 -> wide32 control buffer. Stock Lance must read it.
        n = 4000
        fixture3 = tmp_path / "many.pcapng"
        fixture3.write_bytes(build_many(n))
        dataset3 = tmp_path / "many.lance"
        result3 = subprocess.run([exe, str(fixture3), str(dataset3)], capture_output=True, text=True)
        if result3.returncode != 0:
            print(f"many-row converter failed: {result3.stderr}", file=sys.stderr)
            return 1
        table3 = lance.dataset(str(dataset3)).to_table()
        assert table3.num_rows == n, f"wide32: {table3.num_rows} != {n}"
        payload3 = table3.column("payload_ref").combine_chunks()
        pos3 = payload3.field("position").to_pylist()
        sz3 = payload3.field("size").to_pylist()
        cap3 = table3.column("caplen").to_pylist()
        # Every external ref must still be exact and in-bounds after the wide32 round-trip.
        assert all(sz3[i] == cap3[i] == 2 for i in range(n)), "wide32 sizes wrong"
        assert pos3 == sorted(pos3) and len(set(pos3)) == n, "wide32 offsets not monotonic/unique"

    print("pcapng2lance interop ok (stock lance read + external offsets + per-section + wide32 verified)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
