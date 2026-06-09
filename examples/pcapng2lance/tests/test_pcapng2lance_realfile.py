#!/usr/bin/env python3
"""Real-capture end-to-end test for pcapng2lance.

Runs the converter on a real pcap/pcapng file (argv[2]), then validates the dataset against an
INDEPENDENT pure-Python pcapng/pcap walker (argv-driven, not the C++ seam) — so this catches driver
bugs the seam's own tests can't (the seam can't be its own oracle). Checks, per packet row:
  row count, ts_raw, caplen, origlen, denormalized link_type/ts_resol, and that the external
  (position, size) resolves to exactly the source bytes.

argv: <pcapng2lance-exe> <capture-file>.  CTest treats exit 77 as skipped (pylance missing)."""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def walk_capture(raw: bytes):
    """Independent oracle -> (section_count, packets[dict]). pcap + pcapng.

    Crucially section-aware: a pcapng may concatenate several sections (one SHB each); the interface
    table resets per SHB and interface_id is section-relative. Each packet carries its OWN resolved
    link_type / ts_resol so the check is correct even across heterogeneous sections."""
    lead = struct.unpack_from("<I", raw, 0)[0]
    packets: list[dict] = []

    if lead in (0xA1B2C3D4, 0xD4C3B2A1, 0xA1B23C4D, 0x4D3CB2A1):  # legacy pcap
        le = lead in (0xA1B2C3D4, 0xA1B23C4D)
        nanos = lead in (0xA1B23C4D, 0x4D3CB2A1)
        e = "<" if le else ">"
        link_type = struct.unpack_from(e + "I", raw, 20)[0]
        ts_resol = 9 if nanos else 6
        off = 24
        while off + 16 <= len(raw):
            ts_sec, ts_frac, incl, orig = struct.unpack_from(e + "IIII", raw, off)
            packets.append({"off": off + 16, "caplen": incl, "origlen": orig,
                            "ts_raw": (ts_sec << 32) | ts_frac, "link_type": link_type, "ts_resol": ts_resol})
            off += 16 + incl
        return 1, packets

    # pcapng
    le = True
    off = 0
    n = len(raw)
    sections = 0
    cur: list[tuple[int, int]] = []  # current section's interface table
    while off + 12 <= n:
        btype = struct.unpack_from("<I", raw, off)[0]
        if btype == 0x0A0D0D0A:  # SHB: new section, reset interface table
            le = struct.unpack_from("<I", raw, off + 8)[0] == 0x1A2B3C4D
            cur = []
            sections += 1
        e = "<" if le else ">"
        total = struct.unpack_from(e + "I", raw, off + 4)[0]
        if total < 12 or off + total > n:
            break
        if btype == 0x00000001:  # IDB
            link_type = struct.unpack_from(e + "H", raw, off + 8)[0]
            ts_resol = 6
            opt, end = off + 16, off + total - 4
            while opt + 4 <= end:
                code, length = struct.unpack_from(e + "HH", raw, opt)
                if code == 0:
                    break
                if code == 9 and length >= 1:
                    ts_resol = raw[opt + 4]
                opt += 4 + ((length + 3) // 4) * 4
            cur.append((link_type, ts_resol))
        elif btype == 0x00000006:  # EPB
            iface, ts_high, ts_low, caplen, origlen = struct.unpack_from(e + "IIIII", raw, off + 8)
            link_type, ts_resol = cur[iface] if iface < len(cur) else (0, 6)
            packets.append({"off": off + 8 + 20, "caplen": caplen, "origlen": origlen,
                            "ts_raw": (ts_high << 32) | ts_low, "link_type": link_type, "ts_resol": ts_resol})
        off += total
    return sections, packets


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print("usage: test_pcapng2lance_realfile.py <exe> <capture>", file=sys.stderr)
        return 2
    exe, capture = argv[1], argv[2]
    try:
        import lance  # noqa: F401
    except Exception as exc:  # pragma: no cover - environment dependent
        print(f"realfile interop skipped: {exc}", file=sys.stderr)
        return 77

    raw = Path(capture).read_bytes()
    sections, packets = walk_capture(raw)
    assert packets, "oracle found no packets in the capture"

    with tempfile.TemporaryDirectory() as tmp:
        dataset = str(Path(tmp) / "out.lance")
        result = subprocess.run([exe, capture, dataset], capture_output=True, text=True)
        if result.returncode != 0:
            print(f"converter failed: {result.stderr}", file=sys.stderr)
            return 1

        table = lance.dataset(dataset).to_table()
        assert table.num_rows == len(packets), f"row count {table.num_rows} != oracle {len(packets)}"

        ts_raw = table.column("ts_raw").to_pylist()
        caplen = table.column("caplen").to_pylist()
        origlen = table.column("origlen").to_pylist()
        link_type = table.column("link_type").to_pylist()
        ts_resol = table.column("ts_resol").to_pylist()
        payload = table.column("payload_ref").combine_chunks()
        positions = payload.field("position").to_pylist()
        sizes = payload.field("size").to_pylist()

        for i, pk in enumerate(packets):
            assert ts_raw[i] == pk["ts_raw"], (i, "ts_raw", ts_raw[i], pk["ts_raw"])
            assert caplen[i] == pk["caplen"], (i, "caplen")
            assert origlen[i] == pk["origlen"], (i, "origlen")
            assert link_type[i] == pk["link_type"], (i, "link_type", link_type[i], pk["link_type"])
            assert ts_resol[i] == pk["ts_resol"], (i, "ts_resol", ts_resol[i], pk["ts_resol"])
            assert sizes[i] == pk["caplen"] and positions[i] == pk["off"], (i, "external ref")
            assert raw[positions[i] : positions[i] + sizes[i]] == raw[pk["off"] : pk["off"] + pk["caplen"]]

    print(f"pcapng2lance realfile ok ({len(packets)} packets across {sections} section(s) "
          f"cross-checked vs independent oracle)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
