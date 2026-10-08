"""Phrase queries: INVERTED indexes with positions (``with_position=True``), against pylance.

pylance's indexes with positions are searched with pylance's answers -- the same rows and BM25
scores for ``PhraseQuery`` with any slop, over indexed rows, rows appended after the index (which
Lance matches by its own rule for unindexed rows) and deleted rows -- and indexes nanolance builds
with positions are searched by pylance as by its own index. ``optimize_indices`` keeps the
positions. Rows of equal score may come in another order (docs/ROADMAP.md), so rows are compared
with their scores.
"""

from __future__ import annotations

import random
import shutil

import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance import query as nq
from tests.support import require_pylance

VOCAB = "the a and of quick brown fox jumps over lazy dog dogs foxes running runs ran cat cats".split()


def _texts(n, seed):
    rng = random.Random(seed)
    out = []
    for i in range(n):
        if rng.random() < 0.03:
            out.append(None)
        else:
            out.append(" ".join(rng.choice(VOCAB) for _ in range(rng.randrange(0, 50))))
    out[min(5, n - 1)] = " ".join(["fox"] * 300)  # a long document: full position groups
    return out


def _table(n, start=0, seed=0):
    return pa.table({"id": pa.array(range(start, start + n), pa.int64()), "text": _texts(n, seed)})


def _queries(n, seed):
    rng = random.Random(seed)
    return [(" ".join(rng.choice(VOCAB) for _ in range(rng.randrange(1, 5))), rng.randrange(0, 4)) for _ in range(n)]


def _rows(module, path, words, slop, limit=None):
    q = (module.query.PhraseQuery if module is not nl else nq.PhraseQuery)(words, "text", slop=slop)
    t = module.dataset(path).to_table(full_text_query=q, columns=["id"], limit=limit)
    return dict(zip(t.column("id").to_pylist(), t.column("_score").to_pylist()))


def _same(a, b):
    assert set(a) == set(b)
    for k in a:
        assert a[k] == pytest.approx(b[k], rel=1e-5)


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    lance = require_pylance()
    root = tmp_path_factory.mktemp("phrase")
    paths = {}
    for who, module in (("pylance", lance), ("nanolance", nl)):
        path = str(root / f"{who}.lance")
        lance.write_dataset(_table(4000), path, max_rows_per_file=1500)
        module.dataset(path).create_scalar_index("text", "INVERTED", with_position=True)
        paths[who] = path
    return paths


def test_pylance_index_searched_as_pylance(built):
    lance = require_pylance()
    for words, slop in _queries(60, seed=1):
        _same(_rows(lance, built["pylance"], words, slop), _rows(nl, built["pylance"], words, slop))


def test_nanolance_index_searched_as_pylance_index(built):
    lance = require_pylance()
    for words, slop in _queries(60, seed=2):
        reference = _rows(lance, built["pylance"], words, slop)
        _same(reference, _rows(lance, built["nanolance"], words, slop))
        _same(reference, _rows(nl, built["nanolance"], words, slop))
    # Match queries are unchanged by positions.
    t = lance.dataset(built["nanolance"]).to_table(full_text_query="quick fox", columns=["id"])
    r = lance.dataset(built["pylance"]).to_table(full_text_query="quick fox", columns=["id"])
    assert set(t.column("id").to_pylist()) == set(r.column("id").to_pylist())


def test_unindexed_and_deleted_rows(built, tmp_path):
    lance = require_pylance()
    for who in ("pylance", "nanolance"):
        path = str(tmp_path / f"{who}.lance")
        shutil.copytree(built[who], path)
        lance.write_dataset(_table(400, start=4000, seed=9), path, mode="append")
        lance.dataset(path).delete("id % 89 = 3")
        for words, slop in _queries(40, seed=3):
            _same(_rows(lance, path, words, slop), _rows(nl, path, words, slop))
        # Limited: the best rows' scores (ties may be cut differently).
        for words, slop in _queries(10, seed=4):
            a = sorted(_rows(lance, path, words, slop, limit=10).values(), reverse=True)
            b = sorted(_rows(nl, path, words, slop, limit=10).values(), reverse=True)
            assert a == pytest.approx(b, rel=1e-5)


def test_optimize_keeps_positions(built, tmp_path):
    lance = require_pylance()
    path = str(tmp_path / "opt.lance")
    shutil.copytree(built["nanolance"], path)
    nl.write_dataset(_table(500, start=4000, seed=5), path, mode="append")
    nl.dataset(path).optimize.optimize_indices()
    stats = lance.dataset(path).stats.index_stats("text_idx")
    assert stats["num_unindexed_rows"] == 0
    for words, slop in _queries(20, seed=6):
        _same(_rows(lance, path, words, slop), _rows(nl, path, words, slop))


def test_phrase_inside_boolean(built):
    lance = require_pylance()
    from lance import query as lq
    for words, slop in _queries(10, seed=7):
        ql = lq.BooleanQuery([(lq.Occur.MUST, lq.PhraseQuery(words, "text", slop=slop)),
                              (lq.Occur.SHOULD, lq.MatchQuery("lazy", "text"))])
        qn = nq.BooleanQuery([(nq.Occur.MUST, nq.PhraseQuery(words, "text", slop=slop)),
                              (nq.Occur.SHOULD, nq.MatchQuery("lazy", "text"))])
        a = lance.dataset(built["pylance"]).to_table(full_text_query=ql, columns=["id"])
        b = nl.dataset(built["pylance"]).to_table(full_text_query=qn, columns=["id"])
        _same(dict(zip(a.column("id").to_pylist(), a.column("_score").to_pylist())),
              dict(zip(b.column("id").to_pylist(), b.column("_score").to_pylist())))


def test_phrase_needs_positions(tmp_path):
    path = str(tmp_path / "nopos.lance")
    nl.write_dataset(_table(200), path)
    nl.dataset(path).create_scalar_index("text", "INVERTED")
    with pytest.raises(Exception, match="position is not found"):
        nl.dataset(path).to_table(full_text_query=nq.PhraseQuery("quick fox", "text"))
