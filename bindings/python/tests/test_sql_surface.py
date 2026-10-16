# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""SQL expressions in a column projection (docs/tasks/SQL_SURFACE.md, S1): ``columns={"alias": "<sql>"}``
in to_table / scanner / to_batches / take / _take_rows.

Each case runs under real pylance and under nanolance (``install_as_lance``), in a subprocess, over the
same dataset built the same way, and the two results -- the schema (name, type, nullability) and the
rows, or the exception class -- must agree.

Left out, because the engines differ (measured on pylance 12.0.0): mixed-type arithmetic result types
(int32 + float32 is float32 in pylance, double here; uint8 + int32 is int32 there, int64 here); a float
literal against an int32 column (an error in pylance, a double here); abs(int32 - literal) (int64 in
pylance); coalesce with a literal fallback (non-null in pylance); `s['x']` and list indexing (not in
nanolance's expression grammar yet); CASE, SUBSTR, CONCAT, round (not in nanolance); the trailing
`)` that pylance ignores; `S.X` (nanolance matches struct names case-insensitively).
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from tests.support import require_pylance

NANOLANCE_PATH = Path(__file__).resolve().parents[1]
PRELUDE = {
    "pylance": "import lance\n",
    "nanolance": "import nanolance.lance as _nl\n_nl.install_as_lance()\nimport lance\n",
}

# The runner: builds the dataset named by "setup" under a temp dir, runs each case, prints JSON.
RUNNER = r'''
import json, sys, tempfile
import pyarrow as pa
req = json.load(sys.stdin)
root = tempfile.mkdtemp()
path = root + "/ds.lance"
rows = [
    {"a": i + 1, "a64": (i + 1) * 10, "f": i / 2, "d": i * 0.25,
     "b": None if i % 4 == 3 else "w%d" % i, "flag": i % 2 == 0,
     "s": {"x": i, "y": i % 3 == 0}, "tags": [i, i + 1, i + 2], "vec": [float(i), float(2 * i)]}
    for i in range(10)
]
table = pa.Table.from_pylist(rows, schema=pa.schema([
    ("a", pa.int32()), ("a64", pa.int64()), ("f", pa.float32()), ("d", pa.float64()), ("b", pa.string()),
    ("flag", pa.bool_()), ("s", pa.struct([("x", pa.int64()), ("y", pa.bool_())])),
    ("tags", pa.list_(pa.int64())), ("vec", pa.list_(pa.float32(), 2))]))
lance.write_dataset(table, path, max_rows_per_file=100 if req["setup"] == "single" else 4)
if req["setup"] == "fragments_deleted":
    ds = lance.dataset(path)
    ds.delete("a = 5")
    more = pa.Table.from_pylist([{"a": 11, "a64": 110, "f": 5.5, "d": 2.75, "b": "w11", "flag": False,
                                  "s": {"x": 11, "y": True}, "tags": [1], "vec": [11.0, 22.0]}],
                                schema=table.schema)
    lance.write_dataset(more, path, mode="append")
elif req["setup"] == "indexed":
    lance.dataset(path).create_scalar_index("b", index_type="INVERTED")
ds = lance.dataset(path)


def project(case):
    kwargs = dict(case.get("kwargs", {}))
    method = case["method"]
    if method == "to_table":
        return ds.to_table(**kwargs)
    if method == "scanner":
        return ds.scanner(**kwargs).to_table()
    if method == "to_batches":
        batches = list(ds.to_batches(**kwargs))
        return pa.Table.from_batches(batches, schema=batches[0].schema) if batches else None
    if method == "take":
        return ds.take(case["rows"], **kwargs)
    if method == "take_rows":
        return ds._take_rows(case["rows"], **kwargs)
    raise ValueError(method)


out = []
for case in req["cases"]:
    try:
        table = project(case)
        if table is None:
            out.append({"schema": [], "rows": []})
            continue
        out.append({"schema": [[f.name, str(f.type), f.nullable] for f in table.schema],
                    "rows": table.to_pylist()})
    except Exception as error:  # the class is what must agree, not the message
        out.append({"error": type(error).__name__})
print(json.dumps(out, default=str))
'''


def _run(engine: str, setup: str, cases: list) -> list:
    env = {"PATH": os.environ.get("PATH", "")}
    if engine == "nanolance":
        env["PYTHONPATH"] = str(NANOLANCE_PATH)
    code = PRELUDE[engine] + RUNNER
    result = subprocess.run([sys.executable, "-c", code], input=json.dumps({"setup": setup, "cases": cases}),
                            capture_output=True, text=True, env=env, timeout=600)
    assert result.returncode == 0, f"{engine}: {result.stderr[-2000:]}"
    return json.loads(result.stdout.strip().splitlines()[-1])


def _compare(cases: list, setup: str = "plain", unordered: bool = False) -> None:
    """Every case gives the same schema and rows (or the same error class) under both engines. With
    ``unordered``, the rows are compared as a multiset: a scan over several fragments returns them in no
    fixed order."""
    require_pylance()
    expected = _run("pylance", setup, cases)
    actual = _run("nanolance", setup, cases)
    if unordered:
        for result in expected + actual:
            if "rows" in result:
                result["rows"] = sorted(result["rows"], key=lambda row: json.dumps(row, sort_keys=True))
    for case, want, got in zip(cases, expected, actual):
        assert got == want, f"{case}: pylance {want} != nanolance {got}"


def _to_table(exprs: dict, **kwargs) -> dict:
    return {"method": "to_table", "kwargs": {"columns": exprs, **kwargs}}


PROJECTIONS = [
    {"a2": "a*2", "bup": "UPPER(b)"},
    {"a2": "a*2"},
    {"x": "a64 * a64", "c": "CAST(a AS DOUBLE)"},
    {"half": "a64 / 4", "mod": "a64 % 7", "neg": "-a"},
    {"f2": "f * 2", "d4": "d / 4"},
    {"sum": "a64 + a", "diff": "a - a64"},
    {"lo": "LOWER(b)", "n": "length(b)", "cat": "b || '!'"},
    {"bool": "a > 5", "eq": "a = 2", "not": "NOT flag", "isn": "b IS NULL", "in": "a IN (1, 2)"},
    {"v": "1", "a": "a"},
    {"x": "s.x", "y": "s.y", "xx": "s.x * 10"},
    {"first": "a", "plus": "a + 1", "name": "b"},
    {"r": "a", "e": "abs(a)"},
    {"v": "_rowid", "d": "a * 2"},
    {"lit": "'text'", "a": "a"},
    {"dist": "f + d", "g": "abs(a)"},
]

ERRORS = [
    {"x": "nope * 2"},
    {"x": "a +"},
    {"x": "1"},
    {"x": "SELECT a"},
    {"x": "CASE WHEN a > 1 THEN 1 ELSE 0 END"},
    {"x": "a * 2", "y": "_rowid + 1"},
]


@pytest.mark.parametrize("projection", PROJECTIONS, ids=lambda p: ",".join(p))
def test_projection_matches_pylance(projection):
    _compare([_to_table(projection)])


def test_projection_with_filters_limit_and_offset():
    # One fragment: an unordered scan over several fragments returns rows in no fixed order, so the
    # rows that limit and offset keep would differ between the engines.
    cases = [
        _to_table({"a2": "a * 2", "bup": "UPPER(b)"}, filter="a > 3"),
        _to_table({"a2": "a * 2", "flag": "flag"}, filter="flag AND a < 8", limit=2),
        _to_table({"a2": "a * 2", "tag": "b"}, filter="b IS NOT NULL", limit=3, offset=2),
        _to_table({"n": "a64 + 1"}, filter="s.y", offset=1),
        _to_table({"n": "a64 + 1", "a": "a"}, limit=0),
        _to_table({"n": "a64 + 1"}, filter="a > 100"),
    ]
    _compare(cases, setup="single")


def test_projection_in_small_batches():
    # Batches of two rows come back in no fixed order under pylance: compared as a multiset.
    cases = [_to_table({"a2": "a * 2", "bup": "UPPER(b)"}, filter="a > 2", batch_size=2)]
    _compare(cases, setup="single", unordered=True)


def test_projection_over_fragments_and_deletions():
    cases = [
        _to_table({"a2": "a * 2", "bup": "UPPER(b)"}),
        _to_table({"a2": "a * 2", "bup": "UPPER(b)"}, filter="a >= 5"),
        {"method": "scanner", "kwargs": {"columns": {"a2": "a * 2", "x": "s.x"}, "filter": "a < 7"}},
        {"method": "to_batches", "kwargs": {"columns": {"a2": "a * 2", "bup": "UPPER(b)"}}},
    ]
    _compare(cases, setup="fragments_deleted", unordered=True)


def test_take_and_take_rows_with_projection():
    cases = [
        {"method": "take", "rows": [0, 2, 9], "kwargs": {"columns": {"a2": "a*2", "bup": "UPPER(b)"}}},
        {"method": "take", "rows": [1, 4], "kwargs": {"columns": {"a2": "a * 2", "a": "a", "x": "s.x"}}},
        {"method": "take_rows", "rows": [0], "kwargs": {"columns": {"a2": "a*2", "bup": "UPPER(b)"}}},
        {"method": "take_rows", "rows": [1, 2], "kwargs": {"columns": {"n": "a64 + 1", "t": "tags"}}},
        {"method": "take", "rows": [0], "kwargs": {"columns": {"bool": "a > 5"}}},
    ]
    _compare(cases)


def test_projection_with_knn_and_fts():
    cases = [
        _to_table({"a2": "a * 2"}, nearest={"column": "vec", "q": [1.0, 2.0], "k": 3}),
        _to_table({"a2": "a * 2", "d": "_distance"}, nearest={"column": "vec", "q": [1.0, 2.0], "k": 3}),
        _to_table({"a2": "a * 2"}, nearest={"column": "vec", "q": [1.0, 2.0], "k": 3}, filter="a > 4"),
        _to_table({"s2": "a * 2"}, full_text_query="w3"),
    ]
    _compare(cases, setup="indexed")


def test_projection_errors_match_pylance():
    _compare([_to_table(p) for p in ERRORS])
