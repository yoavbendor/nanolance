"""Scalar indices: BTREE, BITMAP and LABEL_LIST, built by nanolance and used by both.

The bar is Lance's own: an index nanolance builds is one pylance lists, validates and answers
filters from (its plan shows the index, its results equal a scan's), with the same contents as the
index pylance builds from the same data; and nanolance answers filters from an index whoever built
it, returning exactly what a scan returns.
"""

from __future__ import annotations

import glob
import shutil
import struct

import numpy as np
import pyarrow as pa
import pytest

import nanolance
import nanolance.lance as nl
from tests.support import require_pylance

pytestmark = pytest.mark.filterwarnings("ignore:The 'list_indices' method is deprecated")

SPECS = [("x", "BTREE", "x_bt"), ("s", "BITMAP", "s_bm"), ("tags", "LABEL_LIST", "tags_ll"), ("x", "BITMAP", "x_bm"),
         ("f", "BTREE", "f_bt"), ("d", "BTREE", "d_bt")]

FILTERS = [
    "x = 7", "x < 5", "x >= 45", "x IS NULL", "x IS NOT NULL AND x < 3", "x IN (1, 2, 49)", "x BETWEEN 10 AND 12",
    "x > 1000", "s = 'k3'", "s IS NULL", "s IN ('k1', 'k8')", "s <> 'k0'", "x = 7 OR s = 'k1'", "x = 7 AND s = 'k1'",
    "NOT (x = 7)", "f > 0.5", "f <= -1.5", "f = 0", "d >= DATE '2001-01-01'", "d < DATE '2000-03-01'",
    "array_has_any(tags, ['t1'])", "array_has_all(tags, ['t0', 't1'])", "array_contains(tags, 't3')",
    "tags IS NULL", "array_has_any(tags, ['t1', 'zz']) AND x < 10", "array_has_all(tags, [])",
    "x = 7 OR tags IS NULL",
]


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _table(n: int = 3000, start: int = 0) -> pa.Table:
    i = np.arange(start, start + n)
    rng = np.random.default_rng(start)
    f = rng.standard_normal(n)
    f[::97] = np.nan
    f[5::101] = -0.0
    return pa.table({
        "id": pa.array(i),
        "x": pa.array([None if k % 10 == 0 else int(k * 7919 % 50) for k in i], pa.int32()),
        "s": pa.array([None if k % 13 == 0 else f"k{k * 31 % 9}" for k in i], pa.string()),
        "tags": pa.array([None if k % 17 == 0 else ([] if k % 5 == 0 else
                          [f"t{(k + j) % 4}" for j in range(k % 3 + 1)] + ([None] if k % 23 == 0 else []))
                          for k in i], pa.list_(pa.string())),
        "f": pa.array(f),
        "d": pa.array((10_950 + (i * 37) % 800).astype("datetime64[D]")),
    })


def _dataset(lance, path, builder):
    lance.write_dataset(_table(), str(path), max_rows_per_file=1000)
    nl.dataset(str(path)).delete("id % 11 = 3")
    ds = nl.dataset(str(path)) if builder == "nanolance" else lance.dataset(str(path))
    for column, kind, name in SPECS:
        ds.create_scalar_index(column, kind, name=name)
    return str(path)


@pytest.fixture(scope="module")
def built(lance, tmp_path_factory):
    """The same data, indexed by each."""
    root = tmp_path_factory.mktemp("si")
    return {who: _dataset(lance, root / f"{who}.lance", who) for who in ("nanolance", "pylance")}


def _ids(table):
    return sorted(table.column("id").to_pylist())


def test_pylance_lists_and_validates_nanolance_indexes(lance, built):
    ds = lance.dataset(built["nanolance"])
    ds.validate()
    got = {i["name"]: (i["type"], i["fields"], sorted(i["fragment_ids"])) for i in ds.list_indices()}
    assert got == {"x_bt": ("BTree", ["x"], [0, 1, 2]), "s_bm": ("Bitmap", ["s"], [0, 1, 2]),
                   "tags_ll": ("LabelList", ["tags"], [0, 1, 2]), "x_bm": ("Bitmap", ["x"], [0, 1, 2]),
                   "f_bt": ("BTree", ["f"], [0, 1, 2]), "d_bt": ("BTree", ["d"], [0, 1, 2])}
    # nanolance lists either builder's indices as pylance does.
    for path in built.values():
        mine = {i["name"]: (i["type"], i["fields"], sorted(i["fragment_ids"])) for i in nl.dataset(path).list_indices()}
        assert mine == got


@pytest.mark.parametrize("flt", FILTERS)
def test_pylance_answers_from_nanolance_indexes(lance, built, flt):
    ds = lance.dataset(built["nanolance"])
    with_index = _ids(ds.to_table(filter=flt, columns=["id"]))
    scan = _ids(ds.to_table(filter=flt, columns=["id"], use_scalar_index=False))
    theirs = _ids(lance.dataset(built["pylance"]).to_table(filter=flt, columns=["id"]))
    assert with_index == scan == theirs


def test_pylance_plans_use_nanolance_indexes(lance, built):
    ds = lance.dataset(built["nanolance"])
    for flt, index in [("x < 5", "x_bt"), ("s = 'k3'", "s_bm"), ("array_has_any(tags, ['t1'])", "tags_ll")]:
        assert f"@{index}(" in ds.scanner(filter=flt).explain_plan(), flt


def _read(path):
    from lance.file import LanceFileReader

    reader = LanceFileReader(path)
    return reader.read_all().to_table(), reader.metadata()


def _rows_of(b):
    """A serialized RowAddrTreeMap: {fragment: its Roaring bitmap's bytes}."""
    count = struct.unpack_from("<I", b, 0)[0]
    at, out = 4, {}
    for _ in range(count):
        frag, size = struct.unpack_from("<II", b, at)
        at += 8
        out[frag] = bytes(b[at:at + size])
        at += size
    return out


def _roaring(b):
    """A Roaring bitmap's values (array and bitmap containers; run containers when present)."""
    cookie = struct.unpack_from("<I", b, 0)[0]
    if cookie & 0xFFFF == 12347:
        n = (cookie >> 16) + 1
        runs = b[4:4 + (n + 7) // 8]
        at = 4 + (n + 7) // 8
    else:
        n = struct.unpack_from("<I", b, 4)[0]
        runs = bytes((n + 7) // 8)
        at = 8
    keys = [struct.unpack_from("<HH", b, at + 4 * k) for k in range(n)]
    at += 4 * n
    if cookie & 0xFFFF == 12346 or n >= 4:
        at += 4 * n
    values = []
    for k, (key, card) in enumerate(keys):
        hi = key << 16
        if runs[k // 8] >> (k % 8) & 1:
            (nruns,) = struct.unpack_from("<H", b, at)
            at += 2
            for _ in range(nruns):
                start, length = struct.unpack_from("<HH", b, at)
                at += 4
                values += [hi + v for v in range(start, start + length + 1)]
        elif card + 1 > 4096:
            words = np.frombuffer(b[at:at + 8192], dtype="<u8")
            at += 8192
            bits = np.unpackbits(words.view(np.uint8), bitorder="little")
            values += [hi + int(v) for v in np.nonzero(bits)[0]]
        else:
            vals = struct.unpack_from(f"<{card + 1}H", b, at)
            at += 2 * (card + 1)
            values += [hi + v for v in vals]
    return values


def _bitmap_index(path):
    table, _ = _read(path)
    out = {}
    for key, bitmap in zip(table.column("keys").to_pylist(), table.column("bitmaps").to_pylist()):
        out[key] = {f: _roaring(b) for f, b in _rows_of(bitmap).items()}
    return out


def _index_dir(path, name):
    import lance as pylance

    (index,) = [i for i in pylance.dataset(path).list_indices() if i["name"] == name]
    return f"{path}/_indices/{index['uuid']}"


@pytest.mark.parametrize("name", ["s_bm", "x_bm", "tags_ll"])
def test_bitmap_contents_equal_pylances(lance, built, name):
    ours = _index_dir(built["nanolance"], name)
    theirs = _index_dir(built["pylance"], name)
    assert _bitmap_index(f"{ours}/bitmap_page_lookup.lance") == _bitmap_index(f"{theirs}/bitmap_page_lookup.lance")
    a, _ = _read(f"{ours}/bitmap_page_lookup.lance")
    b, _ = _read(f"{theirs}/bitmap_page_lookup.lance")
    assert a.schema.metadata == b.schema.metadata  # index stats; a label list's null-list buffer


@pytest.mark.parametrize("name", ["x_bt", "f_bt", "d_bt"])
def test_btree_contents_equal_pylances(lance, built, name):
    ours = _index_dir(built["nanolance"], name)
    theirs = _index_dir(built["pylance"], name)
    for file in ("page_lookup.lance", "page_data.lance"):
        a, _ = _read(f"{ours}/{file}")
        b, _ = _read(f"{theirs}/{file}")
        assert a.schema.equals(b.schema, check_metadata=True), file
        if file == "page_lookup.lance":
            assert repr(a.to_pylist()) == repr(b.to_pylist()), file  # repr: a page's NaN bound
        else:
            # The values in order; equal values' ids as sets (Lance's sort is not stable).
            assert repr(a.column("values").to_pylist()) == repr(b.column("values").to_pylist())
            key = lambda t: sorted(zip([str(v) for v in t.column("values").to_pylist()], t.column("ids").to_pylist()))
            assert key(a) == key(b)


@pytest.mark.parametrize("builder", ["nanolance", "pylance"])
def test_nanolance_answers_from_either_builders_indexes(lance, built, builder):
    path = built[builder]
    theirs = lance.dataset(path)
    for flt in FILTERS:
        stats, got = _stats_of(lambda: nl.dataset(path).to_table(filter=flt, columns=["id"]))
        scan = nl.dataset(path).to_table(filter=flt, columns=["id"], use_scalar_index=False)
        assert _ids(got) == _ids(scan) == _ids(theirs.to_table(filter=flt, columns=["id"])), flt
    stats, _ = _stats_of(lambda: nl.dataset(path).to_table(filter="x = 7 AND s = 'k1'"))
    assert stats["indexed_fragments"] == 3, stats
    stats, _ = _stats_of(lambda: nl.dataset(path).to_table(filter="x = 7", use_scalar_index=False))
    assert stats["indexed_fragments"] == 0, stats


def _stats_of(fn):
    nanolance._reset_work_stats()
    result = fn()
    return nanolance._work_stats(), result


def test_rows_appended_after_the_index_are_found(lance, built, tmp_path):
    path = str(tmp_path / "t.lance")
    shutil.copytree(built["nanolance"], path)
    nl.write_dataset(_table(500, 3000), path, mode="append")
    for flt in ["x = 7", "s = 'k3'", "array_has_any(tags, ['t1'])", "id >= 3100 AND x < 3"]:
        got = _ids(nl.dataset(path).to_table(filter=flt, columns=["id"]))
        assert got == _ids(lance.dataset(path).to_table(filter=flt, columns=["id"], use_scalar_index=False)), flt
        assert any(i >= 3000 for i in got) or flt.startswith("id"), flt
    stats, _ = _stats_of(lambda: nl.dataset(path).to_table(filter="x = 7"))
    assert stats["indexed_fragments"] == 3  # the new fragment is scanned


def test_names_replace_and_refusals(lance, tmp_path):
    path = str(tmp_path / "t.lance")
    nl.write_dataset(_table(200), path)
    ds = nl.dataset(path)
    ds.create_scalar_index("x", "BTREE")
    assert [i["name"] for i in ds.list_indices()] == ["x_idx"]  # Lance's default name
    ds.create_scalar_index("x", "BITMAP")  # replaces, as pylance's replace=True does
    assert [(i["name"], i["type"]) for i in nl.dataset(path).list_indices()] == [("x_idx", "Bitmap")]
    with pytest.raises(Exception, match="already exists"):
        nl.dataset(path).create_scalar_index("x", "BTREE", replace=False)
    with pytest.raises(Exception, match="LABEL_LIST index needs a list column"):
        nl.dataset(path).create_scalar_index("x", "LABEL_LIST", name="bad")
    with pytest.raises(Exception, match="takes a LABEL_LIST index"):
        nl.dataset(path).create_scalar_index("tags", "BTREE", name="bad")
    with pytest.raises(Exception, match="not found"):
        nl.dataset(path).create_scalar_index("nope", "BTREE")
    with pytest.raises(ValueError, match="unsupported index type"):
        nl.dataset(path).create_scalar_index("x", "IVF_PQ")
    lance.dataset(path).validate()
    nl.dataset(path).drop_index("x_idx")
    assert nl.dataset(path).list_indices() == [] and lance.dataset(path).list_indices() == []
    with pytest.raises(Exception, match="not found"):
        nl.dataset(path).drop_index("x_idx")


def test_describe_indices_and_explain_plan(built):
    ds = nl.dataset(built["pylance"])
    described = {d.name: d for d in ds.describe_indices()}
    assert described["x_bt"].index_type == "BTree" and described["tags_ll"].index_type == "LabelList"
    assert described["s_bm"].type_url == "/lance.table.BitmapIndexDetails"
    assert described["x_bt"].field_names == ["x"] and len(described["x_bt"].segments) == 1
    assert described["x_bt"].num_rows_indexed == ds.count_rows()
    plan = ds.scanner(filter="x BETWEEN 1 AND 3 AND s = 'k1'").explain_plan()
    assert "ScalarIndexQuery: query=[x >= 1 && x <= 3]@x_bt(BTree)" in plan, plan
    assert "ScalarIndexQuery: query=[s = 'k1']@s_bm(Bitmap)" in plan, plan
    assert "ScalarIndexQuery" not in ds.scanner(filter="x = 1", use_scalar_index=False).explain_plan()


def test_an_index_over_an_empty_dataset(lance, tmp_path):
    path = str(tmp_path / "t.lance")
    nl.write_dataset(_table(0), path)
    nl.dataset(path).create_scalar_index("x", "BTREE")
    nl.dataset(path).create_scalar_index("s", "BITMAP")
    nl.write_dataset(_table(50), path, mode="append")
    assert _ids(nl.dataset(path).to_table(filter="x = 7", columns=["id"])) == \
        _ids(lance.dataset(path).to_table(filter="x = 7", columns=["id"]))


# The vectorized filters (simple predicates over a column) against the row-at-a-time interpreter,
# which coalesce(column) forces: every comparison, null, NaN and signed zero agrees.
@pytest.mark.parametrize("flt", [
    "{c} = {v}", "{c} <> {v}", "{c} < {v}", "{c} <= {v}", "{c} > {v}", "{c} >= {v}", "{v} < {c}",
    "{c} IN ({v}, {w})", "{c} NOT IN ({v}, {w})", "{c} IN ({v}, NULL)", "{c} NOT IN ({v}, NULL)",
    "{c} BETWEEN {v} AND {w}", "{c} NOT BETWEEN {v} AND {w}", "{c} IS NULL", "{c} IS NOT NULL",
    "NOT ({c} = {v}) OR {c} IS NULL", "{c} = NULL",
])
def test_vectorized_filters_match_the_interpreter(tmp_path, flt):
    path = str(tmp_path / "t.lance")
    t = _table(400)
    t = t.append_column("u", pa.array(np.arange(400, dtype=np.uint16) % 37))
    t = t.append_column("ts", pa.array((np.arange(400) * 3600).astype("datetime64[s]"), pa.timestamp("s")))
    nl.write_dataset(t, path)
    ds = nl.dataset(path)
    cases = [("x", "7", "20"), ("x", "7.5", "-1"), ("u", "3", "30"), ("u", "-1", "5"), ("f", "0", "0.5"),
             ("f", "-0.0", "1"), ("s", "'k3'", "'k7'"), ("d", "DATE '2000-06-01'", "DATE '2001-01-01'"),
             ("ts", "TIMESTAMP '1970-01-02 00:00:00'", "TIMESTAMP '1970-01-05 12:00:00'"),
             ("ts", "DATE '1970-01-03'", "DATE '1970-01-09'")]
    for c, v, w in cases:
        fast = flt.format(c=c, v=v, w=w)
        slow = flt.format(c=f"coalesce({c})", v=v, w=w)
        assert _ids(ds.to_table(filter=fast, columns=["id"])) == _ids(ds.to_table(filter=slow, columns=["id"])), fast


# Floats as Lance's filters order them: -NaN < -inf < ... < -0 = +0 < ... < +inf < +NaN; and a
# literal compared with a float32 column is first rounded to float32 (x = 0.1 matches 0.1f, 1e39
# is +inf). Scan, BTREE and BITMAP each answer as pylance's scan does.
@pytest.mark.parametrize("kind", [None, "BTREE", "BITMAP"])
def test_float_order_and_float32_literals_match_pylance(lance, tmp_path, kind):
    neg_nan = struct.unpack("<d", struct.pack("<Q", 0xFFF8000000000000))[0]
    vals = [1.0, neg_nan, float("inf"), float("-inf"), 2.0, float("nan"), -0.0, 0.0, None, 0.1, 0.2,
            3.4028234663852886e38, -1e-40]
    path = str(tmp_path / "t.lance")
    nl.write_dataset(pa.table({"id": pa.array(range(len(vals))), "d": pa.array(vals, pa.float64()),
                               "f": pa.array(vals, pa.float32())}), path)
    if kind:
        for c in ("d", "f"):
            nl.dataset(path).create_scalar_index(c, kind)
    theirs = lance.dataset(path)
    for c in ("d", "f"):
        for flt in ["{c} > 0", "{c} < 5", "{c} <= 0", "{c} = 0", "{c} = -0.0", "{c} < 0", "{c} <> 0",
                    "{c} IN (0, 2)", "{c} < -1e308", "{c} > 1e308", "{c} BETWEEN -1 AND 1", "{c} = 0.1",
                    "{c} > 0.1", "{c} IN (0.1, 0.2)", "{c} BETWEEN 0.1 AND 0.2", "{c} >= 3.4028235e38",
                    "{c} < 1e39", "{c} = 0.1000000001", "{c} IS NULL", "{c} < 0 OR {c} > 2"]:
            flt = flt.format(c=c)
            want = _ids(theirs.to_table(filter=flt, columns=["id"], use_scalar_index=False))
            assert _ids(nl.dataset(path).to_table(filter=flt, columns=["id"])) == want, (kind, flt)
