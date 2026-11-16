#!/usr/bin/env python3
"""Baseline for ndjson2lance: the plain pyarrow.json + pylance route.

Usage: baseline.py INPUT OUT_DIR [--route-key FIELD]

Reads INPUT line by line and parses each line with json.loads. Blank lines are skipped. A line that
is not a JSON object, or (with --route-key) has no FIELD, is a reject. Lines are grouped by the sorted
tuple of their top-level keys, or by the JSON value of FIELD. Each group is read with
pyarrow.json.read_json (schema inferred from the whole group) and written with lance.write_dataset to
OUT_DIR/tNN.lance.

If a group fails to read, bad lines are found by bisection (read_json on each half, recursively), are
counted as rejects, and the rest of the group is read again. A group that still fails is unwritable.

Top-level keys with a "." are renamed to "_" before routing, because pylance cannot store them (without
the rename most tables fail to write). Collisions after the rename are not handled. A table that still
fails to write is counted unwritable.

Not done, on purpose: no schema lock or learning window, no staging or flushes (each group is held in
memory whole), no duplicate-key check (json.loads keeps the last value), no multiprocessing. Prints one
summary line: "baseline: tables= rows= rejects= unwritable=".
"""

import argparse
import collections
import io
import json
import os
import shutil
import sys

import lance
import pyarrow as pa
import pyarrow.json as pj


def read_group(lines):
    return pj.read_json(io.BytesIO(b"".join(lines)))


def bad_lines(lines):
    """The lines that make the group fail, found by bisection (a conflict between two good lines is not found)."""
    if not lines:
        return []
    try:
        read_group(lines)
        return []
    except pa.lib.ArrowException:
        if len(lines) == 1:
            return lines
        mid = len(lines) // 2
        return bad_lines(lines[:mid]) + bad_lines(lines[mid:])


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("input")
    ap.add_argument("out_dir")
    ap.add_argument("--route-key", metavar="FIELD")
    args = ap.parse_args(argv)

    if os.path.isdir(args.out_dir) and os.listdir(args.out_dir):
        sys.exit("baseline: out_dir must not exist or be empty")
    os.makedirs(args.out_dir, exist_ok=True)

    groups = collections.OrderedDict()
    rejects = 0
    with open(args.input, "rb") as f:
        for raw in f:
            if not raw.strip():
                continue
            try:
                obj = json.loads(raw)
            except ValueError:
                rejects += 1
                continue
            if not isinstance(obj, dict):
                rejects += 1
                continue
            if any("." in k for k in obj):
                # pylance cannot store a top-level "user.id"; the same "." -> "_" rename as ndjson2lance.
                obj = {k.replace(".", "_"): v for k, v in obj.items()}
                raw = json.dumps(obj).encode() + b"\n"
            if args.route_key is None:
                key = tuple(sorted(obj))
            elif args.route_key in obj:
                key = json.dumps(obj[args.route_key], sort_keys=True)
            else:
                rejects += 1
                continue
            groups.setdefault(key, []).append(raw)

    tables = rows = unwritable = 0
    for idx, lines in enumerate(groups.values()):
        try:
            table = read_group(lines)
        except pa.lib.ArrowException:
            bad = {id(line) for line in bad_lines(lines)}
            rejects += len(bad)
            lines = [line for line in lines if id(line) not in bad]
            try:
                table = read_group(lines)
            except pa.lib.ArrowException:
                unwritable += len(lines)
                continue
        try:
            lance.write_dataset(table, os.path.join(args.out_dir, f"t{idx:02d}.lance"))
        except OSError:
            # pylance refuses a top-level column name with a dot ("user.id"); the tool renames such keys.
            shutil.rmtree(os.path.join(args.out_dir, f"t{idx:02d}.lance"), ignore_errors=True)
            unwritable += table.num_rows
            continue
        tables += 1
        rows += table.num_rows

    print(f"baseline: tables={tables} rows={rows} rejects={rejects} unwritable={unwritable}")


if __name__ == "__main__":
    main()
