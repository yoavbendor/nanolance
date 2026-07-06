#!/usr/bin/env python3
"""Dataset-stitch equivalence (the core guarantee, several ways).

pcapng2lance's --drop/--count cut a capture into packet slices as free-standing datasets; because --drop
keeps the GLOBAL packet_id and payload offsets are absolute, contiguous slices are exact pieces of the full
run. nlance-stitch merges them (Option A: relocate fragments + one manifest, no re-encode). Cases:

  core      stitch [0,N)+[N,2N)+[2N,3N)  == full [0,3N)            (bit-exact, read back via nlance2table)
  order     stitch respects the <item> suffix order, not content/discovery order
  uneven    slices of unequal sizes stitch == full
  single    stitch of one slice == that slice (identity)
  decode    with --decode-l2l3, each per-PDU table (ethernet/ipv4/...) stitches == the full run's table

argv: <pcapng2lance> <nlance_stitch> <nlance2table> <fixture.pcapng>"""

import subprocess
import sys
import tempfile
from pathlib import Path

N = 60  # base slice size; 3N must be <= packets in the fixture (SRL_front_left_51_short has 224)
PDU_SUFFIXES = ["ethernet", "vlan", "ipv4", "ipv6", "tcp", "udp"]


def run(*cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"command failed ({r.returncode}): {' '.join(str(c) for c in cmd)}\n{r.stderr}", file=sys.stderr)
        raise SystemExit(1)
    return r


CONV = STITCH = N2T = None
FIXTURE = None


def slice_to(out, drop, count, decode=False):
    run(CONV, FIXTURE, out, "-d", drop, "-c", count, *(["--decode-l2l3"] if decode else []))


def stitch(parts_dir, out):
    run(STITCH, parts_dir, out, "--prefix", "results_")


def dump(dataset):
    r = run(N2T, dataset, "-f", "csv")
    return r.stdout


def pids(csv_text):
    rows = csv_text.splitlines()
    assert rows[0].split(",")[0] == "packet_id", "packet_id must be the first column"
    return [int(r.split(",", 1)[0]) for r in rows[1:]]


def equal_or_die(label, a_csv, b_csv):
    if a_csv != b_csv:
        al, bl = a_csv.splitlines(), b_csv.splitlines()
        print(f"{label}: MISMATCH ({len(al)} vs {len(bl)} rows)", file=sys.stderr)
        for i, (x, y) in enumerate(zip(al, bl)):
            if x != y:
                print(f"  first diff line {i}:\n    got: {x}\n    exp: {y}", file=sys.stderr)
                break
        raise SystemExit(1)


def case_core(t):
    parts = t / "core"; parts.mkdir()
    for k in range(3):
        slice_to(parts / f"results_{k}.lance", k * N, N)
    slice_to(t / "core_full.lance", 0, 3 * N)
    stitch(parts, t / "core_stitched.lance")
    s, f = dump(t / "core_stitched.lance"), dump(t / "core_full.lance")
    equal_or_die("core", s, f)
    assert pids(s) == list(range(3 * N)), "core: packet_id not contiguous 0..3N-1"
    print(f"  core      ok (stitch 3x{N} == full {3*N}, packet_id 0..{3*N-1})")


def case_order(t):
    # Map item suffix -> which slice's content, permuted: item0<-slice2, item1<-slice0, item2<-slice1.
    parts = t / "order"; parts.mkdir()
    content_for_item = {0: 2, 1: 0, 2: 1}  # item k holds slice `content_for_item[k]` ([s*N,(s+1)*N))
    for item, s in content_for_item.items():
        slice_to(parts / f"results_{item}.lance", s * N, N)
    stitch(parts, t / "order_stitched.lance")
    got = pids(dump(t / "order_stitched.lance"))
    expect = []  # rows must follow ITEM order (0,1,2) -> contents slice2, slice0, slice1
    for item in (0, 1, 2):
        s = content_for_item[item]
        expect += list(range(s * N, (s + 1) * N))
    assert got == expect, f"order: row order follows content not item suffix\n got={got[:5]}.. exp={expect[:5]}.."
    print("  order     ok (row order follows the <item> suffix, not content)")


def case_uneven(t):
    parts = t / "uneven"; parts.mkdir()
    sizes = [40, 80, 50]  # unequal, contiguous; sum 170 <= 224
    off = 0
    for k, c in enumerate(sizes):
        slice_to(parts / f"results_{k}.lance", off, c)
        off += c
    slice_to(t / "uneven_full.lance", 0, sum(sizes))
    stitch(parts, t / "uneven_stitched.lance")
    equal_or_die("uneven", dump(t / "uneven_stitched.lance"), dump(t / "uneven_full.lance"))
    print(f"  uneven    ok (slices {sizes} stitch == full {sum(sizes)})")


def case_single(t):
    parts = t / "single"; parts.mkdir()
    slice_to(parts / "results_0.lance", 0, N)
    slice_to(t / "single_ref.lance", 0, N)
    stitch(parts, t / "single_stitched.lance")
    equal_or_die("single", dump(t / "single_stitched.lance"), dump(t / "single_ref.lance"))
    print("  single    ok (stitch of 1 == identity)")


def case_decode(t):
    # Per-PDU tables: slices/full with --decode-l2l3, then stitch each <stem>_<pdu>.lance across slices.
    work = t / "decode"; work.mkdir()
    for k in range(3):
        slice_to(work / f"results_{k}.lance", k * N, N, decode=True)
    slice_to(work / "full.lance", 0, 3 * N, decode=True)
    checked = []
    for pdu in PDU_SUFFIXES:
        full_tbl = work / f"full_{pdu}.lance"
        if not full_tbl.exists():
            continue  # this capture has no PDUs of that type
        pdir = work / f"{pdu}_parts"; pdir.mkdir()
        for k in range(3):
            src = work / f"results_{k}_{pdu}.lance"
            if src.exists():  # a slice may legitimately have zero PDUs of this type
                src.rename(pdir / f"results_{k}.lance")
        stitch(pdir, work / f"stitched_{pdu}.lance")
        equal_or_die(f"decode/{pdu}", dump(work / f"stitched_{pdu}.lance"), dump(full_tbl))
        checked.append(pdu)
    assert checked, "decode: no PDU tables were produced to check"
    print(f"  decode    ok (--decode-l2l3 per-PDU tables stitch == full: {', '.join(checked)})")


def main(argv):
    global CONV, STITCH, N2T, FIXTURE
    if len(argv) < 5:
        print("usage: test_pcapng2lance_stitch_equiv.py <pcapng2lance> <nlance_stitch> <nlance2table> <fixture>",
              file=sys.stderr)
        return 2
    CONV, STITCH, N2T, FIXTURE = argv[1], argv[2], argv[3], Path(argv[4])
    if not FIXTURE.exists():
        print(f"fixture not found: {FIXTURE}", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as tmp:
        t = Path(tmp)
        case_core(t)
        case_order(t)
        case_uneven(t)
        case_single(t)
        case_decode(t)
    print("pcapng2lance stitch-equiv ok (core / order / uneven / single / decode-l2l3)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
