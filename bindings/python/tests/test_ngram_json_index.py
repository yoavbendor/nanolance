"""NGRAM scalar indexes: nanolance.lance against pylance on the same data and operations.

An NGRAM index is ngram_postings.lance: one row per distinct trigram token (token 0 holds the null rows),
its posting list a RoaringTreemap of row ids. Built by either engine it must read the same to the other:
the same token rows and the same posting bytes, the same answers to contains(), the same description and
statistics. Each engine searches the other's index; with and without the index the rows are the same.
"""

from __future__ import annotations

import glob
import os
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
