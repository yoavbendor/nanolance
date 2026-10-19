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
    # Phrase queries need an index entry on the column, for string columns too (a gap of its own).
    paths = _sides(lance, tmp_path, list_type, index=False)
    _same_everywhere(lance, paths, [q for q in QUERIES if q[0] == "match"])


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
