"""JSON columns, against pylance.

Lance stores a pa.json_() column as JSONB (logical type "json", a large_binary column marked
lance.json) and reads it back as arrow.json text, normalized: compact, keys in byte order, numbers
reprinted. nanolance must write the same bytes, read pylance's, print the same text, and answer the
json_* filter functions as pylance does.
"""

from __future__ import annotations

import glob
import json
import os
import random

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


def _docs(n, seed):
    rng = random.Random(seed)

    def value(depth=0):
        k = rng.random()
        if depth > 2 or k < 0.4:
            return rng.choice([None, True, False, rng.randint(-1000, 1000), rng.randint(0, 2**64 - 1),
                               rng.uniform(-1e3, 1e3), rng.choice([0.5, 1e20, 1e-7, -0.0, 2.5e3]),
                               rng.choice(["Alice", "bob", "", "x\"y", "é", "yes", "42", "3.7", "true"])])
        if k < 0.7:
            return [value(depth + 1) for _ in range(rng.randint(0, 4))]
        return {rng.choice(["name", "age", "tags", "user", "score", "active", "B", "a"]): value(depth + 1)
                for _ in range(rng.randint(0, 5))}

    out = []
    for i in range(n):
        doc = {"name": rng.choice(["Alice", "Bob", "Carol", None]), "age": rng.randint(10, 60),
               "score": round(rng.uniform(0, 100), 2), "active": rng.random() < 0.5,
               "tags": rng.sample(["python", "rust", "go", "c"], rng.randint(0, 3)),
               "user": {"profile": {"theme": rng.choice(["dark", "light"])}, "x": value()}, "extra": value()}
        out.append(None if i % 17 == 0 else json.dumps(doc))
    return out


def _table(docs):
    return pa.table({"id": range(len(docs)), "data": pa.array(docs, pa.json_(pa.utf8()))})


@pytest.mark.parametrize("seed", range(3))
def test_both_ways(tmp_path, seed):
    lance = require_pylance()
    from lance.file import LanceFileReader

    table = _table(_docs(300, seed))
    ours, theirs = str(tmp_path / "n.lance"), str(tmp_path / "p.lance")
    nl.write_dataset(table, ours)
    lance.write_dataset(table, theirs)
    # The same bytes on disk ...
    raw = [LanceFileReader(glob.glob(os.path.join(p, "data", "*.lance"))[0]).read_all().to_table()["data"]
           for p in (ours, theirs)]
    assert raw[0].to_pylist() == raw[1].to_pylist()
    # ... the same text out, whichever library writes and whichever reads.
    expected = lance.dataset(theirs).to_table()
    for path in (ours, theirs):
        for lib in (nl, lance):
            assert lib.dataset(path).to_table().equals(expected)
    assert nl.dataset(ours).schema == lance.dataset(theirs).schema
    # Appends across libraries, and a compaction, keep them equal.
    nl.write_dataset(table, theirs, mode="append")
    lance.write_dataset(table, ours, mode="append")
    nl.dataset(ours).optimize.compact_files()
    assert lance.dataset(ours).to_table().equals(nl.dataset(theirs).to_table())


FILTERS = [
    "json_get_string(data, 'name') = 'Alice'",
    "json_get_int(data, 'age') > 30",
    "json_get_float(data, 'score') >= 50",
    "json_get_bool(data, 'active') = true",
    "json_get(data, 'name') IS NOT NULL",
    "json_get(data, 'nope') IS NULL",
    "json_get_string(json_get(json_get(data, 'user'), 'profile'), 'theme') = 'dark'",
    "json_extract(data, '$.user.profile.theme') = '\"dark\"'",
    "json_extract(data, '$.tags') = '[\"python\"]'",
    "json_exists(data, '$.user.x')",
    "json_exists(data, '$.extra.name')",
    "json_array_contains(data, '$.tags', 'rust')",
    "json_array_length(data, '$.tags') >= 2",
    "json_get_string(data, 'age') = '42'",
    "json_get_int(data, 'score') = 50",
]


@pytest.mark.parametrize("filter", FILTERS)
def test_json_filters_match_pylance(tmp_path, filter):
    lance = require_pylance()
    table = _table(_docs(400, 7))
    path = str(tmp_path / "f.lance")
    lance.write_dataset(table, path)
    expected = lance.dataset(path).to_table(filter=filter)
    assert nl.dataset(path).to_table(filter=filter).equals(expected), filter
    assert expected.num_rows > 0 or "nope" in filter


def test_json_errors(tmp_path):
    path = str(tmp_path / "e.lance")
    with pytest.raises(OSError, match="Failed to encode JSON"):
        nl.write_dataset(pa.table({"j": pa.array(['{"a": 1,}'], pa.json_())}), path)
    nl.write_dataset(pa.table({"j": pa.array(['{"a": [1]}'], pa.json_())}), path)
    with pytest.raises(ValueError, match="does not point to an array"):
        nl.dataset(path).to_table(filter="json_array_length(j, '$.a[0]') > 0")


@pytest.mark.parametrize("filter,expected", [
    ("regexp_match(s, 'c.+')", ["bca", "cab", "cba"]),
    ("regexp_like(s, '^A', 'i')", ["aaa", "abc"]),
    ("n::double > 2.5", ["cab", "cba"]),
    ("arrow_cast(n, 'Int64') = 3", ["cab"]),
    ("d <= current_date()", ["aaa", "bbb", "abc", "bca", "cab", "cba"]),
    ("array_has_any(t, ARRAY['x', 'z'])", ["aaa", "abc"]),
    ("b = arrow_cast(0x6262, 'Binary')", ["bbb"]),
])
def test_other_functions(tmp_path, filter, expected):
    lance = require_pylance()
    from datetime import date

    table = pa.table({
        "s": ["aaa", "bbb", "abc", "bca", "cab", "cba"],
        "n": [0, 1, 2, 2, 3, 4],
        "d": [date(2020, 1, 1)] * 6,
        "t": [["x"], ["y"], ["z"], [], ["y"], None],
        "b": [b"aa", b"bb", b"ab", b"bc", b"ca", b"cb"],
    })
    path = str(tmp_path / "o.lance")
    nl.write_dataset(table, path)
    assert nl.dataset(path).to_table(filter=filter)["s"].to_pylist() == expected
    assert lance.dataset(path).to_table(filter=filter)["s"].to_pylist() == expected


def test_pyarrow_arithmetic_expression(tmp_path):
    """pyarrow's add_checked & co. (what pc.field("n") + 1 becomes), as pylance takes them."""
    import pyarrow.compute as pc

    lance = require_pylance()
    path = str(tmp_path / "a.lance")
    nl.write_dataset(pa.table({"n": [0, 1, 2, 3, 4]}), path)
    for expr in (pc.field("n") + 1 == 4, pc.field("n") * 2 > 5, pc.field("n") - 1 < 1):
        assert nl.dataset(path).to_table(filter=expr).equals(lance.dataset(path).to_table(filter=expr))
