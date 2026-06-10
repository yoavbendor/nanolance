#!/usr/bin/env python3
"""Real-world alignment: run --decode-l2l3 on a committed capture, dump the PDU tables via nlance2table,
and cross-check field-for-field against `tshark` on the SAME file. This capture is heavily IP-fragmented
(one UDP datagram spread over many fragments), so it is the regression guard for the fragmentation fix:
the L4 header physically lives only in the first fragment (frag_offset==0), and our udp table must contain
exactly those rows — matching tshark dissected with reassembly OFF (`-o ip.defragment:FALSE`, which
attributes the UDP header to the fragment that physically carries it, i.e. offset 0).

argv[1] = nlance2table, argv[2] = pcapng2lance, argv[3] = pcapng path. Skips (77) if tshark is absent."""

import json
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path


def ip4_hex(dotted: str) -> str:
    return socket.inet_aton(dotted).hex()


def mac_hex(colon: str) -> str:
    return colon.replace(":", "").lower()


def n2t_rows(n2t: str, table: Path) -> dict:
    if not table.exists():
        return {}
    nd = subprocess.run([n2t, "-f", "ndjson", str(table)], capture_output=True, text=True, check=True).stdout
    out = {}
    for ln in nd.splitlines():
        if ln:
            o = json.loads(ln)
            out[o["packet_id"]] = o
    return out


def tshark_rows(tshark: str, pcap: Path) -> list[dict]:
    fields = ["frame.number", "eth.dst", "eth.src", "eth.type", "vlan.id", "ip.src", "ip.dst", "ip.proto",
              "ip.frag_offset", "udp.srcport", "udp.dstport"]
    # reassembly OFF so the UDP header is dissected on the first fragment (where its bytes physically are).
    cmd = ([tshark, "-r", str(pcap), "-o", "ip.defragment:FALSE", "-T", "fields"] +
           sum([["-e", f] for f in fields], []) + ["-E", "separator=|", "-E", "occurrence=f"])
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    rows = []
    for ln in out.splitlines():
        if ln.strip():
            rows.append(dict(zip(fields, ln.split("|"))))
    return rows


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print("usage: test_nlance2table_tshark_realfile.py <nlance2table> <pcapng2lance> <pcapng>", file=sys.stderr)
        return 2
    n2t, conv, pcap = argv[1], argv[2], Path(argv[3])
    tshark = shutil.which("tshark") or (
        r"C:\Program Files\Wireshark\tshark.exe" if Path(r"C:\Program Files\Wireshark\tshark.exe").exists() else None)
    if not tshark:
        print("nlance2table/tshark realfile alignment skipped: tshark not found", file=sys.stderr)
        return 77
    if not pcap.exists():
        print(f"capture not found: {pcap}", file=sys.stderr)
        return 77

    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "out.lance"
        r = subprocess.run([conv, "--decode-l2l3", str(pcap), str(out)], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"converter failed: {r.stderr}", file=sys.stderr)
            return 1

        eth_t = n2t_rows(n2t, Path(tmp) / "out_ethernet.lance")
        ip4_t = n2t_rows(n2t, Path(tmp) / "out_ipv4.lance")
        vlan_t = n2t_rows(n2t, Path(tmp) / "out_vlan.lance")
        udp_t = n2t_rows(n2t, Path(tmp) / "out_udp.lance")

        ref = tshark_rows(tshark, pcap)
        assert ref, "tshark returned no frames"

        mism = []

        def check(cond, label):
            if not cond:
                mism.append(label)

        tshark_first_frag_udp = 0
        for tr in ref:
            pid = int(tr["frame.number"]) - 1

            # L2 + VLAN + L3 present on every frame.
            assert pid in eth_t, f"no ethernet row for packet {pid}"
            e = eth_t[pid]
            check(e["dst"] == mac_hex(tr["eth.dst"]), f"pkt{pid} eth.dst")
            check(e["src"] == mac_hex(tr["eth.src"]), f"pkt{pid} eth.src")
            check(e["ethertype"] == int(tr["eth.type"], 16), f"pkt{pid} eth.type")
            if tr.get("vlan.id"):
                check(pid in vlan_t and vlan_t[pid]["vid"] == int(tr["vlan.id"]), f"pkt{pid} vlan.id")
            assert pid in ip4_t, f"no ipv4 row for packet {pid}"
            v = ip4_t[pid]
            check(v["src"] == ip4_hex(tr["ip.src"]), f"pkt{pid} ip.src")
            check(v["dst"] == ip4_hex(tr["ip.dst"]), f"pkt{pid} ip.dst")
            check(v["protocol"] == int(tr["ip.proto"]), f"pkt{pid} ip.proto")
            check(v["frag_offset"] == int(tr["ip.frag_offset"]), f"pkt{pid} ip.frag_offset")

            # L4 must appear in OUR table iff tshark (reassembly off) dissected a UDP header here
            # == the first/only fragment. This is the fragmentation-gating regression assertion.
            has_udp = bool(tr.get("udp.dstport"))
            if has_udp:
                tshark_first_frag_udp += 1
                assert pid in udp_t, f"pkt{pid}: tshark sees udp but our udp table has no row"
                check(udp_t[pid]["dst_port"] == int(tr["udp.dstport"]), f"pkt{pid} udp.dstport")
                check(udp_t[pid]["src_port"] == int(tr["udp.srcport"]), f"pkt{pid} udp.srcport")
            else:
                check(pid not in udp_t, f"pkt{pid}: fragment (offset>0) wrongly has a udp row")

        # Headline counts: our udp rows == tshark first-fragment udp frames; ipv4 covers every frame.
        check(len(udp_t) == tshark_first_frag_udp,
              f"udp row count {len(udp_t)} != tshark first-fragment udp {tshark_first_frag_udp}")
        check(len(ip4_t) == len(ref), f"ipv4 rows {len(ip4_t)} != frames {len(ref)}")

        if mism:
            print("REALFILE MISMATCHES vs tshark:", file=sys.stderr)
            for m in mism[:40]:
                print("  " + m, file=sys.stderr)
            return 1

        print(f"nlance2table/tshark realfile ok ({len(ref)} frames; udp on {tshark_first_frag_udp} "
              f"first-fragments only; eth/vlan/ipv4 fields match Wireshark)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
