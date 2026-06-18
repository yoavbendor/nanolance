#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""IPv6 extension-header / SRv6 alignment harness: our --decode-l2l3 IPv6 tables (the spec_dag ext-header
chain walk + the tlv_cursor/repeat_at child tables, dumped via nlance2table) must agree field-for-field with
Wireshark's `tshark` dissection of the SAME capture. The fixture (srv6_sample.pcap) is built by scapy, so
the encoder (scapy), the dissector (tshark), and our parser are three independent implementations.

Validates, per packet:
  * SRv6 SRH (ipv6_routing table): routing_type, segments_left, last_entry, tag.
  * the segment list (ipv6_srh_segment table): each 16-byte address, in order, vs ipv6.routing.srh.addr.
  * Hop-by-Hop / Destination options (ipv6_option table): option count + types vs ipv6.opt.type.
  * the L4 fix: TCP/UDP is reached past the extension-header chain (vs udp.dstport / tcp.dstport).

argv[1] = nlance2table exe, argv[2] = pcapng2lance exe, argv[3] = srv6_sample.pcap. Skips (77) if tshark
is not found."""

import ipaddress
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


def packed_hex(addr: str) -> str:
    return ipaddress.IPv6Address(addr).packed.hex()


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print("usage: test_nlance2table_tshark_srv6.py <nlance2table> <pcapng2lance> <srv6_sample.pcap>",
              file=sys.stderr)
        return 2
    n2t, conv, fixture = argv[1], argv[2], Path(argv[3])
    tshark = find_tshark()
    if not tshark:
        print("nlance2table/tshark SRv6 alignment skipped: tshark not found", file=sys.stderr)
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

        routing = {row["packet_id"]: row for row in n2t_list(n2t, Path(stem + "_ipv6_routing.lance"))}
        segs = by_pid(n2t_list(n2t, Path(stem + "_ipv6_srh_segment.lance")))
        opts = by_pid(n2t_list(n2t, Path(stem + "_ipv6_option.lance")))
        udp = {row["packet_id"]: row for row in n2t_list(n2t, Path(stem + "_udp.lance"))}
        tcp = {row["packet_id"]: row for row in n2t_list(n2t, Path(stem + "_tcp.lance"))}

        fields = ["frame.number", "ipv6.routing.type", "ipv6.routing.segleft",
                  "ipv6.routing.srh.last_entry", "ipv6.routing.srh.tag", "ipv6.routing.srh.addr",
                  "ipv6.opt.type", "udp.dstport", "tcp.dstport"]
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

        srh_pkts = seg_cells = opt_cells = l4_cells = 0
        for tr in ref:
            pid = int(tr["frame.number"]) - 1

            if tr.get("ipv6.routing.type", "") != "":  # this packet has a Routing/SRH header
                srh_pkts += 1
                o = routing.get(pid)
                ck(o is not None, f"pkt{pid}: no ipv6_routing row")
                if o is not None:
                    ck(int(o["routing_type"]) == int(tr["ipv6.routing.type"]), f"pkt{pid} routing_type")
                    ck(int(o["segments_left"]) == int(tr["ipv6.routing.segleft"]), f"pkt{pid} segments_left")
                    ck(int(o["last_entry"]) == int(tr["ipv6.routing.srh.last_entry"]), f"pkt{pid} last_entry")
                    ck(int(o["tag"]) == int(tr["ipv6.routing.srh.tag"], 16),
                       f"pkt{pid} tag ours={o['tag']} tshark={tr['ipv6.routing.srh.tag']}")
                # segment list, in order
                ts_addrs = [packed_hex(a) for a in tr.get("ipv6.routing.srh.addr", "").split(",") if a]
                my_rows = sorted(segs.get(pid, []), key=lambda x: x["segment_index"])
                my_addrs = [str(x["address"]).lower() for x in my_rows]
                ck(my_addrs == ts_addrs, f"pkt{pid} segments ours={my_addrs} tshark={ts_addrs}")
                seg_cells += len(ts_addrs)

            if tr.get("ipv6.opt.type", "") != "":  # Hop-by-Hop / Dest-Opts options
                ts_types = [int(x, 16) for x in tr["ipv6.opt.type"].split(",") if x != ""]
                # tshark's ipv6.opt.type covers Hop-by-Hop (container 0) + Dest-Opts (container 60) only;
                # SRH TLVs (container 43, e.g. HMAC) live under ipv6.routing.srh.* — exclude them here.
                my_types = [int(x["opt_type"]) for x in opts.get(pid, []) if int(x["container_type"]) in (0, 60)]
                ck(my_types == ts_types, f"pkt{pid} opt types ours={my_types} tshark={ts_types}")
                opt_cells += len(ts_types)

            if tr.get("udp.dstport", "") != "":
                ck(pid in udp and int(udp[pid]["dst_port"]) == int(tr["udp.dstport"]),
                   f"pkt{pid} udp.dstport (L4 reached past ext headers)")
                l4_cells += 1
            if tr.get("tcp.dstport", "") != "":
                ck(pid in tcp and int(tcp[pid]["dst_port"]) == int(tr["tcp.dstport"]),
                   f"pkt{pid} tcp.dstport (L4 reached past ext headers)")
                l4_cells += 1

        if mism:
            print(f"SRv6/IPv6-ext FIELD MISMATCHES vs tshark ({len(mism)}):", file=sys.stderr)
            for m in mism[:20]:
                print("  " + m, file=sys.stderr)
            return 1

        # sanity: the capture actually exercised the new paths.
        if srh_pkts < 2 or seg_cells < 4 or opt_cells < 2 or l4_cells < 3:
            print(f"insufficient coverage: srh_pkts={srh_pkts} seg={seg_cells} opt={opt_cells} l4={l4_cells}",
                  file=sys.stderr)
            return 1
        print(f"nlance2table/tshark SRv6 alignment ok: {len(ref)} frames, {cells} cells checked "
              f"({srh_pkts} SRH, {seg_cells} segments, {opt_cells} options, {l4_cells} L4)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
