#!/usr/bin/env python3
"""Checks gen_logs.py and its truth file.

Runs gen_logs.py twice with the same arguments and asserts both the NDJSON and the
truth are byte-identical. Then, against the NDJSON:
  - blank lines and their numbers match the truth; per_line covers every non-blank
    line in order;
  - fingerprint and kind are recomputed from each line with json;
  - the checkable case tags are recomputed from the line and must match exactly
    (non_object, malformed, unicode, special_key, optional_key, empty_array,
    always_null_key, int_widened_to_float, int_locked_then_float,
    type_drift_after_lock, long_line);
  - expect_by_keys / expect_by_event and lock_points match simulate() over the
    lines, and rejects are exactly the lines carrying the reject cases;
  - each case meets its minimum count; realistic object lines are 200-2000 bytes.

Usage:
  check_gen.py --profile small --seed 1
  check_gen.py --profile realistic --lines 50000 --seed 2 [--kinds K] [--workdir DIR]
"""

import argparse
import collections
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_logs  # noqa: E402

MIN_COUNTS = {
    "key_order_shuffle": 100,
    "optional_key": 50,
    "empty_array": 100,
    "array_filled": 100,
    "always_null_key": 100,
    "int_widened_to_float": 2,
    "int_locked_then_float": 5,
    "type_drift_after_lock": 5,
    "non_object": 1,
    "malformed": 1,
    "unicode": 100,
    "special_key": 100,
    "long_line": 1,
}
RECOMPUTED = {
    "non_object", "malformed", "unicode", "special_key", "optional_key", "empty_array",
    "always_null_key", "int_widened_to_float", "int_locked_then_float",
    "type_drift_after_lock", "long_line",
}


def fail(msg):
    raise SystemExit(f"FAIL: {msg}")


def check(cond, msg):
    if not cond:
        fail(msg)


def recompute(raw_line, value, parse_ok, is_obj, long_bytes):
    if not parse_ok:
        return {"malformed"}
    if not is_obj:
        return {"non_object"}
    text = raw_line.decode("utf-8")
    cases = set()
    if any(ord(c) > 127 for c in text):
        cases.add("unicode")
    if any("." in k or " " in k for k in value):
        cases.add("special_key")
    if "mfa_method" in value:
        cases.add("optional_key")
    if value.get("tags") == []:
        cases.add("empty_array")
    if "replica_lag" in value and value["replica_lag"] is None:
        cases.add("always_null_key")
    if isinstance(value.get("duration_ms"), float):
        cases.add("int_widened_to_float")
    if isinstance(value.get("value"), float):
        cases.add("int_locked_then_float")
    if isinstance(value.get("sample_count"), str):
        cases.add("type_drift_after_lock")
    if len(raw_line) >= long_bytes:
        cases.add("long_line")
    return cases


def validate(raw, truth_bytes, profile):
    truth = json.loads(truth_bytes)
    check(raw.endswith(b"\n"), "output must end with a newline")
    phys = raw[:-1].split(b"\n")
    blank = [n for n, line in enumerate(phys, 1) if line.strip() == b""]
    nonblank = [(n, line) for n, line in enumerate(phys, 1) if line.strip() != b""]

    check(truth["blank_lines"] == len(blank) and truth["blank_line_numbers"] == blank,
          "blank line count or numbers differ from the output")
    check(truth["lines"] == len(nonblank) == len(truth["per_line"]),
          "truth line count differs from non-blank lines in the output")

    counts = collections.Counter()
    heaps_k, heaps_e = {}, {}
    rejects_seen = 0
    for entry, (n, raw_line) in zip(truth["per_line"], nonblank):
        check(entry["line"] == n, f"per_line entry for line {n} has line {entry['line']}")
        try:
            value, parse_ok = json.loads(raw_line.decode("utf-8")), True
        except ValueError:
            value, parse_ok = None, False
        is_obj = parse_ok and isinstance(value, dict)

        if is_obj:
            fp = "\x1f".join(sorted(value))
            kind = value.get("event")
            check(entry["fingerprint"] == fp, f"line {n}: fingerprint mismatch")
            check(entry["kind"] == kind, f"line {n}: kind mismatch")
            heaps_k.setdefault(fp, []).append((n, value))
            heaps_e.setdefault(kind, []).append((n, value))
        else:
            check(entry["fingerprint"] is None and entry["kind"] is None,
                  f"line {n}: non-object line must have null fingerprint and kind")

        cases = set(entry["cases"])
        got = recompute(raw_line, value, parse_ok, is_obj, truth["long_line_bytes"])
        check((cases & RECOMPUTED) == got,
              f"line {n}: cases {sorted(cases & RECOMPUTED)} != recomputed {sorted(got)}")

        reject_planned = bool(cases & gen_logs.REJECT_CASES)
        if reject_planned:
            rejects_seen += 1
        if not is_obj:
            check(entry["expect_by_keys"] == "reject" and entry["expect_by_event"] == "reject",
                  f"line {n}: non-object line must reject in both modes")

        if profile == "realistic" and is_obj and "long_line" not in cases:
            size = len(raw_line)
            check(200 <= size <= 2000, f"line {n}: realistic object line is {size} bytes")
        counts.update(cases)

    exp_k, locks_k = gen_logs.simulate(heaps_k)
    exp_e, locks_e = gen_logs.simulate(heaps_e)
    check(truth["lock_points"]["by_keys"] == locks_k, "by_keys lock points differ from simulate()")
    check(truth["lock_points"]["by_event"] == locks_e, "by_event lock points differ from simulate()")
    for entry in truth["per_line"]:
        n = entry["line"]
        if entry["fingerprint"] is None:
            continue
        check(entry["expect_by_keys"] == exp_k[n], f"line {n}: expect_by_keys differs from simulate()")
        check(entry["expect_by_event"] == exp_e[n], f"line {n}: expect_by_event differs from simulate()")
        planned = bool(set(entry["cases"]) & gen_logs.REJECT_CASES)
        check((entry["expect_by_keys"] == "reject") == planned,
              f"line {n}: expect_by_keys inconsistent with reject cases")
        check((entry["expect_by_event"] == "reject") == planned,
              f"line {n}: expect_by_event inconsistent with reject cases")

    for case, minimum in MIN_COUNTS.items():
        check(counts[case] >= minimum, f"case {case}: {counts[case]} < {minimum}")
    return counts, len(blank), len(nonblank), rejects_seen


def run_gen(gen_args, out, truth):
    cmd = [sys.executable, os.path.join(HERE, "gen_logs.py"), *gen_args,
           "--out", out, "--truth", truth]
    subprocess.run(cmd, check=True)


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--profile", choices=sorted(gen_logs.PROFILES), default="small")
    ap.add_argument("--lines", type=int)
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--kinds", type=int)
    ap.add_argument("--workdir")
    args = ap.parse_args(argv)

    workdir = args.workdir or tempfile.mkdtemp(prefix="check_gen_")
    os.makedirs(workdir, exist_ok=True)
    gen_args = ["--profile", args.profile, "--seed", str(args.seed)]
    if args.lines is not None:
        gen_args += ["--lines", str(args.lines)]
    if args.kinds is not None:
        gen_args += ["--kinds", str(args.kinds)]

    tag = f"{args.profile}_s{args.seed}_{args.lines or 'default'}"
    a_out, a_truth = (os.path.join(workdir, f"{tag}.{s}") for s in ("a.ndjson", "a.truth.json"))
    b_out, b_truth = (os.path.join(workdir, f"{tag}.{s}") for s in ("b.ndjson", "b.truth.json"))
    run_gen(gen_args, a_out, a_truth)
    run_gen(gen_args, b_out, b_truth)
    check(read(a_out) == read(b_out), "same seed produced different NDJSON bytes")
    check(read(a_truth) == read(b_truth), "same seed produced different truth bytes")

    raw, truth_bytes = read(a_out), read(a_truth)
    counts, n_blank, n_lines, n_rejects = validate(raw, truth_bytes, args.profile)

    print(f"profile={args.profile} seed={args.seed} lines={n_lines} blank={n_blank} "
          f"rejects={n_rejects} bytes={len(raw)}")
    for case in sorted(counts, key=lambda c: (-counts[c], c)):
        print(f"  {case:24s} {counts[case]}")
    print("PASS: deterministic bytes, truth consistent with output")


if __name__ == "__main__":
    main()
