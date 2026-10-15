"""NGRAM scalar indexes: nanolance.lance against pylance on the same data and operations.

An NGRAM index is ngram_postings.lance: one row per distinct trigram token (token 0 holds the null rows),
its posting list a RoaringTreemap of row ids. Built by either engine it must read the same to the other:
the same token rows and the same posting bytes, the same answers to contains(), the same description and
statistics. Each engine searches the other's index; with and without the index the rows are the same.
"""

from __future__ import annotations

import glob
import os
import re
import shutil

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

pytestmark = pytest.mark.filterwarnings("ignore:The 'list_indices' method is deprecated")

VALUES = [
    "apple pie", "Banana!", None, "", "ÉCOLE ab", "ΟΔΟΣ kelvin K", "Straße ÆØ ß", "pineapple", "APPLE",
    "aébéc", "ab", "x y z", "123 abc", None, "Ångström 5",
]
NEEDLES = [
    "apple", "pple", "APP", "ban", "ana", "ecole", "École", "kelvin", "straße", "xyz", "ab", "", "pie",
    "ØÆ", "ß", "ole a", "ple", "ana!", "123", "Ångström", "ångström", "ö", "K",
]


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _table(values=VALUES):
    return pa.table({
        "w": pa.array(values, pa.string()),
        "id": pa.array(range(len(values)), pa.int64()),
    })


def _fresh(tmp_path, name):
    path = str(tmp_path / name)
    shutil.rmtree(path, ignore_errors=True)
    return path


def _posting_file(path):
    (file,) = glob.glob(os.path.join(path, "_indices", "*", "ngram_postings.lance"))
    return file


def _postings(path):
    """The token rows of the index on `path`: [(token, posting bytes)], read with pylance's file reader."""
    from lance.file import LanceFileReader

    table = LanceFileReader(_posting_file(path)).read_all().to_table()
    return table, list(zip(table.column("tokens").to_pylist(), table.column("posting_list").to_pylist()))


def _ids(table):
    return sorted(table.column("id").to_pylist())


def _build_both(lance, tmp_path, table, name="w_idx"):
    """The same table written twice, each with an NGRAM index on `w` built by one engine."""
    paths = {}
    for engine, module in (("pylance", lance), ("nanolance", nl)):
        path = _fresh(tmp_path, f"{engine}.lance")
        ds = module.write_dataset(table, path)
        ds.create_scalar_index("w", index_type="NGRAM", name=name)
        paths[engine] = path
    return paths


def test_ngram_tokens_and_postings_equal_pylance(lance, tmp_path):
    paths = _build_both(lance, tmp_path, _table())
    real, real_pairs = _postings(paths["pylance"])
    ours, our_pairs = _postings(paths["nanolance"])
    assert real.schema.field("posting_list").metadata == ours.schema.field("posting_list").metadata
    assert ours.schema.field("posting_list").metadata[b"lance-encoding:compression"] == b"none"
    assert ours.schema.field("tokens").nullable and not ours.schema.field("posting_list").nullable
    assert [t for t, _ in our_pairs] == [t for t, _ in real_pairs]
    assert our_pairs == real_pairs
    assert 0 in [t for t, _ in our_pairs]  # token 0: the null rows


def test_lower_case_and_fold_match_pylance_over_the_bmp(lance, tmp_path):
    # Every BMP character, once, as the middle of a trigram: a character that lower-cases or folds to ASCII
    # alphanumerics yields tokens, so the two engines' case and fold tables are compared character by character.
    chars = [chr(c) for c in range(0x80, 0x3000) if not 0xD800 <= c <= 0xDFFF]
    table = pa.table({"w": pa.array([f"ab{c}cd" for c in chars], pa.string()),
                      "id": pa.array(range(len(chars)), pa.int64())})
    paths = _build_both(lance, tmp_path, table)
    real, real_pairs = _postings(paths["pylance"])
    ours, our_pairs = _postings(paths["nanolance"])
    assert [t for t, _ in our_pairs] == [t for t, _ in real_pairs]
    assert our_pairs == real_pairs


def test_each_engine_searches_the_others_index(lance, tmp_path):
    paths = _build_both(lance, tmp_path, _table())
    scan_ds = _scan_copy(nl, tmp_path, "scan")
    for built, reader in (("pylance", nl), ("nanolance", lance)):
        for needle in NEEDLES:
            f = f"contains(w, '{needle}')"
            scan = _ids(scan_ds.to_table(filter=f, columns=["id"]))
            got = _ids(reader.dataset(paths[built]).to_table(filter=f, columns=["id"]))
            assert got == scan, (built, needle)
        plan = reader.dataset(paths[built]).scanner(filter="contains(w, 'apple')", columns=["id"]).explain_plan()
        assert 'ScalarIndexQuery: query=[contains(w, Utf8("apple"))]@w_idx(NGram)' in plan, built


def _scan_copy(module, tmp_path, name, table=None):
    """The same rows, never indexed: the scan every indexed answer must equal."""
    path = _fresh(tmp_path, f"{name}.lance")
    return module.write_dataset(_table() if table is None else table, path)


@pytest.mark.parametrize("engine", ["pylance", "nanolance"])
def test_same_rows_with_and_without_the_index(lance, tmp_path, engine):
    module = lance if engine == "pylance" else nl
    plain = module.write_dataset(_table(), _fresh(tmp_path, "plain.lance"))
    indexed_path = _fresh(tmp_path, "indexed.lance")
    module.write_dataset(_table(), indexed_path).create_scalar_index("w", index_type="NGRAM")
    indexed = module.dataset(indexed_path)
    for needle in NEEDLES:
        f = f"contains(w, '{needle}')"
        assert _ids(indexed.to_table(filter=f, columns=["id"])) == _ids(plain.to_table(filter=f, columns=["id"])), needle
        assert indexed.count_rows(filter=f) == plain.count_rows(filter=f), needle


def test_describe_and_stats_equal_pylance(lance, tmp_path):
    paths = _build_both(lance, tmp_path, _table())
    described = {}
    stats = {}
    for engine, module in (("pylance", lance), ("nanolance", nl)):
        ds = module.dataset(paths[engine])
        described[engine] = [(d.name, d.type_url, d.num_rows_indexed, list(d.fields), list(d.field_names))
                             for d in ds.describe_indices()]
        s = ds.stats.index_stats("w_idx")
        stats[engine] = {k: s[k] for k in ("index_type", "name", "num_indices", "num_segments", "indices",
                                           "segments", "num_indexed_rows", "num_unindexed_rows")}
    assert described["nanolance"] == described["pylance"]
    assert described["nanolance"][0][1] == "/lance.table.NGramIndexDetails"
    assert stats["nanolance"] == stats["pylance"]
    assert stats["nanolance"]["index_type"] == "NGram"
    assert stats["nanolance"]["indices"] == [{"num_ngrams": len(_postings(paths["pylance"])[1])}]


@pytest.mark.parametrize("engine", ["pylance", "nanolance"])
def test_large_string_column_uses_the_same_tokens(lance, tmp_path, engine):
    module = lance if engine == "pylance" else nl
    table = pa.table({"w": pa.array(VALUES, pa.large_string()), "id": pa.array(range(len(VALUES)), pa.int64())})
    path = _fresh(tmp_path, "large.lance")
    module.write_dataset(table, path).create_scalar_index("w", index_type="NGRAM")
    _, pairs = _postings(path)
    _, reference = _postings(_build_both(lance, tmp_path, _table())["pylance"])
    assert pairs == reference


@pytest.mark.parametrize("engine", ["pylance", "nanolance"])
def test_ngram_after_deletes_and_appends(lance, tmp_path, engine):
    module = lance if engine == "pylance" else nl
    path = _fresh(tmp_path, "mutated.lance")
    ds = module.write_dataset(_table(), path)
    ds.create_scalar_index("w", index_type="NGRAM")
    ds.delete("id IN (0, 4)")
    appended = pa.table({"w": pa.array(["new apple", None], pa.string()), "id": pa.array([100, 101], pa.int64())})
    ds = module.dataset(path)
    ds.insert(appended)
    reference = _scan_copy(module, tmp_path, "mutated_plain", pa.concat_tables([_table(), appended]))
    reference.delete("id IN (0, 4)")
    for needle in NEEDLES:
        f = f"contains(w, '{needle}')"
        assert _ids(ds.to_table(filter=f, columns=["id"])) == _ids(reference.to_table(filter=f, columns=["id"])), needle


@pytest.mark.parametrize("engine", ["pylance", "nanolance"])
def test_ngram_drop_and_empty_dataset(lance, tmp_path, engine):
    module = lance if engine == "pylance" else nl
    path = _fresh(tmp_path, "empty.lance")
    schema = pa.schema([pa.field("w", pa.string()), pa.field("id", pa.int64())])
    ds = module.write_dataset(pa.table({"w": pa.array([], pa.string()), "id": pa.array([], pa.int64())}, schema=schema), path)
    ds.create_scalar_index("w", index_type="NGRAM")
    assert ds.to_table(filter="contains(w, 'apple')").num_rows == 0
    ds.insert(pa.table({"w": pa.array(["apple"], pa.string()), "id": pa.array([1], pa.int64())}))
    assert _ids(module.dataset(path).to_table(filter="contains(w, 'apple')", columns=["id"])) == [1]
    ds = module.dataset(path)
    ds.drop_index("w_idx")
    assert ds.describe_indices() == []
    assert _ids(ds.to_table(filter="contains(w, 'apple')", columns=["id"])) == [1]


def test_drop_index_of_missing_name_raises_runtime_error_as_pylance(tmp_path):
    for engine in (nl, require_pylance()):
        ds = engine.write_dataset(pa.table({"a": [1, 2]}), str(tmp_path / engine.__name__))
        with pytest.raises(RuntimeError, match="index not found"):
            ds.drop_index("nonexistent_name")


# ── N1b: maintenance and distributed builds ──────────────────────────────────────────────────────────

T2 = ["apple tart", None, "x y z", "Straße ÆØ ß", "apple"]
T3 = ["ÉCOLE again", "banana split", ""]

# Each scenario: (op, argument). Rows are ids 0..14 (VALUES), then the inserted rows in order.
SCENARIOS = {
    "append_optimized": [("insert", T2), ("optimize", {})],
    "delta_then_merge": [("insert", T2), ("optimize", {"num_indices_to_merge": 0}), ("insert", T3),
                         ("optimize", {})],
    "delete_optimized": [("delete", "id = 2 OR id = 6"), ("optimize", {})],
    "append_delete_merge": [("insert", T2), ("optimize", {"num_indices_to_merge": 0}), ("delete", "id = 3"),
                            ("optimize", {"num_indices_to_merge": 2})],
    "merge_without_new_rows": [("insert", T2), ("optimize", {"num_indices_to_merge": 0}),
                               ("optimize", {"num_indices_to_merge": 2})],
}


def _run(module, path, steps, indexed=True, stable=False):
    """VALUES written to `path` (4 rows per fragment), an NGRAM index on `w` built by `module`, then the
    steps: inserts, deletes, optimize_indices. Returns the dataset."""
    shutil.rmtree(path, ignore_errors=True)
    module.write_dataset(_table(), path, max_rows_per_file=4, enable_stable_row_ids=stable or None)
    if indexed:
        module.dataset(path).create_scalar_index("w", index_type="NGRAM", name="w_idx")
    next_id = len(VALUES)
    for op, arg in steps:
        ds = module.dataset(path)
        if op == "insert":
            ds.insert(pa.table({"w": pa.array(arg, pa.string()),
                                "id": pa.array(range(next_id, next_id + len(arg)), pa.int64())}))
            next_id += len(arg)
        elif op == "delete":
            ds.delete(arg)
        else:
            ds.optimize.optimize_indices(**arg)
    return module.dataset(path)


def _segment_postings(path):
    """Each segment's token rows (token, posting bytes), sorted: the segments' order is their UUIDs'."""
    from lance.file import LanceFileReader

    segments = []
    for file in glob.glob(os.path.join(path, "_indices", "*", "ngram_postings.lance")):
        table = LanceFileReader(file).read_all().to_table()
        segments.append(sorted(zip(table.column("tokens").to_pylist(), table.column("posting_list").to_pylist())))
    return sorted(segments)


def _state(module, path):
    """What describe_indices and stats say, and the posting rows on disk, for the index on `w`."""
    ds = module.dataset(path)
    described = [(i.name, i.index_type, len(i.segments)) for i in ds.describe_indices()]
    stats = {k: v for k, v in ds.stats.index_stats("w_idx").items() if k != "updated_at_timestamp_ms"}
    return described, stats, _segment_postings(path)


@pytest.mark.parametrize("stable", [False, True], ids=["addresses", "stable_row_ids"])
@pytest.mark.parametrize("scenario", list(SCENARIOS))
def test_optimize_matches_pylance_and_scan(lance, tmp_path, scenario, stable):
    states, paths = {}, {}
    for engine, module in (("pylance", lance), ("nanolance", nl)):
        paths[engine] = _fresh(tmp_path, f"{scenario}_{engine}.lance")
        _run(module, paths[engine], SCENARIOS[scenario], stable=stable)
        states[engine] = _state(module, paths[engine])
    scan = _run(nl, _fresh(tmp_path, f"{scenario}_scan.lance"), SCENARIOS[scenario], indexed=False, stable=stable)
    if not stable:
        # Under stable row ids nanolance rebuilds the segment over the live rows (the row ids are not
        # addresses, so the old postings cannot be kept as Lance keeps them): only the answers are compared.
        assert states["nanolance"] == states["pylance"]
    for engine in paths:
        for module in (lance, nl):
            ds = module.dataset(paths[engine])
            for needle in NEEDLES:
                f = f"contains(w, '{needle}')"
                assert ds.count_rows(f) == scan.count_rows(f), (engine, needle)


def test_search_after_compaction_equals_scan(lance, tmp_path):
    """Compaction rewrites rows to new addresses: each engine's index still answers as a scan does. (The
    coverage it leaves differs from pylance's, which remaps the postings instead of re-covering: not compared.)"""
    for module, name in ((lance, "pylance"), (nl, "nanolance")):
        path = _fresh(tmp_path, f"compact_{name}.lance")
        ds = _run(module, path, [("insert", T2)])
        ds.optimize.compact_files(target_rows_per_fragment=100)
        scan = _run(nl, _fresh(tmp_path, f"compact_scan_{name}.lance"), [("insert", T2)], indexed=False)
        for reader in (lance, nl):
            indexed = reader.dataset(path)
            for needle in NEEDLES:
                f = f"contains(w, '{needle}')"
                assert indexed.count_rows(f) == scan.count_rows(f), (name, needle)


def test_distributed_segments_merge_and_commit_match_pylance(lance, tmp_path):
    states, paths = {}, {}
    for engine, module in (("pylance", lance), ("nanolance", nl)):
        path = _fresh(tmp_path, f"dist_{engine}.lance")
        ds = module.write_dataset(_table(), path, max_rows_per_file=4)
        segments = [
            ds.create_index_uncommitted(column="w", index_type="NGRAM", name="w_idx", fragment_ids=[f.fragment_id])
            for f in ds.get_fragments()
        ]
        ds.commit_existing_index_segments("w_idx", "w", [ds.merge_existing_index_segments(segments)])
        paths[engine] = path
        states[engine] = _state(module, path)
    assert states["nanolance"] == states["pylance"]
    scan = _run(nl, _fresh(tmp_path, "dist_scan.lance"), [], indexed=False)
    for engine in paths:
        for module in (lance, nl):
            ds = module.dataset(paths[engine])
            for needle in NEEDLES:
                f = f"contains(w, '{needle}')"
                assert ds.count_rows(f) == scan.count_rows(f), (engine, needle)


# ── JSON scalar indexes (IndexConfig, target btree): pages, describe / stats, reads, errors ──────────

JSON_CASES = {
    "int": (['{"x": 7}', '{"x": -2}', None, '{"y": 1}', '{"x": 7}'], "x"),
    "float_with_ints": (['{"x": 1.5}', '{"x": 2}', '{"x": -0.25}', None], "x"),
    "float_then_ints": (['{"x": 2.5}', '{"x": 1}', None, '{"x": 4}'], "x"),
    "string": (['{"x": "apple"}', '{"x": ""}', None, '{"x": "ÉCOLE ab"}', '{"x": "ΟΔΟΣ"}'], "x"),
    "bool": (['{"x": true}', '{"x": false}', None, '{"x": null}'], "x"),
    "all_null": ([None, '{"y": 1}', '{"x": null}'], "x"),
    "array": (['{"x": [1, "a", null]}', '{"x": []}', None], "x"),
    "object": (['{"x": {"b": [true, 2.5], "a": "é"}}', '{"x": {}}'], "x"),
    "big_unsigned": (['{"x": 18446744073709551615}', '{"x": 5}'], "x"),
    "missing_path": (['{"x": 1}', '{"z": 1}', '3', '"s"'], "q"),
    "dotted_path": (['{"a": {"b": 3}}', '{"a": {"b": 5}, "b": 9}', '{"a.b": 4}'], "a.b"),
    "nested_dotted": (['{"a": {"b": 3}}', '{"a": {"b": 5}}', '{"a": {"c": 1}}'], "a.b"),
    "dollar_root": (['{"x": 1}', '{"x": 2}'], "$"),
    "empty_path_scalar_root": (['3', '5', None], ""),
    "dollar_dot": (['{"x": 3}', '{"x": 4}'], "$.x"),
}


def _json_table(values):
    return pa.table({
        "j": pa.array(values, pa.json_()),
        "id": pa.array(range(len(values)), pa.int64()),
    })


def _json_config(module, path, target="btree"):
    if module == "nanolance":
        from nanolance.lance.indices import IndexConfig
    else:
        from lance.indices import IndexConfig
    return IndexConfig(index_type="json", parameters={"target_index_type": target, "path": path})


def _module(which, lance):
    return lance if which == "pylance" else nl


def _page_rows(path):
    """The BTree files of the JSON index on `path`, read with pylance's file reader: [(schema, rows)]."""
    from lance.file import LanceFileReader

    (directory,) = glob.glob(os.path.join(path, "_indices", "*"))
    out = {}
    for name in ("page_data.lance", "page_lookup.lance"):
        table = LanceFileReader(os.path.join(directory, name)).read_all().to_table()
        out[name] = ([(f.name, str(f.type)) for f in table.schema], table.to_pylist())
    return out


def _lance_text(error):
    """pylance's message without the Rust source location it appends (', /home/runner/...rs:164:17')."""
    return re.sub(r", /home/runner/\S+$", "", str(error))


def _build_json_pair(lance, tmp_path, values, path, name="j_idx"):
    """The same JSON table written twice, each engine building the index with the same IndexConfig."""
    paths = {}
    for engine in ("pylance", "nanolance"):
        out = _fresh(tmp_path, f"{engine}.lance")
        ds = _module(engine, lance).write_dataset(_json_table(values), out)
        ds.create_scalar_index("j", _json_config(engine, path), name=name)
        paths[engine] = out
    return paths


@pytest.mark.parametrize("case", list(JSON_CASES))
def test_json_index_pages_equal_pylance(lance, tmp_path, case):
    values, path = JSON_CASES[case]
    paths = _build_json_pair(lance, tmp_path, values, path)
    assert _page_rows(paths["nanolance"]) == _page_rows(paths["pylance"])
    assert len(_page_rows(paths["nanolance"])["page_data.lance"][1]) == len(values)


@pytest.mark.parametrize("case", list(JSON_CASES))
def test_json_index_describe_and_stats_equal_pylance(lance, tmp_path, case):
    values, path = JSON_CASES[case]
    paths = _build_json_pair(lance, tmp_path, values, path)
    seen = {}
    for engine in ("pylance", "nanolance"):
        ds = _module(engine, lance).dataset(paths[engine])
        (d,) = ds.describe_indices()
        seen[engine] = (d.name, d.type_url, d.index_type, d.details, d.fields, d.field_names, d.num_rows_indexed,
                        [s.index_version for s in d.segments], d.details == {"path": path, "target_details": {}})
        stats = ds.stats.index_stats("j_idx")
        seen[engine] += (stats["index_type"], stats["indices"], stats["segments"], stats["num_indexed_rows"])
    assert seen["nanolance"] == seen["pylance"]
    assert seen["nanolance"][1] == "/lance.index.pb.JsonIndexDetails"
    assert seen["nanolance"][2] == "Json"
    assert seen["nanolance"][8] is True
    assert seen["nanolance"][9] == "BTree"


# Filters whose function type is the index's (an Int64 index on `x`): pylance errors on a mismatched type
# even with its own index, so the reader test keeps to the matching ones (and one off-path lookup).
INT_FILTERS = ["json_get_int(j, 'x') = 7", "json_get_int(j, 'x') > 0", "json_get_int(j, 'x') IS NULL",
               "json_get_int(j, 'x') <= 7", "json_get_int(j, 'a.b') = 3"]


@pytest.mark.parametrize("writer", ["pylance", "nanolance"])
def test_each_engine_reads_the_others_json_index(lance, tmp_path, writer):
    values = JSON_CASES["int"][0] + ['{"x": 7, "a": {"b": 3}}', '{"x": 8}']
    paths = _build_json_pair(lance, tmp_path, values, "x")
    indexed = paths[writer]
    plain = _fresh(tmp_path, "plain.lance")
    lance.write_dataset(_json_table(values), plain)
    for reader in ("pylance", "nanolance"):
        module = _module(reader, lance)
        ds = module.dataset(indexed)
        assert [(d.name, d.index_type, d.details) for d in ds.describe_indices()] == [
            ("j_idx", "Json", {"path": "x", "target_details": {}})]
        assert ds.stats.index_stats("j_idx")["index_type"] == "BTree"
        for flt in INT_FILTERS:
            got = sorted(module.dataset(indexed).to_table(filter=flt)["id"].to_pylist())
            want = sorted(lance.dataset(plain).to_table(filter=flt)["id"].to_pylist())
            assert got == want, (reader, flt)


def test_json_index_after_deletes_and_appends_equals_pylance(lance, tmp_path):
    values = JSON_CASES["int"][0] * 3
    paths = _build_json_pair(lance, tmp_path, values, "x")
    counts = {}
    for engine in ("pylance", "nanolance"):
        module = _module(engine, lance)
        ds = module.dataset(paths[engine])
        ds.delete("id < 4")
        ds = module.dataset(paths[engine])
        ds.insert(_json_table(['{"x": 7}', '{"x": 1}']))
        ds = module.dataset(paths[engine])
        (d,) = ds.describe_indices()
        counts[engine] = (d.num_rows_indexed, ds.stats.index_stats("j_idx")["num_unindexed_rows"])
        assert sorted(ds.to_table(filter="json_get_int(j, 'x') = 7")["id"].to_pylist()) == sorted(
            lance.dataset(paths["pylance"]).to_table(filter="json_get_int(j, 'x') = 7")["id"].to_pylist())
    assert counts["nanolance"] == counts["pylance"]


def test_json_index_on_pylance_built_index_scans_equal(lance, tmp_path):
    values = JSON_CASES["string"][0] + ['{"y": 2}']
    paths = _build_json_pair(lance, tmp_path, values, "x")
    for flt in ["json_get_string(j, 'x') = 'apple'", "json_get_string(j, 'x') IS NULL"]:
        got = sorted(nl.dataset(paths["pylance"]).to_table(filter=flt)["id"].to_pylist())
        want = sorted(lance.dataset(paths["pylance"]).to_table(filter=flt)["id"].to_pylist())
        assert got == want


ERROR_CASES = [
    ("bitmap_target", {"target_index_type": "bitmap", "path": "x"}, None),
    ("label_list_target", {"target_index_type": "label_list", "path": "x"}, None),
    ("unknown_target", {"target_index_type": "foo", "path": "x"}, None),
    ("json_target", {"target_index_type": "json", "path": "x"}, None),
    ("unsupported_path", {"target_index_type": "btree", "path": "a[0]"}, None),
    ("path_with_brackets", {"target_index_type": "btree", "path": "$['x']"}, None),
    ("path_with_space", {"target_index_type": "btree", "path": "x y"}, None),
]


@pytest.mark.parametrize("name,params,_", ERROR_CASES, ids=[c[0] for c in ERROR_CASES])
def test_json_index_errors_as_pylance(lance, tmp_path, name, params, _):
    """Targets pylance rejects: the same exception type and text (minus Rust's location). Paths outside the
    supported subset are refused by nanolance by name (pylance may accept them as JSONPath)."""
    values = ['{"x": 1}']
    messages = {}
    for engine in ("pylance", "nanolance"):
        out = _fresh(tmp_path, f"{engine}.lance")
        ds = _module(engine, lance).write_dataset(_json_table(values), out)
        try:
            ds.create_scalar_index("j", _json_config(engine, params["path"], params["target_index_type"]), name="z")
            messages[engine] = "built"
        except Exception as e:  # noqa: BLE001 -- the type and text are the point of the comparison
            messages[engine] = (type(e).__name__, _lance_text(e))
    if params["target_index_type"] in ("bitmap",) or params["path"] in ("a[0]", "$['x']", "x y"):
        assert messages["nanolance"][0] == "NotSupportedError"
    else:
        assert messages["nanolance"] == messages["pylance"]


def test_json_index_error_texts(lance, tmp_path):
    out = _fresh(tmp_path, "texts.lance")
    ds = nl.write_dataset(_json_table(['{"x": 1}']), out)
    with pytest.raises(NotImplementedError, match='target_index_type \'bitmap\' is not supported yet \\(supported: btree\\)'):
        ds.create_scalar_index("j", _json_config("nanolance", "x", "bitmap"))
    with pytest.raises(ValueError, match="^Invalid user input: No scalar index plugin found for name 'label_list'$"):
        ds.create_scalar_index("j", _json_config("nanolance", "x", "label_list"))
    with pytest.raises(ValueError, match="needs the parameter path"):
        from nanolance.lance.indices import IndexConfig
        ds.create_scalar_index("j", IndexConfig(index_type="json", parameters={"target_index_type": "btree"}))
    with pytest.raises(NotImplementedError, match="Received JSON"):
        ds.create_scalar_index("j", "JSON")
    with pytest.raises(ValueError, match="^Invalid user input: A JSON index can only be created on a Binary"):
        ds2 = nl.write_dataset(pa.table({"s": ["a"], "id": [0]}), _fresh(tmp_path, "str.lance"))
        ds2.create_scalar_index("s", _json_config("nanolance", "x"))


def test_json_index_type_mismatch_as_pylance(lance, tmp_path):
    values = ['{"x": 7}', '{"x": 1.5}']
    paths = {}
    errors = {}
    for engine in ("pylance", "nanolance"):
        out = _fresh(tmp_path, f"{engine}.lance")
        ds = _module(engine, lance).write_dataset(_json_table(values), out)
        with pytest.raises(ValueError) as info:
            ds.create_scalar_index("j", _json_config(engine, "x"))
        errors[engine] = _lance_text(info.value)
        paths[engine] = out
    assert errors["nanolance"] == errors["pylance"]
    assert errors["nanolance"] == "Invalid user input: JSON path 'x' expected Int64, but batch row 1 has JSON type Float64"


def test_json_index_optimize_is_refused_until_it_is_supported(lance, tmp_path):
    values = JSON_CASES["int"][0]
    out = _fresh(tmp_path, "opt.lance")
    ds = nl.write_dataset(_json_table(values), out)
    ds.create_scalar_index("j", _json_config("nanolance", "x"), name="j_idx")
    ds = nl.dataset(out)
    with pytest.raises(NotImplementedError, match="JSON index.*not supported yet"):
        ds.optimize.optimize_indices()
    # the refusal leaves the index and its answers as they were
    assert sorted(ds.to_table(filter="json_get_int(j, 'x') = 7")["id"].to_pylist()) == [0, 4]
