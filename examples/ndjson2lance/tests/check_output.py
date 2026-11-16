#!/usr/bin/env python3
"""Checks ndjson2lance's output against the truth file written by gen_logs.py.

Usage: check_output.py OUT_DIR INPUT TRUTH {keys,event}

The checker splits and parses INPUT itself (its own lines, JSON and routing), and uses the truth
file only for the expected verdict of each line. It checks that:
  - every non-blank line is in exactly one table, in _rejects.lance, or unwritable (counts add up);
  - each table holds, in input order, the lines the truth expects in it, projected onto the table's
    Arrow schema the way Arrow reads JSON (a missing key is null, an unknown key is dropped, an
    integer becomes a float in a float column);
  - _rejects.lance holds the lines the truth expects to be rejected, each with its raw text, and a
    route-level reject (not an object, malformed, duplicate key) has an empty table name;
  - the catalog has one row per record shape, with its row, reject, first and last line counts.
A table that did not write is listed with its status. A status that mentions "null" is the known
list<null> gap in nanolance and is reported, not failed. Exit 0 when everything matches, 1 otherwise.
"""

import json
import pathlib
import sys
from collections import defaultdict

try:
    import lance
    import pyarrow as pa
except ImportError as error:
    sys.exit(f"check_output: needs pylance and pyarrow: {error}")

SEP = "\x1f"
MISSING = object()


class DuplicateKey(ValueError):
    pass


def object_hook(pairs):
    keys = [key for key, _ in pairs]
    if len(set(keys)) != len(keys):
        raise DuplicateKey("duplicate top-level key")
    return dict(pairs)


def read_input(path):
    """Returns {line number: (kind, raw text, value)} for every physical line, kind in
    blank, object, nonobject, malformed, duplicate."""
    data = pathlib.Path(path).read_bytes()
    parts = data.split(b"\n")
    if parts and parts[-1] == b"":
        parts.pop()
    lines = {}
    for number, raw in enumerate(parts, 1):
        if number == 1 and raw.startswith(b"\xef\xbb\xbf"):
            raw = raw[3:]
        if raw.endswith(b"\r"):
            raw = raw[:-1]
        if raw.strip(b" \t\r") == b"":
            lines[number] = ("blank", None, None)
            continue
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            lines[number] = ("malformed", raw.decode("utf-8", "replace"), None)
            continue
        try:
            value = json.loads(text, object_pairs_hook=object_hook)
        except DuplicateKey:
            lines[number] = ("duplicate", text, None)
            continue
        except ValueError:
            lines[number] = ("malformed", text, None)
            continue
        if isinstance(value, dict):
            lines[number] = ("object", text, value)
        else:
            lines[number] = ("nonobject", text, None)
    return lines


def route_of(value, mode):
    if mode == "keys":
        return SEP.join(sorted(value.keys()))
    event = value.get("event", MISSING)
    if event is MISSING:
        return "_no_event"
    if isinstance(event, str):
        return event
    return json.dumps(event, separators=(",", ":"), ensure_ascii=False)


def project(value, typ):
    """The value as Arrow reads it into typ (see the module docstring)."""
    if value is None:
        return None
    if pa.types.is_struct(typ):
        if not isinstance(value, dict):
            raise ValueError(f"expected an object for {typ}, got {value!r}")
        return {typ.field(i).name: project(value.get(typ.field(i).name), typ.field(i).type)
                for i in range(typ.num_fields)}
    if pa.types.is_list(typ):
        return [project(item, typ.value_type) for item in value]
    if pa.types.is_floating(typ):
        return float(value)
    if pa.types.is_integer(typ):
        return int(value)
    return value


def main(argv):
    if len(argv) != 5 or argv[4] not in ("keys", "event"):
        sys.exit(__doc__)
    out = pathlib.Path(argv[1])
    lines = read_input(argv[2])
    truth = json.loads(pathlib.Path(argv[3]).read_text(encoding="utf-8"))
    mode = argv[4]
    failures = []

    def check(ok, message):
        if not ok:
            failures.append(message)

    expect_field = "expect_by_keys" if mode == "keys" else "expect_by_event"
    expect = {entry["line"]: entry[expect_field] for entry in truth["per_line"]}
    input_blank = {n for n, (kind, _, _) in lines.items() if kind == "blank"}
    check(input_blank == set(truth["blank_line_numbers"]),
          "blank lines differ from the truth file")
    check(len(lines) - len(input_blank) == truth["lines"],
          f"non-blank line count {len(lines) - len(input_blank)} != truth {truth['lines']}")

    # Route of each object line, and the generator's own routing for the same line.
    heap_of = {}
    for number, (kind, _, value) in lines.items():
        if kind != "object":
            continue
        heap_of[number] = route_of(value, mode)
    for entry in truth["per_line"]:
        number = entry["line"]
        kind = lines[number][0]
        if kind == "object":
            if mode == "keys":
                check(entry["fingerprint"] == heap_of[number],
                      f"line {number}: routing differs from the truth file's fingerprint")
            elif entry["kind"] is not None:
                check(entry["kind"] == heap_of[number],
                      f"line {number}: routing differs from the truth file's event")
        else:
            check(expect[number] == "reject",
                  f"line {number} is {kind} but the truth file expects {expect[number]}")

    members = defaultdict(list)  # heap -> object line numbers, input order
    for number in sorted(heap_of):
        members[heap_of[number]].append(number)

    catalog = lance.dataset(str(out / "_catalog.lance")).to_table().to_pylist()
    routes = {row["route"]: row for row in catalog}
    check(len(routes) == len(catalog), "the catalog has two rows for one route")
    check(set(routes) == set(members),
          f"catalog routes differ: missing {sorted(set(members) - set(routes))[:5]}, "
          f"extra {sorted(set(routes) - set(members))[:5]}")

    total_rows = total_unwritable = 0
    failed_heaps = {}
    expected_rejects = set()
    rejects_path = out / "_rejects.lance"
    table_of = {}  # route -> table name
    for route, row in routes.items():
        table_of[route] = row["table"]
        total_rows += row["rows"]
        total_unwritable += row["unwritable"]
        objects = members.get(route, [])
        table_lines = [n for n in objects if expect[n] == "table"]
        reject_lines = [n for n in objects if expect[n] == "reject"]
        if row["status"] != "ok":
            # Which lines were rejected before the table failed depends on when it failed, so only
            # the totals are checked.
            failed_heaps[row["table"]] = row["status"]
            check(row["rows"] == 0, f"{row['table']}: failed, but has {row['rows']} rows")
            check(row["rejects"] + row["unwritable"] == len(objects),
                  f"{row['table']}: failed table's rejects+unwritable != {len(objects)} lines")
            continue
        check(row["rows"] == len(table_lines),
              f"{row['table']}: {row['rows']} rows, the truth expects {len(table_lines)}")
        check(row["rejects"] == len(reject_lines),
              f"{row['table']}: {row['rejects']} rejects, the truth expects {len(reject_lines)}")
        check(row["unwritable"] == 0, f"{row['table']}: {row['unwritable']} unwritable lines")
        if objects:
            check(row["first_line"] == objects[0] and row["last_line"] == objects[-1],
                  f"{row['table']}: first/last line {row['first_line']}/{row['last_line']} != "
                  f"{objects[0]}/{objects[-1]}")
        expected_rejects.update(reject_lines)

        dataset = lance.dataset(str(out / f"{row['table']}.lance"))
        got = dataset.to_table().to_pylist()
        root = pa.struct(list(dataset.schema))
        try:
            want = [project(lines[n][2], root) for n in table_lines]
        except ValueError as error:
            failures.append(f"{row['table']}: {error}")
            continue
        check(got == want,
              f"{row['table']}: table rows differ from the truth-projected input "
              f"({len(got)} read, {len(want)} expected)")

    # Route-level rejects have no table; every other reject must be in its heap's table.
    route_rejects = {n for n, (kind, _, _) in lines.items() if kind != "blank" and kind != "object"}
    expected_rejects |= route_rejects
    rejected = {}
    if rejects_path.exists():
        for row in lance.dataset(str(rejects_path)).to_table().to_pylist():
            if row["line"] in rejected:
                failures.append(f"line {row['line']} is in the rejects file twice")
            rejected[row["line"]] = row
    failing_lines = set()
    for route in routes:
        if routes[route]["status"] != "ok":
            failing_lines.update(members.get(route, []))
    actual = set(rejected)
    check(expected_rejects <= actual, f"rejects missing lines {sorted(expected_rejects - actual)[:10]}")
    check(actual <= expected_rejects | failing_lines,
          f"rejects has lines the truth does not expect: {sorted(actual - expected_rejects - failing_lines)[:10]}")
    for number, row in rejected.items():
        _, text, _ = lines.get(number, ("missing", None, None))
        check(row["raw"] == text, f"line {number}: rejected raw text differs from the input line")
        if number in heap_of:
            check(row["table"] == table_of[heap_of[number]],
                  f"line {number}: reject names table {row['table']!r}, its shape's table is "
                  f"{table_of[heap_of[number]]!r}")
        else:
            check(row["table"] == "", f"line {number}: a route-level reject must have no table")
    route_rejects_file = sum(1 for row in rejected.values() if row["table"] == "")
    check(route_rejects_file == len(route_rejects & actual),
          "route-level rejects in the file do not match the non-object/malformed lines")
    heap_rejects_file = len(rejected) - route_rejects_file
    check(heap_rejects_file == sum(r["rejects"] for r in catalog),
          "the rejects file's table rows do not match the catalog's reject counts")

    non_blank = len(lines) - len(input_blank)
    rejects_total = len(rejected)
    balance = total_rows + rejects_total + total_unwritable
    check(non_blank == balance,
          f"accounting: {non_blank} lines != {total_rows} rows + {rejects_total} rejects + "
          f"{total_unwritable} unwritable")

    # Each table directory on disk is a catalog table, the rejects file, or the catalog itself.
    known = {row["table"] for row in catalog} | {"_rejects", "_catalog"}
    for entry in out.iterdir():
        if entry.suffix == ".lance" and entry.stem not in known:
            failures.append(f"{entry.name} is not in the catalog")

    print(f"tables: {len(catalog)}  rows: {total_rows}  rejects: {rejects_total}  "
          f"unwritable: {total_unwritable}  input lines: {non_blank}")
    for table, status in sorted(failed_heaps.items()):
        known_gap = "null" in status.lower()
        print(f"  failed {table}: {status}" + ("  [known list<null> gap]" if known_gap else ""))
        if not known_gap:
            failures.append(f"{table} failed for a reason other than the null gap: {status}")
    if failures:
        print(f"FAIL ({len(failures)})")
        for message in failures[:50]:
            print(f"  {message}")
        return 1
    print("OK: every table, the rejects and the catalog match the truth file")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
