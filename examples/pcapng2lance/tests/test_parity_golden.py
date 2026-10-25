#!/usr/bin/env python3
"""The nanom converters and DPAR/LLDP tools write the tables the nanotins reference wrote.

golden/parity_digests.json holds one SHA-256 per table (columns, schema metadata and rows, in order), computed from
the nanotins converters on the fixture captures (golden/make_parity_digests.py regenerates it). This runs the
default pcapng2lance (nanom) on the same captures, staged and one-shot, and dpar2lance / lldp2lance on the DPAR
fixture, and compares every digest.

  test_parity_golden.py <pcapng2lance> <dpar2lance> <lldp2lance> <repo-root> <nanom-testdata> <workdir>
CTest 77 = skipped (pylance missing)."""
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "golden"))


def main(argv):
    if len(argv) < 7:
        print(__doc__, file=sys.stderr)
        return 2
    pcap2lance, dpar, lldp, repo, testdata, work = (os.path.abspath(a) for a in argv[1:7])
    try:
        import lance  # noqa: F401
    except Exception as exc:  # pragma: no cover - environment dependent
        print(f"parity golden skipped: {exc}", file=sys.stderr)
        return 77
    from make_parity_digests import digest_ds, tables_of  # noqa: E402

    repo = Path(repo)
    golden = json.loads((repo / "examples/pcapng2lance/tests/golden/parity_digests.json").read_text())
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    os.chdir(work)
    t = repo / "examples/pcapng2lance/tests"
    fixtures = {
        "short": repo / "tests/test_short.pcapng", "framed": repo / "tests/test_framed.pcapng",
        "srl": t / "SRL_front_left_51_short.pcapng", "srv6": t / "srv6_sample.pcap",
        "ipv4opt": t / "ipv4_options_sample.pcap", "ptp": t / "some_ptp_data.pcapng",
    }
    prev = {"l2": "packets.lance", "l3": "remainder_after_l2.lance", "l4": "remainder_after_l3.lance"}
    got = {"stages": {}, "oneshot": {}, "dpar": {}, "lldp": {}}

    def run(args):
        r = subprocess.run([str(a) for a in args], capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"failed: {args}\n{r.stderr}")

    for name, cap in fixtures.items():
        d = f"nm_{name}"
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        run([pcap2lance, "--stage", "l1", cap, d])
        for st in ["l2", "l3", "l4"]:
            if os.path.exists(os.path.join(d, prev[st])):
                run([pcap2lance, "--stage", st, d])
        got["stages"][name] = tables_of(d)
        if name in golden["oneshot"]:
            od = f"os_{name}"
            shutil.rmtree(od, ignore_errors=True)
            os.makedirs(od)
            run([pcap2lance, "--decode-l2l3", cap, os.path.join(od, "out.lance")])
            got["oneshot"][name] = tables_of(od)
    subset = repo / "examples/pcapng2lance/tests/golden/dpar_rules_subset.txt"
    shutil.rmtree("dpar_nm", ignore_errors=True)
    run([dpar, subset, Path(testdata) / "dpar_sample.pcap", "dpar_nm"])
    got["dpar"] = {x: digest_ds(os.path.join("dpar_nm", x + ".lance")) for x in ["someip_tlv", "raw_tlv"]}
    shutil.rmtree("lldp_nm", ignore_errors=True)
    run([lldp, Path(testdata) / "lldp_rules.txt", Path(testdata) / "dpar_sample.pcap", "lldp_nm"])
    got["lldp"] = {"lldp": digest_ds(os.path.join("lldp_nm", "lldp.lance"))}

    bad = []
    for section in ("stages", "oneshot"):
        for fixture, want in golden[section].items():
            have = got[section].get(fixture, {})
            if have != want:
                bad.append(f"{section}/{fixture}: {sorted(set(want) ^ set(have)) or 'digest differs'}")
    for section in ("dpar", "lldp"):
        if got[section] != golden[section]:
            bad.append(f"{section}: {got[section]} != {golden[section]}")
    if bad:
        print("parity with the nanotins reference differs:\n  " + "\n  ".join(bad), file=sys.stderr)
        return 1
    n = sum(len(v) for s in ("stages", "oneshot") for v in golden[s].values()) + len(golden["dpar"]) + len(golden["lldp"])
    print(f"parity golden ok: {n} tables match the nanotins reference")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
