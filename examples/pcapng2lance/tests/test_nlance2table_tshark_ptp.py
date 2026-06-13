#!/usr/bin/env python3
"""PTP/gPTP alignment harness: our --decode-l2l3 PTP tables (the spec_dag GptpNode common header +
the per-message-type body nodes, dumped via nlance2table) must agree field-for-field with Wireshark's
`tshark` PTPv2 dissection of the SAME capture. Proves the wire_spec/DAG PTP parser — including the
GptpNode message_type sub-dispatch into the body specs — matches an independent dissector on real gPTP.

Validates:
  * common header (gptp table): messageType, versionPTP, domainNumber, flags, ClockIdentity, SourcePortID,
    sequenceId, controlField, logMessageInterval, messageLength.
  * per-message-type body (ptp_timestamp / ptp_ts_port / ptp_announce / ptp_signaling), routed by the DAG:
    the origin/receive/precise timestamp (48-bit seconds + nanoseconds), the requesting PortIdentity
    (Delay_Resp / Pdelay_Resp / Pdelay_Resp_Follow_Up), the Announce grandmaster fields, and the
    Signaling targetPortIdentity.

argv[1] = nlance2table exe, argv[2] = pcapng2lance exe, argv[3] = pcapng fixture.
Skips (77) if tshark is not found."""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def n2t_rows(n2t: str, table: Path) -> dict:
    if not table.exists():
        return {}
    nd = subprocess.run([n2t, "-f", "ndjson", str(table)], capture_output=True, text=True, check=True).stdout
    return {o["packet_id"]: o for o in (json.loads(ln) for ln in nd.splitlines() if ln)}


def iv(x):  # tshark value -> int (hex/decimal/negative); empty -> 0
    return int(x, 0) if x not in (None, "") else 0


def hx(x):  # tshark hex bytes ("0x..") -> lowercase hex without prefix; empty -> ""
    return x.lower().removeprefix("0x") if x else ""


# ---- common header checks (our gptp column, tshark field, comparator) ----
HDR = [
    ("message_type", "ptp.v2.messagetype", lambda a, b: int(a) == iv(b)),
    ("version_ptp", "ptp.v2.versionptp", lambda a, b: int(a) == iv(b)),
    ("domain_number", "ptp.v2.domainnumber", lambda a, b: int(a) == iv(b)),
    ("flags", "ptp.v2.flags", lambda a, b: int(a) == iv(b)),
    ("clock_identity", "ptp.v2.clockidentity", lambda a, b: str(a).lower() == hx(b)),
    ("source_port_number", "ptp.v2.sourceportid", lambda a, b: int(a) == iv(b)),
    ("sequence_id", "ptp.v2.sequenceid", lambda a, b: int(a) == iv(b)),
    ("control_field", "ptp.v2.controlfield", lambda a, b: int(a) == iv(b)),
    ("log_message_interval", "ptp.v2.logmessageinterval", lambda a, b: int(a) == iv(b)),
    ("message_length", "ptp.v2.messagelength", lambda a, b: int(a) == iv(b)),
]

# ---- body routing (by messageType): which body table + tshark field prefixes ----
# table per messageType
TABLE = {0: "timestamp", 1: "timestamp", 2: "timestamp", 8: "timestamp",
         9: "ts_port", 3: "ts_port", 10: "ts_port", 11: "announce", 12: "signaling"}
# the timestamp tshark prefix per messageType (append .seconds / .nanoseconds)
TS_FIELD = {0: "ptp.v2.sdr.origintimestamp", 1: "ptp.v2.sdr.origintimestamp",
            2: "ptp.v2.pdrq.origintimestamp", 8: "ptp.v2.fu.preciseorigintimestamp",
            9: "ptp.v2.dr.receivetimestamp", 3: "ptp.v2.pdrs.requestreceipttimestamp",
            10: "ptp.v2.pdfu.responseorigintimestamp", 11: "ptp.v2.an.origintimestamp"}
# requesting-PortIdentity tshark fields per ts_port messageType: (clockidentity field, portnumber field)
REQ_PORT = {9: ("ptp.v2.dr.requestingsourceportidentity", "ptp.v2.dr.requestingsourceportid"),
            3: ("ptp.v2.pdrs.requestingportidentity", "ptp.v2.pdrs.requestingsourceportid"),
            10: ("ptp.v2.pdfu.requestingportidentity", "ptp.v2.pdfu.requestingsourceportid")}

# all tshark body fields to request (one tshark run pulls everything; empty where not applicable)
BODY_FIELDS = []
for p in set(TS_FIELD.values()):
    BODY_FIELDS += [p + ".seconds", p + ".nanoseconds"]
for idf, pnf in REQ_PORT.values():
    BODY_FIELDS += [idf, pnf]
BODY_FIELDS += ["ptp.v2.sig.targetportidentity", "ptp.v2.sig.targetportid",
                "ptp.v2.an.origincurrentutcoffset", "ptp.v2.an.localstepsremoved",
                "ptp.v2.an.grandmasterclockidentity", "ptp.v2.an.grandmasterclockclass",
                "ptp.v2.an.grandmasterclockaccuracy", "ptp.v2.an.grandmasterclockvariance",
                "ptp.v2.timesource"]


def tshark_rows(tshark: str, pcap: Path) -> list[dict]:
    fields = ["frame.number"] + [c[1] for c in HDR] + BODY_FIELDS
    cmd = [tshark, "-r", str(pcap), "-T", "fields"] + sum([["-e", f] for f in fields], []) + \
          ["-E", "separator=|", "-E", "occurrence=f"]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    return [dict(zip(fields, ln.split("|"))) for ln in out.splitlines() if ln.strip()]


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
        t = Path(tmp)
        out = t / "out.lance"
        r = subprocess.run([conv, "--decode-l2l3", str(fixture), str(out)], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"converter failed: {r.stderr}", file=sys.stderr)
            return 1

        gptp = n2t_rows(n2t, t / "out_gptp.lance")
        body = {
            "timestamp": n2t_rows(n2t, t / "out_ptp_timestamp.lance"),
            "ts_port": n2t_rows(n2t, t / "out_ptp_ts_port.lance"),
            "announce": n2t_rows(n2t, t / "out_ptp_announce.lance"),
            "signaling": n2t_rows(n2t, t / "out_ptp_signaling.lance"),
        }
        ref = tshark_rows(tshark, fixture)
        if not ref or len(gptp) != len(ref):
            print(f"row count: gptp {len(gptp)} vs tshark {len(ref)}", file=sys.stderr)
            return 1

        mism, cells, msgtypes = [], 0, {}

        def ck(cond, label):
            nonlocal cells
            cells += 1
            if not cond:
                mism.append(label)

        for tr in ref:
            pid = int(tr["frame.number"]) - 1
            if pid not in gptp:
                mism.append(f"pkt{pid}: no gptp row")
                continue
            o = gptp[pid]
            mt = int(o["message_type"])
            msgtypes[mt] = msgtypes.get(mt, 0) + 1

            # common header
            for ocol, tcol, cmp in HDR:
                if tr.get(tcol, "") != "":
                    ck(cmp(o[ocol], tr[tcol]), f"pkt{pid} hdr.{ocol}: ours={o[ocol]!r} tshark={tr[tcol]!r}")

            # body, routed by message_type
            tbl = TABLE.get(mt)
            if tbl is None:
                continue
            row = body[tbl].get(pid)
            if row is None:
                mism.append(f"pkt{pid}: no {tbl} body row (msgtype {mt})")
                continue

            if tbl in ("timestamp", "ts_port", "announce"):
                pfx = "origin_" if tbl == "announce" else "ts_"
                sec = (int(row[pfx + "seconds_msb"]) << 32) | int(row[pfx + "seconds_lsb"])
                tsf = TS_FIELD[mt]
                ck(sec == iv(tr.get(tsf + ".seconds")), f"pkt{pid} {tbl}.seconds ours={sec} tshark={tr.get(tsf+'.seconds')!r}")
                ck(int(row[pfx + "nanoseconds"]) == iv(tr.get(tsf + ".nanoseconds")),
                   f"pkt{pid} {tbl}.nanoseconds")
            if tbl == "ts_port":
                idf, pnf = REQ_PORT[mt]
                ck(str(row["req_clock_identity"]).lower() == hx(tr.get(idf)),
                   f"pkt{pid} req_clock_identity ours={row['req_clock_identity']!r} tshark={tr.get(idf)!r}")
                ck(int(row["req_port_number"]) == iv(tr.get(pnf)), f"pkt{pid} req_port_number")
            if tbl == "signaling":
                ck(str(row["target_clock_identity"]).lower() == hx(tr.get("ptp.v2.sig.targetportidentity")),
                   f"pkt{pid} target_clock_identity")
                ck(int(row["target_port_number"]) == iv(tr.get("ptp.v2.sig.targetportid")),
                   f"pkt{pid} target_port_number")
            if tbl == "announce":
                ck(int(row["current_utc_offset"]) == iv(tr.get("ptp.v2.an.origincurrentutcoffset")),
                   f"pkt{pid} current_utc_offset")
                ck(int(row["steps_removed"]) == iv(tr.get("ptp.v2.an.localstepsremoved")), f"pkt{pid} steps_removed")
                ck(int(row["time_source"]) == iv(tr.get("ptp.v2.timesource")), f"pkt{pid} time_source")
                ck(str(row["grandmaster_identity"]).lower() == hx(tr.get("ptp.v2.an.grandmasterclockidentity")),
                   f"pkt{pid} grandmaster_identity")
                ck(int(row["grandmaster_clock_class"]) == iv(tr.get("ptp.v2.an.grandmasterclockclass")),
                   f"pkt{pid} grandmaster_clock_class")
                ck(int(row["grandmaster_offset_scaled_log_variance"]) == iv(tr.get("ptp.v2.an.grandmasterclockvariance")),
                   f"pkt{pid} grandmaster_variance")

        if mism:
            print(f"PTP FIELD MISMATCHES vs tshark ({len(mism)}):", file=sys.stderr)
            for m in mism[:20]:
                print("  " + m, file=sys.stderr)
            return 1

        body_rows = {k: len(v) for k, v in body.items() if v}
        print(f"nlance2table/tshark PTP alignment ok: {len(ref)} frames, {cells} cells checked, "
              f"message types {sorted(msgtypes)}, body tables {body_rows}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
