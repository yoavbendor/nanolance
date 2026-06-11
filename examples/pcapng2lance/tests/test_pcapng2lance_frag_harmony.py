#!/usr/bin/env python3
"""Harmonization guard: the one-shot full parse (--decode-l2l3) and the staged/augmented parse
(--stage l1->l2->l3->l4) must produce the SAME per-PDU tables — in particular the IPv4-fragmentation L4
gate must behave identically in both (TCP/UDP only on the first fragment). Uses a real *fragmented*
capture (one UDP datagram fanned across ~32 fragments), which the crafted staged/l2l3 fixtures don't
exercise. For each PDU type it dumps both tables via nlance2table and asserts equal rows (keyed by
packet_id); a missing table is treated as empty (the one-shot writer emits empty tables, the staged
enrich omits them — a structural, not row-content, difference).

argv[1] = pcapng2lance, argv[2] = nlance2table, argv[3] = fragmented pcapng. Skips 77 if a tool is absent."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

PDUS = ["ethernet", "vlan", "ipv4", "ipv6", "tcp", "udp"]


def dump_rows(n2t: str, table: Path) -> list:
    """nlance2table NDJSON -> rows sorted by packet_id; missing table == empty."""
    if not table.exists():
        return []
    out = subprocess.run([n2t, "-f", "ndjson", str(table)], capture_output=True, text=True, check=True).stdout
    rows = [json.loads(ln) for ln in out.splitlines() if ln]
    return sorted(rows, key=lambda r: r.get("packet_id", -1))


def main(argv: list[str]) -> int:
    if len(argv) < 4:
        print("usage: test_pcapng2lance_frag_harmony.py <pcapng2lance> <nlance2table> <pcapng>", file=sys.stderr)
        return 2
    exe, n2t, pcap = argv[1], argv[2], Path(argv[3])
    if not pcap.exists():
        print(f"capture not found: {pcap}", file=sys.stderr)
        return 77

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)

        # one-shot: <stem>_<pdu>.lance
        oneshot = tmp_path / "oneshot.lance"
        r = subprocess.run([exe, "--decode-l2l3", str(pcap), str(oneshot)], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"one-shot failed: {r.stderr}", file=sys.stderr)
            return 1

        # staged: l1 -> l2 -> l3 -> l4 into a data dir; tables are <dir>/<pdu>.lance
        staged = tmp_path / "staged"
        staged.mkdir()
        for stage, args in (("l1", [str(pcap), str(staged)]), ("l2", [str(staged)]),
                            ("l3", [str(staged)]), ("l4", [str(staged)])):
            rs = subprocess.run([exe, "--stage", stage, *args], capture_output=True, text=True)
            if rs.returncode != 0:
                print(f"stage {stage} failed: {rs.stderr}", file=sys.stderr)
                return 1

        mism = []
        for pdu in PDUS:
            os_rows = dump_rows(n2t, tmp_path / f"oneshot_{pdu}.lance")
            st_rows = dump_rows(n2t, staged / f"{pdu}.lance")
            if os_rows != st_rows:
                mism.append(pdu)
                print(f"  [{pdu}] one-shot={len(os_rows)} rows  staged={len(st_rows)} rows", file=sys.stderr)
                # show the first differing row for diagnosis
                for i in range(max(len(os_rows), len(st_rows))):
                    a = os_rows[i] if i < len(os_rows) else None
                    b = st_rows[i] if i < len(st_rows) else None
                    if a != b:
                        print(f"    first diff at idx {i}:\n      one-shot={a}\n      staged  ={b}", file=sys.stderr)
                        break

        if mism:
            print(f"HARMONIZATION FAILED for: {', '.join(mism)}", file=sys.stderr)
            return 1

        # Sanity: the fragmented capture must actually exercise the gate (udp present but < ipv4).
        udp = dump_rows(n2t, tmp_path / "oneshot_udp.lance")
        ipv4 = dump_rows(n2t, tmp_path / "oneshot_ipv4.lance")
        assert ipv4, "expected ipv4 rows"
        assert 0 < len(udp) < len(ipv4), f"capture should be fragmented (udp {len(udp)} << ipv4 {len(ipv4)})"

    print(f"pcapng2lance frag-harmony ok (one-shot == staged for {', '.join(PDUS)}; "
          f"udp on {len(udp)} first-fragments of {len(ipv4)} ipv4 rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
