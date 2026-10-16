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
    if method in ("sql", "sql_stream"):  # S2: ds.sql(query), with builder options [name, value] in order
        builder = ds.sql(case["query"])
        for name, value in case.get("options", []):
            builder = getattr(builder, name)(value)
        query = builder.build()
        if method == "sql":
            batches = query.to_batch_records()
            SIZES[:] = [b.num_rows for b in batches]
            return pa.Table.from_batches(batches, schema=batches[0].schema) if batches else None
        reader = query.to_stream_reader()
        batches = list(reader)
        SIZES[:] = [b.num_rows for b in batches]
        return pa.Table.from_batches(batches, schema=reader.schema)
    raise ValueError(method)


SIZES = []  # the batch sizes of the last SQL case
out = []
for case in req["cases"]:
    SIZES[:] = []
    try:
        table = project(case)
        if table is None:
            out.append({"schema": [], "rows": [], "sizes": list(SIZES)})
            continue
        out.append({"schema": [[f.name, str(f.type), f.nullable] for f in table.schema],
                    "rows": table.to_pylist(), "sizes": list(SIZES)})
    except Exception as error:  # the class is what must agree, not the message
        entry = {"error": type(error).__name__}
        if type(error).__name__ == "NotSupportedError":  # nanolance's alone: its message names the construct
            entry["message"] = str(error)
        out.append(entry)
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


def _compare(cases: list, setup: str = "plain", unordered: bool = False, sizes: bool = True) -> None:
    """Every case gives the same schema and rows (or the same error class) under both engines. With
    ``unordered``, the rows are compared as a multiset: a scan over several fragments returns them in no
    fixed order."""
    require_pylance()
    expected = _run("pylance", setup, cases)
    actual = _run("nanolance", setup, cases)
    if not sizes:  # the batch sizes of a scan over fragments (see the S2 notes)
        for result in expected + actual:
            result.pop("sizes", None)
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


# S2 (docs/tasks/SQL_SURFACE.md): ``LanceDataset.sql(...)``. The statement is parsed by a small SELECT
# grammar; WHERE is a nanolance filter; the select list is S1's projection. Compared with pylance on the
# same datasets: schema, rows and batch sizes. Not compared, because the engines differ on purpose:
# the names of unaliased computed columns (pylance: ``dataset.id + Int64(1)``), the cut of batch_size_bytes
# (a byte budget), and EXPLAIN. Only aliased computed columns are compared, and ORDER BY keys are unique
# or tie on equal values, since ties in a sort are not fixed across engines.


def _sql(query, options=(), stream=False):
    return {"method": "sql_stream" if stream else "sql", "query": query,
            "options": [list(option) for option in options]}


SQL_QUERIES = [
    "SELECT * FROM dataset",
    "select a, b from DATASET where a > 3",
    "SELECT a64 * 2 AS a2, UPPER(b) AS bup FROM dataset WHERE a > 3",
    "SELECT a64 + a AS sum, a - a64 AS diff FROM dataset",
    "SELECT LOWER(b) AS lo, length(b) AS n FROM dataset WHERE b IS NOT NULL",
    "SELECT a > 5 AS bool, a = 2 AS eq, NOT flag AS nf, b IS NULL AS isn FROM dataset",
    "SELECT a IN (1, 2) AS in_list FROM dataset",
    "SELECT a, flag FROM dataset WHERE flag AND a < 8 ORDER BY a DESC LIMIT 2",
    "SELECT *, a64 + 1 AS a1 FROM dataset WHERE a BETWEEN 2 AND 6 ORDER BY a DESC",
    "SELECT a AS x FROM dataset ORDER BY b, a",
    "SELECT a AS x FROM dataset ORDER BY b DESC, a",
    "SELECT a AS x FROM dataset ORDER BY b ASC NULLS FIRST, a",
    "SELECT flag FROM dataset ORDER BY flag",
    "SELECT a, b FROM dataset ORDER BY a DESC LIMIT 3 OFFSET 2",
    "SELECT a FROM dataset ORDER BY flag, a DESC",
    "SELECT a FROM dataset ORDER BY a LIMIT 2 OFFSET 8",
    "SELECT a FROM dataset OFFSET 7",
    "SELECT a AS x FROM dataset LIMIT 0",
    "SELECT a FROM dataset WHERE a > 100",
    "SELECT count(*) FROM dataset",
    "SELECT count(*) AS n FROM dataset WHERE flag",
    "SELECT 'it''s' AS s, 1 AS one, a FROM dataset LIMIT 2",
    'SELECT a AS "Full Name", b AS "b" FROM dataset LIMIT 2',
    "SELECT a AS x FROM dataset WHERE b = 'w3' OR b IS NULL",
    "SELECT tags, vec, s FROM dataset WHERE a = 2",
    "SELECT s.x AS sx, s.y AS sy FROM dataset WHERE s.y",
    "SELECT a FROM dataset WHERE b IS NOT NULL ORDER BY b DESC LIMIT 2 OFFSET 1",
]


@pytest.mark.parametrize("query", SQL_QUERIES, ids=lambda q: q.replace(" ", "_")[:50])
def test_sql_matches_pylance(query):
    _compare([_sql(query)], setup="single")


def test_sql_batch_sizes_and_stream_reader():
    cases = [
        _sql("SELECT a FROM dataset ORDER BY a", [("batch_size", 3)]),
        _sql("SELECT a FROM dataset", [("batch_size", 4)]),
        _sql("SELECT a AS x FROM dataset ORDER BY a DESC LIMIT 7", [("batch_size", 2)]),
        _sql("SELECT a FROM dataset WHERE a > 100", stream=True),
        _sql("SELECT a, b FROM dataset ORDER BY b, a", [("batch_size", 1)], stream=True),
        _sql("SELECT a FROM dataset", [("batch_size", 3)], stream=True),
    ]
    _compare(cases, setup="single")


def test_sql_builder_options_over_fragments_and_deletions():
    cases = [
        _sql("SELECT a, b AS bb FROM dataset WHERE a < 9", [("batch_size", 3)]),
        _sql("SELECT a FROM dataset ORDER BY a", [("batch_size", 3)]),
        _sql("SELECT a AS x FROM dataset WHERE a < 4 ORDER BY a DESC", [("with_row_addr", True)]),
        _sql("SELECT a FROM dataset WHERE a < 4 ORDER BY a", [("with_row_id", True)]),
        _sql("SELECT *, a64 + 1 AS a1 FROM dataset WHERE a < 3 ORDER BY a",
             [("with_row_id", True), ("with_row_addr", True)]),
        _sql("SELECT count(*) AS n FROM dataset", [("with_row_id", True)]),
        _sql("SELECT a FROM dataset ORDER BY a LIMIT 2 OFFSET 1", [("table_name", "dataset")]),
    ]
    _compare(cases, setup="fragments_deleted", unordered=True, sizes=False)


def test_sql_table_name_and_blob_handling():
    cases = [
        _sql("SELECT a FROM mine WHERE a < 3", [("table_name", "mine")]),
        _sql("SELECT a FROM Mine WHERE a < 2", [("table_name", "MINE")]),
        _sql("SELECT a FROM dataset", [("table_name", "mine")]),
        _sql("SELECT a FROM dataset LIMIT 1", [("blob_handling", "all_binary")]),
        _sql("SELECT a FROM dataset LIMIT 1", [("blob_handling", "bogus")]),
    ]
    _compare(cases, setup="single")


SQL_ERRORS = [
    "SELECT nope FROM dataset",
    "SELECT a FROM other",
    "SELECT a FROM dataset WHERE nope = 1",
    "SELECT a, a FROM dataset",
    "SELECT a FROM dataset ORDER BY nope",
    "SELECT a FROM dataset WHERE a = 'x'",
    "SELECT a FROM dataset WHERE",
    "",
    "SELECT a FROM dataset LIMIT x",
]


def test_sql_errors_match_pylance():
    cases = [_sql(query) for query in SQL_ERRORS] + [
        _sql("SELECT * FROM dataset", [("batch_size", 0)]),
        _sql("SELECT * FROM dataset", [("batch_size", 2**32)]),
        _sql("SELECT * FROM dataset", [("batch_size", -1)]),
        _sql("SELECT * FROM dataset", [("batch_size_bytes", -5)]),
    ]
    _compare(cases, setup="single")


UNSUPPORTED = [
    ("SELECT a FROM dataset GROUP BY a", "GROUP BY"),
    ("SELECT sum(a) AS s FROM dataset", "sum()"),
    ("SELECT a, count(*) FROM dataset", "count()"),
    ("SELECT count(a) FROM dataset", "count()"),
    ("SELECT a FROM dataset JOIN other ON a = a", "JOIN"),
    ("SELECT a FROM dataset, other", "several tables"),
    ("SELECT * FROM (SELECT a FROM dataset)", "subqueries in FROM"),
    ("SELECT a FROM dataset WHERE a IN (SELECT a FROM dataset)", "subqueries"),
    ("SELECT a FROM dataset d", "table aliases"),
    ("SELECT DISTINCT a FROM dataset", "DISTINCT"),
    ("SELECT a FROM dataset ORDER BY a + 1", "ORDER BY an expression"),
    ("SELECT a FROM dataset ORDER BY 1", "column position"),
    ("SELECT a FROM dataset UNION SELECT a FROM dataset", "UNION"),
    ("SELECT count(*) FROM dataset LIMIT 2", "count(*)"),
    ("SELECT a, row_number() OVER () FROM dataset", "window functions"),
]


def test_sql_unsupported_constructs_are_named():
    # Nothing to compare: pylance runs these. nanolance refuses each one, naming the construct.
    results = _run("nanolance", "single", [_sql(query) for query, _ in UNSUPPORTED])
    for (query, construct), result in zip(UNSUPPORTED, results):
        assert result.get("error") == "NotSupportedError", query
        assert construct in result["message"], (query, result)
