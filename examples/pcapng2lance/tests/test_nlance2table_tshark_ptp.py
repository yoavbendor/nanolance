#!/usr/bin/env python3
"""PTP/gPTP alignment harness: our --decode-l2l3 `gptp` table (the spec_dag GptpNode → PtpHeaderSpec
common header, dumped via nlance2table) must agree field-for-field with Wireshark's `tshark` PTPv2
dissection of the SAME capture. Proves the struct_spec/DAG PTP common-header parser matches an
independent, authoritative dissector on real-world gPTP traffic, across every message type present.

Validates the 10 common-header fields shared by all PTP messages (messageType, versionPTP, domainNumber,
flags, ClockIdentity, SourcePortID, sequenceId, controlField, logMessageInterval, messageLength). The
per-message-type bodies (Sync/Announce/Delay_Resp timestamps etc.) are a separate later milestone.

argv[1] = nlance2table exe, argv[2] = pcapng2lance exe, argv[3] = pcapng fixture.
Skips (77) if tshark is not found."""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def n2t_rows(n2t: str, table: Path) -> dict:
    """Dump a PDU table as NDJSON, keyed by packet_id (1 PDU/packet for the PTP common header)."""
    if not table.exists():
        return {}
    nd = subprocess.run([n2t, "-f", "ndjson", str(table)], capture_output=True, text=True, check=True).stdout
    out = {}
    for ln in nd.splitlines():
        if ln:
            o = json.loads(ln)
            out[o["packet_id"]] = o
    return out


# (our gptp column, tshark filter field, comparator). int(x, 0) parses "0x.."/decimal/negative.
CHECKS = [
    ("message_type", "ptp.v2.messagetype", lambda a, b: int(a) == int(b, 0)),
    ("version_ptp", "ptp.v2.versionptp", lambda a, b: int(a) == int(b, 0)),
    ("domain_number", "ptp.v2.domainnumber", lambda a, b: int(a) == int(b, 0)),
    ("flags", "ptp.v2.flags", lambda a, b: int(a) == int(b, 0)),
    ("clock_identity", "ptp.v2.clockidentity", lambda a, b: str(a).lower() == b.lower().removeprefix("0x")),
    ("source_port_number", "ptp.v2.sourceportid", lambda a, b: int(a) == int(b, 0)),
    ("sequence_id", "ptp.v2.sequenceid", lambda a, b: int(a) == int(b, 0)),
    ("control_field", "ptp.v2.controlfield", lambda a, b: int(a) == int(b, 0)),
    ("log_message_interval", "ptp.v2.logmessageinterval", lambda a, b: int(a) == int(b, 0)),
    ("message_length", "ptp.v2.messagelength", lambda a, b: int(a) == int(b, 0)),
]


def tshark_fields(tshark: str, pcap: Path) -> list[dict]:
    fields = ["frame.number"] + [c[1] for c in CHECKS]
    cmd = [tshark, "-r", str(pcap), "-T", "fields"] + sum([["-e", f] for f in fields], []) + \
          ["-E", "separator=|", "-E", "occurrence=f"]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    rows = []
    for ln in out.splitlines():
        if ln.strip():
            rows.append(dict(zip(fields, ln.split("|"))))
    return rows


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print("usage: test_nlance2table_tshark_ptp.py <nlance2table> <pcapng2lance> <fixture.pcapng>",
              file=sys.stderr)
        return 2
    n2t, conv, fixture = argv[1], argv[2], Path(argv[3])
    tshark = shutil.which("tshark") or (
        r"C:\Program Files\Wireshark\tshark.exe" if Path(r"C:\Program Files\Wireshark\tshark.exe").exists() else None)
    if not tshark:
        print("nlance2table/tshark PTP alignment skipped: tshark not found", file=sys.stderr)
        return 77
    if not fixture.exists():
        print(f"fixture not found: {fixture}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        out = tmp_path / "out.lance"
        r = subprocess.run([conv, "--decode-l2l3", str(fixture), str(out)], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"converter failed: {r.stderr}", file=sys.stderr)
            return 1

        gptp = n2t_rows(n2t, tmp_path / "out_gptp.lance")
        ref = tshark_fields(tshark, fixture)
        if not ref:
            print("tshark returned no PTP frames", file=sys.stderr)
            return 1
        if len(gptp) != len(ref):
            print(f"row count mismatch: gptp table {len(gptp)} vs tshark {len(ref)}", file=sys.stderr)
            return 1

        mismatches = []
        cells = 0
        msgtypes = {}
        for tr in ref:
            pid = int(tr["frame.number"]) - 1  # tshark 1-based -> our 0-based packet_id
            if pid not in gptp:
                mismatches.append(f"pkt{pid}: no gptp row")
                continue
            o = gptp[pid]
            msgtypes[int(o["message_type"])] = msgtypes.get(int(o["message_type"]), 0) + 1
            for ocol, tcol, cmp in CHECKS:
                tv = tr.get(tcol, "")
                if tv == "":
                    continue  # field absent for this frame (shouldn't happen for the common header)
                cells += 1
                if not cmp(o[ocol], tv):
                    mismatches.append(f"pkt{pid} {ocol}: ours={o[ocol]!r} tshark={tv!r}")

        if mismatches:
            print(f"PTP FIELD MISMATCHES vs tshark ({len(mismatches)}):", file=sys.stderr)
            for m in mismatches[:20]:
                print("  " + m, file=sys.stderr)
            return 1

        print(f"nlance2table/tshark PTP alignment ok: {len(ref)} frames, {cells} cells, "
              f"message types {sorted(msgtypes)} (counts {msgtypes})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
