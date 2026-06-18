#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Download-on-demand real-capture golden: fetch real IPv6/extension-header captures at test time, decode
them with pcapng2lance --decode-l2l3, and check our IPv6 tables (ipv6_routing / ipv6_srh_segment /
ipv6_option / udp / tcp) against tshark's dissection of the SAME file.

LICENSING: the captures live in GPL-2.0-or-later repositories (e.g. Wireshark). They are **fetched at
runtime and never committed/redistributed by this project** — using a GPL file as a test input is not
redistribution, so nothing is imposed on this Apache-2.0 repo. The cache dir lives under the build tree
(gitignored). Add more URLs (e.g. a public SRv6 capture you trust) to CAPTURES below.

Skips (77) if tshark is not found OR no capture could be downloaded (offline). argv[1]=nlance2table,
argv[2]=pcapng2lance, argv[3]=cache dir."""

import ipaddress
import json
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

# (name, url) of real captures to fetch at runtime (never committed). Curated for stable raw URLs only.
#
#   wireshark_ipv6  — real IPv6 + Hop-by-Hop options (Router-Alert + PadN) + ICMPv6 **MLD**. Covers the
#                     ext-header chain walk + the option child table AND the MLD case on non-synthetic data.
#
# Notably ABSENT from stable public sources (checked: Wireshark SampleCaptures wiki + the repo's
# test/captures dir):
#   * SRv6 / SRH        — no public capture anywhere; covered by the committed scapy fixture (srv6_sample.pcap).
#   * IPv6 AH / IPv6 ESP — none (the only test/captures ESP, esp-bug-12671, is IPv4); AH/ESP are covered by
#                          the owned unit test (test_ipv6_ext_walk: AH reaches L4, ESP stops cleanly).
# Add a trusted SRv6 / AH / ESP capture URL here if you obtain one (e.g. a vendor/IETF interop pcap).
CAPTURES = [
    ("wireshark_ipv6", "https://gitlab.com/wireshark/wireshark/-/raw/master/test/captures/ipv6.pcap"),
]


def find_tshark():
    t = shutil.which("tshark")
    if t:
        return t
    for c in (r"C:\Program Files\Wireshark\tshark.exe",
              str(Path.home() / r"scoop\apps\wireshark\current\tshark.exe")):
        if Path(c).exists():
            return c
    return None


def n2t_list(n2t, table: Path):
    if not table.exists():
        return []
    nd = subprocess.run([n2t, "-f", "ndjson", str(table)], capture_output=True, text=True, check=True).stdout
    return [json.loads(ln) for ln in nd.splitlines() if ln]


def by_pid(rows):
    d = {}
    for r in rows:
        d.setdefault(r["packet_id"], []).append(r)
    return d


def packed_hex(addr):
    return ipaddress.IPv6Address(addr).packed.hex()


def check_capture(name, pcap: Path, n2t, conv, tshark, mism):
    """Compare one capture; SRH/options strict, L4 lenient (tolerate chains we don't model). Returns
    (srh_pkts, segs, opts, l4) coverage counts."""
    with tempfile.TemporaryDirectory() as tmp:
        stem = str(Path(tmp) / "out")
        r = subprocess.run([conv, "--decode-l2l3", str(pcap), stem + ".lance"], capture_output=True, text=True)
        if r.returncode != 0:
            mism.append(f"{name}: converter failed: {r.stderr.strip()[:200]}")
            return (0, 0, 0, 0)
        routing = {x["packet_id"]: x for x in n2t_list(n2t, Path(stem + "_ipv6_routing.lance"))}
        segs = by_pid(n2t_list(n2t, Path(stem + "_ipv6_srh_segment.lance")))
        opts = by_pid(n2t_list(n2t, Path(stem + "_ipv6_option.lance")))
        udp = {x["packet_id"]: x for x in n2t_list(n2t, Path(stem + "_udp.lance"))}
        tcp = {x["packet_id"]: x for x in n2t_list(n2t, Path(stem + "_tcp.lance"))}

        fields = ["frame.number", "ipv6.routing.type", "ipv6.routing.segleft", "ipv6.routing.srh.last_entry",
                  "ipv6.routing.srh.tag", "ipv6.routing.srh.addr", "ipv6.opt.type", "udp.dstport", "tcp.dstport"]
        cmd = [tshark, "-r", str(pcap), "-T", "fields"] + sum([["-e", f] for f in fields], []) + \
              ["-E", "separator=|", "-E", "occurrence=a", "-E", "aggregator=,"]
        ref = [dict(zip(fields, ln.split("|")))
               for ln in subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.splitlines()
               if ln.strip()]

        sp = sc = oc = lc = 0
        for tr in ref:
            pid = int(tr["frame.number"]) - 1
            if tr.get("ipv6.routing.type", "") != "":
                sp += 1
                o = routing.get(pid)
                if o is None:
                    mism.append(f"{name} pkt{pid}: no ipv6_routing row")
                else:
                    if int(o["routing_type"]) != int(tr["ipv6.routing.type"]):
                        mism.append(f"{name} pkt{pid} routing_type")
                    if tr["ipv6.routing.segleft"] and int(o["segments_left"]) != int(tr["ipv6.routing.segleft"]):
                        mism.append(f"{name} pkt{pid} segments_left")
                ts_addrs = [packed_hex(a) for a in tr.get("ipv6.routing.srh.addr", "").split(",") if a]
                my = [str(x["address"]).lower() for x in sorted(segs.get(pid, []), key=lambda z: z["segment_index"])]
                if my != ts_addrs:
                    mism.append(f"{name} pkt{pid} segments ours={my} tshark={ts_addrs}")
                sc += len(ts_addrs)
            if tr.get("ipv6.opt.type", "") != "":
                ts_types = [int(x, 16) for x in tr["ipv6.opt.type"].split(",") if x != ""]
                my_types = [int(x["opt_type"]) for x in opts.get(pid, [])]
                if my_types != ts_types:
                    mism.append(f"{name} pkt{pid} opt types ours={my_types} tshark={ts_types}")
                oc += len(ts_types)
            # L4 lenient: only check when we produced a row (a chain we model); never fail on unmodeled chains.
            if tr.get("udp.dstport", "") != "" and pid in udp:
                lc += 1
                if int(udp[pid]["dst_port"]) != int(tr["udp.dstport"]):
                    mism.append(f"{name} pkt{pid} udp.dstport")
            if tr.get("tcp.dstport", "") != "" and pid in tcp:
                lc += 1
                if int(tcp[pid]["dst_port"]) != int(tr["tcp.dstport"]):
                    mism.append(f"{name} pkt{pid} tcp.dstport")
        return (sp, sc, oc, lc)


def main(argv):
    if len(argv) < 4:
        print("usage: test_nlance2table_tshark_download.py <nlance2table> <pcapng2lance> <cache_dir>",
              file=sys.stderr)
        return 2
    n2t, conv, cache = argv[1], argv[2], Path(argv[3])
    tshark = find_tshark()
    if not tshark:
        print("download golden skipped: tshark not found", file=sys.stderr)
        return 77
    cache.mkdir(parents=True, exist_ok=True)

    downloaded = []
    for name, url in CAPTURES:
        dst = cache / (name + Path(url).suffix)
        if not dst.exists():
            try:
                with urllib.request.urlopen(url, timeout=20) as resp:
                    dst.write_bytes(resp.read())
            except Exception as e:  # offline / URL moved -> skip this one
                print(f"  download failed ({name}): {e}", file=sys.stderr)
                continue
        downloaded.append((name, dst))

    if not downloaded:
        print("download golden skipped: no captures could be fetched (offline?)", file=sys.stderr)
        return 77

    mism, total = [], (0, 0, 0, 0)
    for name, pcap in downloaded:
        sp, sc, oc, lc = check_capture(name, pcap, n2t, conv, tshark, mism)
        total = tuple(a + b for a, b in zip(total, (sp, sc, oc, lc)))

    if mism:
        print(f"REAL-CAPTURE FIELD MISMATCHES vs tshark ({len(mism)}):", file=sys.stderr)
        for m in mism[:20]:
            print("  " + m, file=sys.stderr)
        return 1
    print(f"download golden ok: {len(downloaded)} capture(s); "
          f"SRH={total[0]} segments={total[1]} options={total[2]} L4={total[3]} checked vs tshark")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
