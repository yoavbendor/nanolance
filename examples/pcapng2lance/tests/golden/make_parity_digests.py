#!/usr/bin/env python3
"""Regenerate parity_digests.json from the nanotins converters (the reference the nanom converters must match).

  make_parity_digests.py <pcapng2lance> <dpar2lance_nanotins> <lldp2lance_nanotins> <repo-root> <nanom-testdata> <workdir>

Needs pylance. Each digest is the SHA-256 of a table's columns, schema metadata and rows (in order)."""
import hashlib, json, os, shutil, subprocess, sys

import lance


def digest_ds(path):
    ds = lance.dataset(path)
    tab = ds.to_table()
    rows = [{k: (v.hex() if isinstance(v, bytes) else v) for k, v in r.items()} for r in tab.to_pylist()]
    meta = {k.decode(): v.decode() for k, v in (ds.schema.metadata or {}).items()}
    canon = json.dumps({"columns": tab.column_names, "metadata": meta, "rows": rows}, sort_keys=True)
    return hashlib.sha256(canon.encode()).hexdigest()


def tables_of(d):
    return {f[:-6]: digest_ds(os.path.join(d, f)) for f in sorted(os.listdir(d)) if f.endswith(".lance")}


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"failed: {args}\n{r.stderr}")


def main(argv):
    pcap2lance, dpar, lldp, repo, testdata, work = argv[1:7]
    os.makedirs(work, exist_ok=True)
    os.chdir(work)
    t = f"{repo}/examples/pcapng2lance/tests"
    fixtures = {
        "short": f"{repo}/tests/test_short.pcapng", "framed": f"{repo}/tests/test_framed.pcapng",
        "srl": f"{t}/SRL_front_left_51_short.pcapng", "srv6": f"{t}/srv6_sample.pcap",
        "ipv4opt": f"{t}/ipv4_options_sample.pcap", "ptp": f"{t}/some_ptp_data.pcapng",
    }
    prev = {"l2": "packets.lance", "l3": "remainder_after_l2.lance", "l4": "remainder_after_l3.lance"}
    out = {"stages": {}, "oneshot": {}, "dpar": {}, "lldp": {}}
    for name, cap in fixtures.items():
        d = f"nt_{name}"
        shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
        run([pcap2lance, "--backend", "nanotins", "--stage", "l1", cap, d])
        for st in ["l2", "l3", "l4"]:
            if os.path.exists(os.path.join(d, prev[st])):
                run([pcap2lance, "--backend", "nanotins", "--stage", st, d])
        out["stages"][name] = tables_of(d)
        if name in ("srl", "srv6", "ipv4opt", "framed"):
            od = f"os_{name}"; shutil.rmtree(od, ignore_errors=True); os.makedirs(od)
            run([pcap2lance, "--backend", "nanotins", "--decode-l2l3", cap, os.path.join(od, "out.lance")])
            out["oneshot"][name] = tables_of(od)
    subset = f"{repo}/examples/pcapng2lance/tests/golden/dpar_rules_subset.txt"
    dd = "dpar_nt"; shutil.rmtree(dd, ignore_errors=True)
    run([dpar, subset, f"{testdata}/dpar_sample.pcap", dd])
    out["dpar"] = {x: digest_ds(os.path.join(dd, x + ".lance")) for x in ["someip_tlv", "raw_tlv"]}
    ld = "lldp_nt"; shutil.rmtree(ld, ignore_errors=True)
    run([lldp, f"{testdata}/lldp_rules.txt", f"{testdata}/dpar_sample.pcap", ld])
    out["lldp"] = {"lldp": digest_ds(os.path.join(ld, "lldp.lance"))}
    with open(os.path.join(repo, "examples/pcapng2lance/tests/golden/parity_digests.json"), "w") as f:
        json.dump(out, f, indent=1, sort_keys=True)
        f.write("\n")
    print("wrote parity_digests.json")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
