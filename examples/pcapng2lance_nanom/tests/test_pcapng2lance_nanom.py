#!/usr/bin/env python3
"""Stock-Lance (pylance) interop + nanotins parity for pcapng2lance_nanom.

argv[1] = the nanom converter (pcapng2lance_nanom).
argv[2] = OPTIONAL: the nanotins converter (pcapng2lance). When present, the two datasets are asserted
          byte-for-byte identical (same schema + every column value, including the external payload_ref) —
          the concrete "nanom is a drop-in for nanotins on the L1 path" claim.

The nanom-only checks mirror test_pcapng2lance_interop.py: read the dataset back with the Rust-backed
`lance` package and verify the scalar columns and that each row's external (uri, position, size) resolves
to exactly the source file's bytes.

CTest treats exit code 77 as "skipped" (pylance not installed)."""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def block(btype: int, body: bytes) -> bytes:
    total = 12 + ((len(body) + 3) // 4) * 4
    pad = b"\x00" * (total - 12 - len(body))
    return struct.pack("<II", btype, total) + body + pad + struct.pack("<I", total)


def epb(iface: int, ts: int, caplen: int, origlen: int, data: bytes, flags: int) -> bytes:
    dpad = b"\x00" * ((((len(data) + 3) // 4) * 4) - len(data))
    return block(
        0x00000006,
        struct.pack("<IIIII", iface, ts >> 32, ts & 0xFFFFFFFF, caplen, origlen)
        + data
        + dpad
        + struct.pack("<HHI", 2, 4, flags)  # epb_flags option
        + struct.pack("<HH", 0, 0),  # opt_endofopt
    )


def build_pcapng() -> tuple[bytes, list[bytes]]:
    shb = block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1) + struct.pack("<HH", 0, 0))
    idb = block(
        0x00000001,
        struct.pack("<HHI", 1, 0, 65535)
        + struct.pack("<HH", 9, 1)
        + bytes([6, 0, 0, 0])  # if_tsresol = 6 (microseconds), padded
        + struct.pack("<HH", 0, 0),
    )
    p0 = bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x01])
    p1 = bytes([0xCA, 0xFE, 0xBA, 0xBE, 0x02, 0x03, 0x04])
    out = shb + idb
    out += epb(0, 0x0000000100000002, len(p0), len(p0), p0, 0x00000001)
    out += epb(0, 0x00000003AABBCCDD, len(p1), 99, p1, 0)
    return out, [p0, p1]


def build_multisection() -> tuple[bytes, list[tuple[int, int]]]:
    """Two sections with different link_type/ts_resol; each uses section-relative interface_id 0."""
    def shb() -> bytes:
        return block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1) + struct.pack("<HH", 0, 0))

    def idb(link: int, tsresol: int) -> bytes:
        return block(
            0x00000001,
            struct.pack("<HHI", link, 0, 65535)
            + struct.pack("<HH", 9, 1)
            + bytes([tsresol, 0, 0, 0])
            + struct.pack("<HH", 0, 0),
        )

    def pkt(data: bytes) -> bytes:
        dpad = b"\x00" * ((((len(data) + 3) // 4) * 4) - len(data))
        return block(
            0x00000006,
            struct.pack("<IIIII", 0, 0, 0, len(data), len(data)) + data + dpad + struct.pack("<HH", 0, 0),
        )

    sec0 = shb() + idb(1, 6) + pkt(b"\xaa\xbb") + pkt(b"\xcc\xdd\xee")  # link 1, microseconds
    sec1 = shb() + idb(147, 9) + pkt(b"\x11\x22\x33\x44")  # link 147, nanoseconds
    return sec0 + sec1, [(1, 6), (1, 6), (147, 9)]


def build_pcap() -> tuple[bytes, list[bytes]]:
    """Classic little-endian microsecond pcap: global header + two records (exercises the pcap path)."""
    gh = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1)  # magic, ver, tz, sig, snap, link=1(eth)
    p0 = bytes([0x11, 0x22, 0x33])
    p1 = bytes([0x44, 0x55, 0x66, 0x77, 0x88])
    r0 = struct.pack("<IIII", 1, 2, len(p0), len(p0)) + p0
    r1 = struct.pack("<IIII", 3, 4, len(p1), len(p1)) + p1
    return gh + r0 + r1, [p0, p1]


def to_dict(dataset_path: str) -> dict:
    import lance

    return lance.dataset(dataset_path).to_table().to_pydict()


def run(exe: str, fixture: Path, dataset: Path, *flags: str) -> None:
    result = subprocess.run([exe, *flags, str(fixture), str(dataset)], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"converter failed ({exe} {' '.join(flags)}): {result.stderr}")


def build_mix_pcap() -> bytes:
    """One classic pcap exercising every walk_packet path: Eth/IPv4/TCP, Eth/IPv6/TCP, Eth/VLAN/IPv4/UDP."""
    def eth(et: int, pl: bytes) -> bytes:
        return bytes(range(6)) + bytes(range(10, 16)) + struct.pack(">H", et) + pl

    def ipv4(proto: int, src: bytes, dst: bytes, pl: bytes) -> bytes:
        return (
            bytes([0x45, 0])
            + struct.pack(">HHH", 20 + len(pl), 1, 0)
            + bytes([64, proto, 0, 0])
            + src
            + dst
            + pl
        )

    def ipv6(nh: int, src: bytes, dst: bytes, pl: bytes) -> bytes:
        return struct.pack(">I", 6 << 28) + struct.pack(">H", len(pl)) + bytes([nh, 64]) + src + dst + pl

    def tcp(sp: int, dp: int, pl: bytes) -> bytes:
        return struct.pack(">HHIIHHHH", sp, dp, 1, 2, 5 << 12, 8192, 0, 0) + pl

    def udp(sp: int, dp: int, pl: bytes) -> bytes:
        return struct.pack(">HHHH", sp, dp, 8 + len(pl), 0) + pl

    pkts = [
        eth(0x0800, ipv4(6, bytes([10, 0, 0, 1]), bytes([10, 0, 0, 2]), tcp(1234, 80, b"hi"))),
        eth(0x86DD, ipv6(6, bytes(range(16)), bytes(range(16, 32)), tcp(5555, 443, b"yo"))),
        eth(0x8100, struct.pack(">HH", 1, 0x0800) + ipv4(17, bytes([1, 2, 3, 4]), bytes([5, 6, 7, 8]), udp(53, 99, b"dns"))),
    ]
    gh = struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1)
    recs = b"".join(struct.pack("<IIII", i, 0, len(p), len(p)) + p for i, p in enumerate(pkts))
    return gh + recs


PDU_TABLES = ["ethernet", "vlan", "ipv4", "ipv6", "tcp", "udp"]


def decode_and_check(nm_exe, nt_exe, tmp_path: Path) -> None:
    """--decode-l2l3 on the mixed capture: the six PDU tables must read back, and (when the nanotins
    converter is present) be byte-for-byte identical to its per-PDU tables, path for path."""
    import lance

    fixture = tmp_path / "mix.pcap"
    fixture.write_bytes(build_mix_pcap())
    nm_out = tmp_path / "mix_nm.lance"
    run(nm_exe, fixture, nm_out, "--decode-l2l3")

    def tables(stem: Path) -> dict:
        out = {}
        for t in PDU_TABLES:
            p = stem.with_name(stem.stem + f"_{t}.lance")
            out[t] = lance.dataset(str(p)).to_table().to_pydict() if p.exists() else None
        return out

    nm = tables(nm_out)
    # Every visited protocol produced a table with a packet_id join column.
    assert nm["tcp"] and nm["tcp"]["src_port"] == [1234, 5555], nm["tcp"]
    assert nm["ipv6"] and nm["ipv6"]["next_header"] == [6], nm["ipv6"]
    assert nm["udp"] and nm["udp"]["dst_port"] == [99], nm["udp"]
    assert nm["vlan"] and nm["vlan"]["vid"] == [1], nm["vlan"]

    if nt_exe:
        nt_out = tmp_path / "mix_nt.lance"
        run(nt_exe, fixture, nt_out, "--decode-l2l3")
        nt = tables(nt_out)
        for t in PDU_TABLES:
            assert (nm[t] is None) == (nt[t] is None), f"decode {t}: table presence differs"
            if nm[t] is None:
                continue
            assert list(nm[t].keys()) == list(nt[t].keys()), f"decode {t}: schema {nm[t].keys()} != {nt[t].keys()}"
            for k in nt[t]:
                assert nm[t][k] == nt[t][k], f"decode {t}: column '{k}' differs (nanom vs nanotins)"


def check_dataset(dataset: Path, fixture: Path, payloads: list[bytes]) -> None:
    import lance

    table = lance.dataset(str(dataset)).to_table()
    assert table.num_rows == len(payloads), f"rows {table.num_rows} != {len(payloads)}"
    raw = fixture.read_bytes()
    payload = table.column("payload_ref").combine_chunks()
    positions = payload.field("position").to_pylist()
    sizes = payload.field("size").to_pylist()
    uris = payload.field("blob_uri").to_pylist()
    for i, expected in enumerate(payloads):
        assert sizes[i] == len(expected), (i, sizes[i], len(expected))
        got = raw[positions[i] : positions[i] + sizes[i]]
        assert got == expected, f"row {i} external bytes mismatch: {got!r} != {expected!r}"
        assert uris[i].endswith(fixture.name), uris[i]


def assert_identical(nm_ds: Path, nt_ds: Path, label: str) -> None:
    nm, nt = to_dict(str(nm_ds)), to_dict(str(nt_ds))
    assert list(nm.keys()) == list(nt.keys()), f"{label}: schema mismatch {nm.keys()} != {nt.keys()}"
    for k in nt:
        assert nm[k] == nt[k], f"{label}: column '{k}' differs (nanom vs nanotins)"


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: test_pcapng2lance_nanom.py <pcapng2lance_nanom-exe> [pcapng2lance-exe]", file=sys.stderr)
        return 2
    nm_exe = argv[1]
    nt_exe = argv[2] if len(argv) > 2 else None
    try:
        import lance  # noqa: F401
        import pyarrow  # noqa: F401
    except Exception as exc:  # pragma: no cover - environment dependent
        print(f"pcapng2lance_nanom interop skipped: {exc}", file=sys.stderr)
        return 77

    cases: list[tuple[str, bytes, list[bytes] | None]] = [
        ("pcapng", *build_pcapng()),
        ("pcap", *build_pcap()),
        ("multisection", build_multisection()[0], None),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        for name, data, payloads in cases:
            ext = "pcap" if name == "pcap" else "pcapng"
            fixture = tmp_path / f"{name}.{ext}"
            fixture.write_bytes(data)
            nm_ds = tmp_path / f"{name}_nm.lance"
            run(nm_exe, fixture, nm_ds)
            if payloads is not None:
                check_dataset(nm_ds, fixture, payloads)
            if nt_exe:
                nt_ds = tmp_path / f"{name}_nt.lance"
                run(nt_exe, fixture, nt_ds)
                assert_identical(nm_ds, nt_ds, name)

        # L2/L3/L4 decode: the full protocol walk lands in per-PDU Lance tables (parity with nanotins).
        decode_and_check(nm_exe, nt_exe, tmp_path)

    tail = " + byte-identical to nanotins pcapng2lance (L1 + L2/L3/L4 PDU tables)" if nt_exe else ""
    print(f"pcapng2lance_nanom interop ok (stock lance read + external offsets + --decode-l2l3{tail})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
