"""Full-text search over list<string> columns at row granularity (Lance's default): the row is one
document, its non-null elements tokenized in order as one stream (positions continue across elements).

For each of the four list types, an INVERTED index built by nanolance holds what pylance's holds for the
same rows, each engine answers the other's index with pylance's rows and scores (bit for bit), and
unindexed or appended rows are searched the same way. The data has nulls, empty strings, null and empty
elements and empty lists.
"""

from __future__ import annotations

import random

import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance import query as nq
from tests.support import require_pylance
from tests.test_fts import _index_content, _same, _same_index, _search

pytestmark = pytest.mark.filterwarnings("ignore::DeprecationWarning")

LIST_TYPES = [
    pa.list_(pa.string()),
    pa.list_(pa.large_string()),
    pa.large_list(pa.string()),
    pa.large_list(pa.large_string()),
]
WORDS = "alpha beta gamma delta lance database search The the Lance résumé café file files filing".split()
# pylance's documentation example: row 0 has a null and an empty element, row 2 is null.
BRIEF = [["alpha beta", "gamma alpha", None, "", "delta"], ["beta", "gamma"], None]
QUERIES = [("match", w) for w in ("alpha", "beta", "lance", "the", "résumé", "file", "nosuchword")] + [
    ("phrase", p) for p in ("beta gamma", "alpha beta", "lance database", "file lance", "gamma alpha delta")
]


def _lists(n, seed):
    rng = random.Random(seed)
    out = []
    for i in range(n):
        if i % 13 == 5:
            out.append(None)
        elif i % 11 == 4:
            out.append([])
        else:
            elems = []
            for _ in range(rng.randint(1, 5)):
                r = rng.random()
                if r < 0.1:
                    elems.append(None)
                elif r < 0.2:
                    elems.append("")
                else:
                    elems.append(" ".join(rng.choice(WORDS) for _ in range(rng.randint(1, 4))))
            out.append(elems)
    return out


def _table(list_type, n=400, seed=0, start=0, brief=False):
    tags = (BRIEF if brief else []) + _lists(n, seed)
    ids = range(start, start + len(tags))
    return pa.table({"id": pa.array(ids, pa.int64()), "tags": pa.array(tags, type=list_type)})


@pytest.fixture
def lance():
    return require_pylance()


def _query(mod, kind, text):
    if mod is nl:
        return nq.MatchQuery(text, "tags") if kind == "match" else nq.PhraseQuery(text, "tags")
    return mod.query.MatchQuery(text, "tags") if kind == "match" else mod.query.PhraseQuery(text, "tags")


def _run(mod, path, kind, text):
    return _search(mod, path, _query(mod, kind, text))


def _sides(lance, tmp_path, list_type, index=True):
    """The same rows (the brief's three first), indexed (or not) by pylance and by nanolance."""
    paths = {"pylance": str(tmp_path / "pylance.lance"), "nanolance": str(tmp_path / "nanolance.lance")}
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.write_dataset(_table(list_type, brief=True), paths[who])
        if index:
            mod.dataset(paths[who]).create_scalar_index("tags", "INVERTED", with_position=True)
    return paths


def _same_everywhere(lance, paths, queries):
    """Each engine answers each dataset: pylance's answer is the reference."""
    for kind, text in queries:
        ref = _run(lance, paths["pylance"], kind, text)
        _same(ref, _run(nl, paths["pylance"], kind, text))
        _same(ref, _run(lance, paths["nanolance"], kind, text))
        _same(ref, _run(nl, paths["nanolance"], kind, text))


@pytest.mark.parametrize("list_type", LIST_TYPES)
def test_row_index_files_match_pylance(lance, tmp_path, list_type):
    paths = _sides(lance, tmp_path, list_type)
    _same_index(paths["pylance"], paths["nanolance"], "tags")
    # The row index has no _doc_index column, and no document for the null row 2 of the brief's rows.
    docs = _index_content(paths["nanolance"], "tags")["docs.lance"][0]
    assert sorted(docs) == ["_num_tokens", "_rowid"]
    assert 2 not in docs["_rowid"]


@pytest.mark.parametrize("list_type", LIST_TYPES)
def test_row_index_searched_by_each_engine(lance, tmp_path, list_type):
    paths = _sides(lance, tmp_path, list_type)
    _same_everywhere(lance, paths, QUERIES)


@pytest.mark.parametrize("list_type", LIST_TYPES)
def test_row_index_after_append_and_optimize(lance, tmp_path, list_type):
    paths = _sides(lance, tmp_path, list_type)
    extra = _table(list_type, n=150, seed=21, start=400)
    lance.write_dataset(extra, paths["pylance"], mode="append")
    nl.write_dataset(extra, paths["nanolance"], mode="append")
    # Appended rows are not indexed yet: they are searched flat, with the indexed rows.
    _same_everywhere(lance, paths, QUERIES)
    lance.dataset(paths["pylance"]).optimize.optimize_indices()
    lance.dataset(paths["nanolance"]).optimize.optimize_indices()
    _same_everywhere(lance, paths, QUERIES)


@pytest.mark.parametrize("list_type", LIST_TYPES)
def test_unindexed_list_column_searched_flat(lance, tmp_path, list_type):
    paths = _sides(lance, tmp_path, list_type, index=False)
    _same_everywhere(lance, paths, QUERIES)


def test_brief_rows_match_pylance_facts(lance, tmp_path):
    """The facts of the brief, for row 0 (positions continue across elements: "beta" ends element 0,
    "gamma" starts element 1) and row 1; the null row has no document."""
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        path = str(tmp_path / f"{who}.lance")
        mod.write_dataset(pa.table({"id": pa.array([0, 1, 2]), "tags": pa.array(BRIEF, pa.list_(pa.string()))}),
                          path)
        mod.dataset(path).create_scalar_index("tags", "INVERTED", with_position=True)
        assert _run(mod, path, "match", "alpha") == [(0, pytest.approx(0.850555, abs=1e-6))]
        phrase = _run(mod, path, "phrase", "beta gamma")
        assert [r for r, _ in phrase] == [1, 0]
        assert [s for _, s in phrase] == [pytest.approx(0.442166, abs=1e-6), pytest.approx(0.310249, abs=1e-6)]
        assert _run(mod, path, "match", "nosuchword") == []


# ── list-element granularity (L2) ───────────────────────────────────────────────────────────────


def _granularity(mod):
    return mod.query.DocumentGranularity.LIST_ELEMENT if mod is not nl else nq.DocumentGranularity.LIST_ELEMENT


def _element_query(mod, kind, text, granularity=True):
    """A match or phrase query over the list column, at list-element granularity (or unset)."""
    extra = {"document_granularity": _granularity(mod)} if granularity else {}
    if mod is nl:
        cls = nq.MatchQuery if kind == "match" else nq.PhraseQuery
    else:
        cls = mod.query.MatchQuery if kind == "match" else mod.query.PhraseQuery
    return cls(text, "tags", **extra)


def _row_query(mod, text):
    row = nq.DocumentGranularity.ROW if mod is nl else mod.query.DocumentGranularity.ROW
    cls = nq.MatchQuery if mod is nl else mod.query.MatchQuery
    return cls(text, "tags", document_granularity=row)


def _elements(mod, path, query):
    """The hits as (id, _doc_index, score), sorted: one per matching element."""
    t = mod.dataset(path).to_table(full_text_query=query)
    doc = t["_doc_index"].to_pylist()
    return sorted(zip(t["id"].to_pylist(), [tuple(d) for d in doc], t["_score"].to_pylist()))


def _same_elements(ref, got):
    assert ref == got, (ref[:6], got[:6])


def _same_element_everywhere(lance, paths, queries):
    for kind, text in queries:
        ref = _elements(lance, paths["pylance"], _element_query(lance, kind, text))
        _same_elements(ref, _elements(nl, paths["pylance"], _element_query(nl, kind, text)))
        _same_elements(ref, _elements(lance, paths["nanolance"], _element_query(lance, kind, text)))
        _same_elements(ref, _elements(nl, paths["nanolance"], _element_query(nl, kind, text)))


def _element_sides(lance, tmp_path, list_type, index=True, brief=True):
    """The brief's three rows (brief=True) or 400 generated rows with them, indexed by element."""
    paths = {"pylance": str(tmp_path / "pylance.lance"), "nanolance": str(tmp_path / "nanolance.lance")}
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.write_dataset(_table(list_type, n=0 if brief else 400, brief=brief), paths[who])
        if index:
            mod.dataset(paths[who]).create_scalar_index("tags", "INVERTED", with_position=True,
                                                        document_granularity=_granularity(mod))
    return paths


@pytest.mark.parametrize("list_type", LIST_TYPES)
def test_element_index_files_match_pylance(lance, tmp_path, list_type):
    paths = _element_sides(lance, tmp_path, list_type)
    _same_index(paths["pylance"], paths["nanolance"], "tags")
    for who in ("pylance", "nanolance"):
        names = {i.name for i in lance.dataset(paths[who]).describe_indices()} if who == "pylance" else \
            {i.name for i in nl.dataset(paths[who]).describe_indices()}
        assert "tags_list_element_idx" in names
    # One document per element (null and empty ones too), with its position in the list; no document
    # for a null row (row 2 of the brief's rows).
    docs = _index_content(paths["nanolance"], "tags")["docs.lance"][0]
    assert sorted(docs) == ["_doc_index_0", "_num_tokens", "_rowid"]
    assert 2 not in docs["_rowid"]
    assert sorted(zip(docs["_rowid"], docs["_doc_index_0"]))[:5] == [(0, 0), (0, 1), (0, 2), (0, 3), (0, 4)]


@pytest.mark.parametrize("list_type", LIST_TYPES)
def test_element_index_searched_by_each_engine(lance, tmp_path, list_type):
    paths = _element_sides(lance, tmp_path, list_type)
    _same_element_everywhere(lance, paths, QUERIES)


@pytest.mark.parametrize("list_type", LIST_TYPES)
def test_element_index_after_append_and_optimize(lance, tmp_path, list_type):
    paths = _element_sides(lance, tmp_path, list_type, brief=False)
    extra = _table(list_type, n=150, seed=21, start=400)
    lance.write_dataset(extra, paths["pylance"], mode="append")
    nl.write_dataset(extra, paths["nanolance"], mode="append")
    # Appended rows are not indexed yet: they are searched flat, beside the indexed elements.
    _same_element_everywhere(lance, paths, QUERIES)
    lance.dataset(paths["pylance"]).optimize.optimize_indices()
    lance.dataset(paths["nanolance"]).optimize.optimize_indices()
    _same_element_everywhere(lance, paths, QUERIES)


@pytest.mark.parametrize("list_type", LIST_TYPES)
def test_unindexed_list_elements_searched_flat(lance, tmp_path, list_type):
    paths = _element_sides(lance, tmp_path, list_type, index=False)
    _same_element_everywhere(lance, paths, QUERIES)


@pytest.mark.parametrize("list_type", LIST_TYPES)
def test_element_prefilter_and_limit(lance, tmp_path, list_type):
    paths = _element_sides(lance, tmp_path, list_type, brief=False)

    def hits(mod, path, **kwargs):
        t = mod.dataset(path).to_table(full_text_query=_element_query(mod, "match", "alpha"), **kwargs)
        return sorted(zip(t["id"].to_pylist(), [tuple(d) for d in t["_doc_index"].to_pylist()],
                          t["_score"].to_pylist()))

    for kwargs in ({"filter": "id < 50", "prefilter": True}, {"filter": "id < 50"}):
        ref = hits(lance, paths["pylance"], **kwargs)
        assert ref and all(i < 50 for i, _, _ in ref)
        for mod, path in ((nl, paths["pylance"]), (lance, paths["nanolance"]), (nl, paths["nanolance"])):
            _same_elements(ref, hits(mod, path, **kwargs))
    # A limit keeps the best scores (rows tied at the cut may differ).
    ref_scores = [score for _, _, score in hits(lance, paths["pylance"], limit=3)]
    assert len(ref_scores) == 3
    for mod, path in ((nl, paths["pylance"]), (lance, paths["nanolance"]), (nl, paths["nanolance"])):
        assert [score for _, _, score in hits(mod, path, limit=3)] == ref_scores


@pytest.mark.parametrize("list_type", LIST_TYPES)
def test_element_granularity_inferred_and_refused(lance, tmp_path, list_type):
    """The one index on the column decides an unset granularity; two indexes make it ambiguous; a
    granularity the index does not have is refused. Both engines say the same."""
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        path = str(tmp_path / f"{who}.lance")
        mod.write_dataset(_table(list_type, n=0, brief=True), path)
        ds = mod.dataset(path)
        ds.create_scalar_index("tags", "INVERTED", with_position=True, document_granularity=_granularity(mod))
        assert _elements(mod, path, _element_query(mod, "match", "alpha", granularity=False)) == [
            (0, (0,), pytest.approx(0.825462, abs=1e-6)), (0, (1,), pytest.approx(0.825462, abs=1e-6))]
        with pytest.raises(ValueError, match=r"requested Row.*ListElement"):
            mod.dataset(path).to_table(full_text_query=_row_query(mod, "alpha"))
        ds.create_scalar_index("tags", "INVERTED", with_position=True)
        assert {"tags_idx", "tags_list_element_idx"} <= {i.name for i in ds.describe_indices()}
        with pytest.raises(ValueError, match=r"ambiguous.*document_granularity"):
            mod.dataset(path).to_table(full_text_query=_element_query(mod, "match", "alpha", granularity=False))
        # A string query asks for row documents, as before.
        rows = mod.dataset(path).to_table(full_text_query="alpha")
        assert rows["id"].to_pylist() == [0] and "_doc_index" not in rows.column_names


def test_element_granularity_on_a_string_column_is_refused(lance, tmp_path):
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        path = str(tmp_path / f"{who}.lance")
        mod.write_dataset(pa.table({"id": pa.array([0, 1]), "text": ["alpha beta", "gamma"]}), path)
        with pytest.raises(ValueError, match=r"no List layer and cannot use ListElement"):
            mod.dataset(path).create_scalar_index("text", "INVERTED", document_granularity=_granularity(mod))


def test_element_granularity_on_a_nested_path_is_refused(lance, tmp_path):
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        path = str(tmp_path / f"{who}.lance")
        mod.write_dataset(_table(pa.list_(pa.string()), n=3), path)
        with pytest.raises(RuntimeError, match=r"tags\[\*\]"):
            mod.dataset(path).create_scalar_index("tags[*]", "INVERTED", document_granularity=_granularity(mod))


@pytest.mark.parametrize("list_type", LIST_TYPES)
def test_phrase_over_rows_no_index_covers(lance, tmp_path, list_type):
    """A phrase query over an unindexed list column, and over rows appended after the index: flat search
    tokenizes with positions, as pylance's does."""
    paths = _sides(lance, tmp_path, list_type, index=False)
    extra = _table(list_type, n=60, seed=5, start=400)
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.write_dataset(extra, paths[who], mode="append")
    phrases = [("phrase", p) for p in ("beta gamma", "alpha beta", "gamma alpha")]
    _same_everywhere(lance, paths, phrases)
    # Indexed rows and appended ones together.
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.dataset(paths[who]).create_scalar_index("tags", "INVERTED", with_position=True)
    more = _table(list_type, n=40, seed=9, start=500)
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        mod.write_dataset(more, paths[who], mode="append")
    _same_everywhere(lance, paths, phrases)


def test_phrase_over_an_unindexed_string_column(lance, tmp_path):
    rows = pa.table({"id": pa.array([0, 1, 2]), "text": ["alpha beta gamma", "beta alpha", "alpha beta"]})
    results = {}
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        path = str(tmp_path / f"{who}.lance")
        mod.write_dataset(rows, path)
        q = mod.query.PhraseQuery("alpha beta", "text") if mod is not nl else nq.PhraseQuery("alpha beta", "text")
        t = mod.dataset(path).to_table(full_text_query=q, columns=["id", "_score"])
        results[who] = sorted(zip(t["id"].to_pylist(), t["_score"].to_pylist()))
    assert results["nanolance"] == results["pylance"]
