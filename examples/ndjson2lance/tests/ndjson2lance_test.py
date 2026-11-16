#!/usr/bin/env python3
"""ctest cases for ndjson2lance: python3 ndjson2lance_test.py CASE --tool PATH_TO_BINARY

Each CASE is one ctest entry (label "ndjson2lance"). Exit 0: the case passed. Exit 1: it failed, with
the reason on stderr. Exit 77 (ctest SKIP): pylance or pyarrow is not installed, nothing else.
"""

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    import lance
    import pyarrow  # noqa: F401 - pylance returns pyarrow tables
except ImportError as error:
    print(f"skip: {error}")
    sys.exit(77)

HERE = Path(__file__).resolve().parent
GEN = HERE / "gen_logs.py"
CHECK = HERE / "check_output.py"
SUMMARY = re.compile(
    r"ndjson2lance: (\d+) input lines \((\d+) blank\), (\d+) tables, (\d+) rows, (\d+) rejects, "
    r"(\d+) new-key lines, (\d+) unwritable"
)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(tool, out, *args, stdin=None):
    """Runs the tool with out as its output directory. Returns (exit code, stderr)."""
    process = subprocess.run([str(tool), str(out), *args], capture_output=True, text=True,
                             stdin=stdin, timeout=120)
    return process.returncode, process.stderr


def summary(stderr):
    match = SUMMARY.search(stderr)
    check(match is not None, f"no summary line in stderr:\n{stderr}")
    lines, blank, tables, rows, rejects, _, unwritable = map(int, match.groups())
    return {"lines": lines, "blank": blank, "tables": tables, "rows": rows, "rejects": rejects,
            "unwritable": unwritable}


def read_outputs(out):
    """(catalog rows, {table: rows}, rejects rows) of an output directory, read by pylance."""
    catalog = lance.dataset(str(out / "_catalog.lance")).to_table().to_pylist()
    tables = {row["table"]: lance.dataset(str(out / f"{row['table']}.lance")).to_table().to_pylist()
              for row in catalog if row["status"] == "ok"}
    rejects_path = out / "_rejects.lance"
    rejects = lance.dataset(str(rejects_path)).to_table().to_pylist() if rejects_path.exists() else []
    return catalog, tables, rejects


def generate(work, seed=1):
    """A small seeded input (gen_logs.py) and its truth file, in work."""
    input_path, truth_path = work / "input.ndjson", work / "truth.json"
    subprocess.run([sys.executable, str(GEN), "--seed", str(seed), "--out", str(input_path),
                    "--truth", str(truth_path)], check=True)
    return input_path, truth_path


def case_route(tool, mode):
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        input_path, truth_path = generate(work)
        out = work / "out"
        args = ["-i", str(input_path)] + (["--route-key", "event"] if mode == "event" else [])
        code, stderr = run(tool, out, *args)
        check(code == 0, f"exit {code}:\n{stderr}")
        counts = summary(stderr)
        check(counts["rows"] + counts["rejects"] + counts["unwritable"] ==
              counts["lines"] - counts["blank"],
              f"accounting: {counts} does not balance")
        checker = subprocess.run([sys.executable, str(CHECK), str(out), str(input_path),
                                  str(truth_path), mode], capture_output=True, text=True)
        check(checker.returncode == 0, f"check_output.py:\n{checker.stdout}{checker.stderr}")


def case_determinism(tool):
    """Same rows, in the same order per table, whatever the threads and the publish mode."""
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        input_path, _ = generate(work)
        variants = {
            "default": [],
            "threads4": ["--threads", "4", "--flush-mb", "1"],
            "one_version": ["--one-version"],
        }
        results = {}
        for name, extra in variants.items():
            out = work / name
            code, stderr = run(tool, out, "-i", str(input_path), *extra)
            check(code == 0, f"{name}: exit {code}:\n{stderr}")
            results[name] = read_outputs(out)
        base = results["default"]
        for name in ("threads4", "one_version"):
            check(results[name] == base, f"{name} output differs from the default output")


def case_tiny(tool):
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        empty = work / "empty.ndjson"
        empty.write_bytes(b"")
        code, stderr = run(tool, work / "empty", "-i", str(empty))
        check(code == 0, f"empty input: exit {code}:\n{stderr}")
        catalog, tables, rejects = read_outputs(work / "empty")
        check(catalog == [] and tables == {} and rejects == [],
              f"empty input: catalog {catalog}, tables {list(tables)}, rejects {rejects}")

        one = work / "one.ndjson"
        one.write_bytes(b'{"a":1}\n')
        code, stderr = run(tool, work / "one", "-i", str(one))
        check(code == 0, f"one line: exit {code}:\n{stderr}")
        catalog, tables, rejects = read_outputs(work / "one")
        check([row["table"] for row in catalog] == ["t01_a"] and rejects == [],
              f"one line: catalog {catalog}, rejects {rejects}")
        check(tables == {"t01_a": [{"a": 1}]}, f"one line: rows {tables}")


def case_longline(tool):
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        long_line = work / "long.ndjson"
        payload = "x" * (2 * 1024 * 1024)
        long_line.write_text('{"msg":"' + payload + '"}\n', encoding="utf-8")
        code, stderr = run(tool, work / "small", "-i", str(long_line), "--max-line-mb", "1")
        check(code == 1, f"2 MB line with --max-line-mb 1: exit {code}, expected 1")
        check("is longer than --max-line-mb" in stderr, f"no long-line message:\n{stderr}")
        code, stderr = run(tool, work / "big", "-i", str(long_line), "--max-line-mb", "4")
        check(code == 0, f"2 MB line with --max-line-mb 4: exit {code}:\n{stderr}")
        _, tables, rejects = read_outputs(work / "big")
        check(rejects == [] and [len(row["msg"]) for row in tables["t01_msg"]] == [len(payload)],
              f"2 MB line not stored intact: rejects {rejects}")
        check(tables["t01_msg"][0]["msg"] == payload, "2 MB line changed in the table")


def case_hostile(tool):
    """Each input with the exact rows and rejects observed when the tool was run on it."""
    rows = [{"a": 1}, {"a": 2}]
    cases = [
        # name, input bytes, expected rows (table t01_a), expected rejects [(line, reason prefix)]
        ("bom", b'\xef\xbb\xbf{"a":1}\n{"a":2}\n', rows, []),
        ("crlf", b'{"a":1}\r\n{"a":2}\r\n', rows, []),
        ("no_final_newline", b'{"a":1}\n{"a":2}', rows, []),
        ("dup_key", b'{"a":1,"a":2}\n{"a":3}\n', [{"a": 3}],
         [(1, "duplicate top-level key: a")]),
        ("truncated", b'{"a":1}\n{"a":2\n', [{"a": 1}],
         [(2, "INCOMPLETE_ARRAY_OR_OBJECT")]),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for name, data, want_rows, want_rejects in cases:
            path = work / f"{name}.ndjson"
            path.write_bytes(data)
            code, stderr = run(tool, work / name, "-i", str(path))
            check(code == 0, f"{name}: exit {code}:\n{stderr}")
            _, tables, rejects = read_outputs(work / name)
            got_rows = tables.get("t01_a", [])
            check(got_rows == want_rows, f"{name}: rows {got_rows}, expected {want_rows}")
            got = [(r["line"], r["reason"]) for r in rejects]
            check(len(got) == len(want_rejects) and all(
                line == want[0] and reason.startswith(want[1])
                for (line, reason), want in zip(got, want_rejects)),
                  f"{name}: rejects {got}, expected {want_rejects}")

        # Non-object lines: no table at all, four rejects with the raw lines.
        path = work / "nonobject.ndjson"
        path.write_bytes(b'[1,2]\n42\n"s"\nnull\n')
        code, stderr = run(tool, work / "nonobject", "-i", str(path))
        check(code == 0, f"nonobject: exit {code}:\n{stderr}")
        catalog, tables, rejects = read_outputs(work / "nonobject")
        check(catalog == [] and tables == {}, f"nonobject: tables {list(tables)}")
        check([(r["line"], r["table"], r["raw"]) for r in rejects] ==
              [(1, "", "[1,2]"), (2, "", "42"), (3, "", '"s"'), (4, "", "null")],
              f"nonobject: rejects {rejects}")
        check(all(r["reason"] == "a record must be a JSON object" for r in rejects),
              f"nonobject: reasons {[r['reason'] for r in rejects]}")


def case_dotted(tool):
    """A top-level key with a dot is written with '_' (pylance reads it); a collision gets _2."""
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        path = work / "dotted.ndjson"
        path.write_bytes(b'{"user.id":"u1","x":1}\n{"user.id":"u2","x":2}\n')
        code, stderr = run(tool, work / "dotted", "-i", str(path))
        check(code == 0, f"dotted: exit {code}:\n{stderr}")
        catalog, tables, _ = read_outputs(work / "dotted")
        check(json.loads(catalog[0]["schema"])["children"][0]["source_name"] == "user.id",
              "dotted: catalog does not record source_name user.id")
        check(tables[catalog[0]["table"]] == [{"user_id": "u1", "x": 1}, {"user_id": "u2", "x": 2}],
              f"dotted: rows {tables[catalog[0]['table']]}")

        path = work / "collide.ndjson"
        path.write_bytes(b'{"a.b":1,"a_b":"x"}\n{"a.b":2,"a_b":"y"}\n')
        code, stderr = run(tool, work / "collide", "-i", str(path))
        check(code == 0, f"collision: exit {code}:\n{stderr}")
        catalog, tables, _ = read_outputs(work / "collide")
        rows = tables[catalog[0]["table"]]
        check(set(rows[0]) == {"a_b", "a_b_2"}, f"collision: columns {sorted(rows[0])}")
        check(rows == [{"a_b": "x", "a_b_2": 1}, {"a_b": "y", "a_b_2": 2}],
              f"collision: rows {rows}")
        sources = {c["name"]: c.get("source_name") for c in
                   json.loads(catalog[0]["schema"])["children"]}
        check(sources == {"a_b_2": "a.b", "a_b": None}, f"collision: sources {sources}")


CASES = {
    "route-keys": lambda tool: case_route(tool, "keys"),
    "route-event": lambda tool: case_route(tool, "event"),
    "determinism": case_determinism,
    "tiny": case_tiny,
    "longline": case_longline,
    "hostile": case_hostile,
    "dotted": case_dotted,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", choices=sorted(CASES))
    parser.add_argument("--tool", required=True, type=Path)
    args = parser.parse_args()
    try:
        CASES[args.case](args.tool)
    except AssertionError as error:
        print(f"{args.case}: {error}", file=sys.stderr)
        return 1
    print(f"{args.case}: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
