#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Compare two benchmark result files by case name: the JSON the layer benchmarks write with --json.

Usage: compare_bench.py OLD.json NEW.json [--threshold PERCENT]

Prints one row per case in either file, with the old and new median in nanoseconds and the change in percent (positive
means slower). A case in only one of the files is reported as missing in the other and does not fail the comparison.
Exits 1 when a case is slower than the threshold (default 15 percent) allows, 0 when none is, and 2 when a file cannot
be read.
"""

import argparse
import json
import sys

DEFAULT_THRESHOLD = 15.0


def load(path):
    """The median time per case name, from a result file: {"results": [...]} or a plain list of cases."""
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    cases = data["results"] if isinstance(data, dict) else data
    medians = {}
    for case in cases:
        name = case["name"]
        if name in medians:
            raise ValueError(f"{path}: case '{name}' is listed twice")
        medians[name] = float(case["median_ns"])
    return medians


def compare(old, new, threshold):
    """Compare two {name: median_ns} maps. Returns (rows, slower): rows are (name, old, new, change_percent, status)
    in the old file's order, then the new file's; slower is the list of names slower than the threshold."""
    names = list(old) + [name for name in new if name not in old]
    rows = []
    slower = []
    for name in names:
        if name not in new:
            rows.append((name, old[name], None, None, "missing in new"))
        elif name not in old:
            rows.append((name, None, new[name], None, "missing in old"))
        elif old[name] <= 0:
            rows.append((name, old[name], new[name], None, "no baseline time"))
        else:
            change = (new[name] - old[name]) * 100.0 / old[name]
            if change > threshold:
                rows.append((name, old[name], new[name], change, "SLOWER"))
                slower.append(name)
            else:
                rows.append((name, old[name], new[name], change, "ok"))
    return rows, slower


def format_table(rows):
    width = max([len("case")] + [len(name) for name, *_ in rows])
    lines = [f"{'case':<{width}}  {'old ns':>14}  {'new ns':>14}  {'change':>9}  status"]
    for name, old, new, change, status in rows:
        old_text = "-" if old is None else f"{old:.0f}"
        new_text = "-" if new is None else f"{new:.0f}"
        change_text = "-" if change is None else f"{change:+.1f}%"
        lines.append(f"{name:<{width}}  {old_text:>14}  {new_text:>14}  {change_text:>9}  {status}")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Compare two layer benchmark result files by case name.")
    parser.add_argument("old", help="the baseline result file")
    parser.add_argument("new", help="the result file to compare with the baseline")
    parser.add_argument(
        "--threshold",
        type=float,
        default=DEFAULT_THRESHOLD,
        help=f"fail when a case is slower by more than this many percent (default {DEFAULT_THRESHOLD:g})",
    )
    args = parser.parse_args(argv)
    try:
        old = load(args.old)
        new = load(args.new)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"compare_bench: {error}", file=sys.stderr)
        return 2
    rows, slower = compare(old, new, args.threshold)
    print(format_table(rows))
    missing = [name for name, _, _, _, status in rows if status.startswith("missing")]
    if missing:
        print(f"\n{len(missing)} case(s) missing from one file: {', '.join(missing)}")
    if slower:
        print(f"\n{len(slower)} case(s) slower by more than {args.threshold:g}%: {', '.join(slower)}")
        return 1
    print(f"\nno case is slower by more than {args.threshold:g}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
