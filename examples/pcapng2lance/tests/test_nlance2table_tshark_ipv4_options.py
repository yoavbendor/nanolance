#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""IPv4-options alignment harness: our --decode-l2l3 ipv4_option child table (the tlv_cursor ipv4_options
walk over [IPv4 base + 20, IHL*4), dumped via nlance2table) must agree with Wireshark's `tshark` dissection
of the SAME capture. The fixture (ipv4_options_sample.pcap) is built by scapy, so the encoder (scapy), the
dissector (tshark), and our parser are three independent implementations.

Validates, per packet:
  * the option type sequence (ipv4_option table) vs tshark's ip.opt.type, in order — including the
    single-byte NOP (1) and End-of-Option-List (0) markers.
  * a header with no options yields no option rows.
  * the L4 fix: TCP/UDP is reached past the option area (vs udp.dstport / tcp.dstport).

argv[1] = nlance2table exe, argv[2] = pcapng2lance exe, argv[3] = ipv4_options_sample.pcap. Skips (77) if
tshark is not found."""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def find_tshark():
    t = shutil.which("tshark")
    if t:
        return t
    for c in (r"C:\Program Files\Wireshark\tshark.exe",
              str(Path.home() / r"scoop\apps\wireshark\current\tshark.exe")):
        if Path(c).exists():
            return c
    return None


def n2t_list(n2t: str, table: Path) -> list:
    if not table.exists():
        return []
    nd = subprocess.run([n2t, "-f", "ndjson", str(table)], capture_output=True, text=True, check=True).stdout
    return [json.loads(ln) for ln in nd.splitlines() if ln]


def by_pid(rows: list) -> dict:  # group multi-row table by packet_id, preserving order
    d = {}
    for r in rows:
        d.setdefault(r["packet_id"], []).append(r)
    return d


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print("usage: test_nlance2table_tshark_ipv4_options.py <nlance2table> <pcapng2lance> "
              "<ipv4_options_sample.pcap>", file=sys.stderr)
        return 2
    n2t, conv, fixture = argv[1], argv[2], Path(argv[3])
    tshark = find_tshark()
    if not tshark:
        print("nlance2table/tshark IPv4-options alignment skipped: tshark not found", file=sys.stderr)
        return 77
    if not fixture.exists():
        print(f"fixture not found: {fixture}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        out = t / "out.lance"
        r = subprocess.run([conv, "--decode-l2l3", str(fixture), str(out)], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"converter failed: {r.stderr}", file=sys.stderr)
            return 1
        stem = str(t / "out")

        opts = by_pid(n2t_list(n2t, Path(stem + "_ipv4_option.lance")))
        udp = {row["packet_id"]: row for row in n2t_list(n2t, Path(stem + "_udp.lance"))}
        tcp = {row["packet_id"]: row for row in n2t_list(n2t, Path(stem + "_tcp.lance"))}

        fields = ["frame.number", "ip.opt.type", "udp.dstport", "tcp.dstport"]
        cmd = [tshark, "-r", str(fixture), "-T", "fields"] + sum([["-e", f] for f in fields], []) + \
              ["-E", "separator=|", "-E", "occurrence=a", "-E", "aggregator=,"]
        out_ts = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
        ref = [dict(zip(fields, ln.split("|"))) for ln in out_ts.splitlines() if ln.strip()]
        if not ref:
            print("tshark produced no rows", file=sys.stderr)
            return 1

        mism, cells = [], 0

        def ck(cond, label):
            nonlocal cells
            cells += 1
            if not cond:
                mism.append(label)

        opt_pkts = opt_cells = l4_cells = 0
        for tr in ref:
            pid = int(tr["frame.number"]) - 1

            ts_types = [int(x) for x in tr.get("ip.opt.type", "").split(",") if x != ""]
            my_types = [int(x["opt_type"]) for x in opts.get(pid, [])]
            if ts_types or my_types:
                opt_pkts += 1
                ck(my_types == ts_types, f"pkt{pid} opt types ours={my_types} tshark={ts_types}")
                opt_cells += len(ts_types)

            if tr.get("udp.dstport", "") != "":
                ck(pid in udp and int(udp[pid]["dst_port"]) == int(tr["udp.dstport"]),
                   f"pkt{pid} udp.dstport (L4 reached past options)")
                l4_cells += 1
            if tr.get("tcp.dstport", "") != "":
                ck(pid in tcp and int(tcp[pid]["dst_port"]) == int(tr["tcp.dstport"]),
                   f"pkt{pid} tcp.dstport (L4 reached past options)")
                l4_cells += 1

        if mism:
            print(f"IPv4-options FIELD MISMATCHES vs tshark ({len(mism)}):", file=sys.stderr)
            for m in mism[:20]:
                print("  " + m, file=sys.stderr)
            return 1

        # sanity: the capture actually exercised the option paths.
        if opt_pkts < 3 or opt_cells < 5 or l4_cells < 3:
            print(f"insufficient coverage: opt_pkts={opt_pkts} opt={opt_cells} l4={l4_cells}", file=sys.stderr)
            return 1
        print(f"nlance2table/tshark IPv4-options alignment ok: {len(ref)} frames, {cells} cells checked "
              f"({opt_pkts} packets with options, {opt_cells} option cells, {l4_cells} L4)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
