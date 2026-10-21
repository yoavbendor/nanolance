"""Full-text search over INVERTED indexes on struct leaves (``meta.title``, ``meta.body.text``, a list
under a struct, a leaf three structs deep).

Each engine builds the index for the same rows and both engines answer both datasets with pylance's rows
and scores (to 1e-6). Appends (unindexed fragments), deletes and ``optimize_indices`` are checked the same
way. The data has null structs, null leaves and null lists, as the nested column's documents have no text
where any struct above the leaf is null.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance import query as nq
from tests.support import require_pylance
from tests.test_fts import _search

pytestmark = pytest.mark.filterwarnings("ignore::DeprecationWarning")

INDEXES = ["meta.title", "meta.body.text", "meta.tags", "deep.a.b.text"]


def _table(start=0, extra=False):
    rows = [
        {"title": "red apple pie", "body": {"text": "sweet fruit"}, "tags": ["apple", "pie"]},
        {"title": "green apple", "body": {"text": "sour fruit"}, "tags": ["green"]},
        {"title": "blue sky", "body": {"text": "no fruit here"}, "tags": None},
        {"title": None, "body": {"text": "apple tree"}, "tags": ["apple pie"]},
        None,
        {"title": "apple apple", "body": None, "tags": []},
    ]
    deep = [
        {"a": {"b": {"text": "lance deep apple"}}},
        {"a": {"b": {"text": "plain"}}},
        {"a": None},
        {"a": {"b": None}},
        {"a": {"b": {"text": "deep fruit"}}},
        None,
    ]
    if extra:
        rows = [
            {"title": "fresh apple", "body": {"text": "fresh fruit"}, "tags": ["fresh"]},
            {"title": "plain append", "body": {"text": "apple"}, "tags": None},
        ]
        deep = [{"a": {"b": {"text": "appended apple"}}}, {"a": None}]
    ids = pa.array(range(start, start + len(rows)), pa.int64())
    return pa.table(
        {
            "id": ids,
            "meta": pa.array(rows),
            "deep": pa.array(deep),
        }
    )


@pytest.fixture
def lance():
    return require_pylance()


def _query(mod, kind):
    """Queries as each engine spells them: a dict, a default-column string, or a query object."""
    q = nq if mod is nl else mod.query
    return {
        "dict_title": {"columns": ["meta.title"], "query": "apple"},
        "match_title": q.MatchQuery("apple", "meta.title"),
        "dict_body": {"columns": ["meta.body.text"], "query": "fruit"},
        "phrase_title": q.PhraseQuery("apple pie", "meta.title"),
        "tags": {"columns": ["meta.tags"], "query": "apple"},
        "deep": {"columns": ["deep.a.b.text"], "query": "apple"},
        "default": "fruit",
    }[kind]


QUERIES = ["dict_title", "match_title", "dict_body", "phrase_title", "tags", "deep", "default"]


def _hits(mod, path, kind):
    return _search(mod, path, _query(mod, kind))


def _same(ref, got):
    """The same rows with the same scores (to 1e-6), in the same score order. Rows tied on a score may
    come in either order, as test_fts.py compares them."""
    assert [s for _, s in ref] == pytest.approx([s for _, s in got], abs=1e-6), (ref, got)
    ref_scores, got_scores = dict(ref), dict(got)
    assert ref_scores.keys() == got_scores.keys(), (ref, got)
    for row, score in ref_scores.items():
        assert got_scores[row] == pytest.approx(score, abs=1e-6), (row, ref, got)


def _build(mod, path, positions=True):
    ds = mod.dataset(path)
    for column in INDEXES:
        ds.create_scalar_index(column, "INVERTED", with_position=positions)
        ds = mod.dataset(path)


def _sides(lance, tmp_path):
    """One dataset per engine, each indexed by that engine (the same rows)."""
    paths = {"pylance": str(tmp_path / "pylance.lance"), "nanolance": str(tmp_path / "nanolance.lance")}
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.write_dataset(_table(), paths[who])
        _build(mod, paths[who])
    return paths


def _check_everywhere(lance, paths, queries=QUERIES):
    """Each engine answers each dataset; pylance's answer on its own dataset is the reference."""
    for kind in queries:
        ref = _hits(lance, paths["pylance"], kind)
        _same(ref, _hits(nl, paths["pylance"], kind))
        _same(ref, _hits(lance, paths["nanolance"], kind))
        _same(ref, _hits(nl, paths["nanolance"], kind))


def test_nested_indexes_answered_by_both_engines(lance, tmp_path):
    paths = _sides(lance, tmp_path)
    fields = {tuple(i["fields"]) for i in nl.dataset(paths["nanolance"]).list_indices()}
    assert fields == {(c,) for c in INDEXES}
    _check_everywhere(lance, paths)


def test_probe_cases_match_pylance(lance, tmp_path):
    """The probe's cases through both engines: ids and scores equal."""
    paths = {"pylance": str(tmp_path / "pylance.lance"), "nanolance": str(tmp_path / "nanolance.lance")}
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.write_dataset(_table(), paths[who])
        mod.dataset(paths[who]).create_scalar_index("meta.title", "INVERTED", with_position=False)
        mod.dataset(paths[who]).create_scalar_index("meta.body.text", "INVERTED", with_position=False)
    for kind in ("dict_title", "match_title", "default"):
        ref = _hits(lance, paths["pylance"], kind)
        _same(ref, _hits(nl, paths["pylance"], kind))
        _same(ref, _hits(nl, paths["nanolance"], kind))
    assert [r for r, _ in _hits(nl, paths["nanolance"], "dict_title")] == [5, 1, 0]


def test_append_delete_and_optimize(lance, tmp_path):
    paths = _sides(lance, tmp_path)
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.write_dataset(_table(start=6, extra=True), paths[who], mode="append")
    _check_everywhere(lance, paths)  # the appended rows are unindexed: searched flat

    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.dataset(paths[who]).delete("id = 1")
    _check_everywhere(lance, paths)

    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.dataset(paths[who]).optimize.optimize_indices()
    _check_everywhere(lance, paths)


def test_unindexed_nested_column_searched_flat(lance, tmp_path):
    paths = {"pylance": str(tmp_path / "pylance.lance"), "nanolance": str(tmp_path / "nanolance.lance")}
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.write_dataset(_table(), paths[who])
    for kind in ("dict_title", "dict_body", "deep"):
        ref = _hits(lance, paths["pylance"], kind)
        _same(ref, _hits(nl, paths["pylance"], kind))
        _same(ref, _hits(nl, paths["nanolance"], kind))


def _granularity(mod):
    return nq.DocumentGranularity.LIST_ELEMENT if mod is nl else mod.query.DocumentGranularity.LIST_ELEMENT


def _element_hits(mod, path, kind):
    q = nq if mod is nl else mod.query
    text, cls = ("apple", q.MatchQuery) if kind == "match" else ("apple pie", q.PhraseQuery)
    return _search(mod, path, cls(text, "meta.tags", document_granularity=_granularity(mod)))


def test_list_element_index_under_struct(lance, tmp_path):
    """A list under a struct, indexed by element: one document per element, as for a top-level list. A row
    can give several documents, so the rows are compared as multisets of (row, score)."""
    paths = {"pylance": str(tmp_path / "pylance.lance"), "nanolance": str(tmp_path / "nanolance.lance")}
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.write_dataset(_table(), paths[who])
        mod.dataset(paths[who]).create_scalar_index(
            "meta.tags", "INVERTED", with_position=True, document_granularity=_granularity(mod)
        )
    for kind in ("match", "phrase"):
        ref = _element_hits(lance, paths["pylance"], kind)
        assert ref, kind
        for mod, path in ((nl, paths["pylance"]), (lance, paths["nanolance"]), (nl, paths["nanolance"])):
            got = _element_hits(mod, path, kind)
            assert sorted(r for r, _ in got) == sorted(r for r, _ in ref), (kind, ref, got)
            assert sorted(s for _, s in got) == pytest.approx(sorted(s for _, s in ref), abs=1e-6), kind
