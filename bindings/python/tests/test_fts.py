"""Full-text search: ``full_text_query`` answered from Lance's INVERTED indexes, and INVERTED indexes
built by nanolance.

The bar is pylance's own answer for the same dataset and query: the same rows with the same
``_score`` (bit for bit) -- for an index pylance built and for one nanolance built, for the fragments
an index does not cover, with filters before or after the search, and deletions. Rows of equal score
may come in another order (Lance's order among them is arbitrary). An index nanolance builds holds
what pylance's holds for the same data: the same vocabulary, postings, block scores and impacts.
"""

from __future__ import annotations

import glob
import json
import os
import random
import shutil

import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance import query as nq
from tests.support import require_pylance

pytestmark = pytest.mark.filterwarnings("ignore::DeprecationWarning")

N = 3000

WORDS = (
    "file files filing input output stream lines line python string format strings formatted value values "
    "return returns returned def class object objects index indexes indexing search searching searched data "
    "dataset datasets table tables column columns row rows query queries fast faster fastest slow running "
    "runs ran runner database vector vectors token tokens tokenizer quick brown fox jumps over lazy dog "
    "résumé naïve café École straße Ωmega 東京 tower html2json getUserName foo_bar"
).split()
STOP = "the a an and of to in is it that for on with as by".split()


def _texts(n, seed):
    rng = random.Random(seed)
    out = []
    for i in range(n):
        if i % 97 == 3:
            out.append(None)
            continue
        if i % 89 == 7:
            out.append(rng.choice(["", "   ", "the and of", "!!!"]))
            continue
        k = rng.randint(1, 25)
        words = [rng.choice(WORDS[: rng.randint(3, len(WORDS))]) if rng.random() < 0.8 else rng.choice(STOP)
                 for _ in range(k)]
        if rng.random() < 0.1:
            words = [w.upper() for w in words]
        out.append(" ".join(words) + rng.choice(["", ".", "!", ", ok"]))
    return out


def _table(n=N, start=0, seed=0):
    return pa.table({"id": pa.array(range(start, start + n), pa.int64()),
                     "text": pa.array(_texts(n, seed), pa.string()),
                     "title": pa.array(_texts(n, seed + 1), pa.string())})


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


@pytest.fixture(scope="module")
def built(lance, tmp_path_factory):
    """The same dataset (two fragments) indexed on `text` and `title` by pylance and by nanolance."""
    root = tmp_path_factory.mktemp("fts")
    out = {}
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        path = str(root / f"{who}.lance")
        ds = mod.write_dataset(_table(), path, max_rows_per_file=1700)
        ds.create_scalar_index("text", "INVERTED")
        mod.dataset(path).create_scalar_index("title", "INVERTED")
        out[who] = path
    return out


def _index_dir(path, column):
    import lance as pylance

    ds = pylance.dataset(path)
    uuid = next(i["uuid"] for i in ds.list_indices() if i["fields"] == [column])
    return os.path.join(path, "_indices", uuid)


def _index_content(path, column):
    from lance.file import LanceFileReader

    d = _index_dir(path, column)
    out = {}
    for f in sorted(os.listdir(d)):
        t = LanceFileReader(os.path.join(d, f)).read_all().to_table()
        meta = dict(t.schema.metadata or {})
        meta.pop(b"partitions", None)  # which partition number Lance's workers end up with varies
        key = f.split("_", 2)[-1] if f.startswith("part_") else f
        # nanolance tags the integer columns it bit-packs (nanolance:packing); Lance ignores the tag.
        schema = pa.schema([f.remove_metadata() if f.metadata and all(k.startswith(b"nanolance:") for k in f.metadata)
                            else f for f in t.schema.remove_metadata()])
        out[key] = (t.replace_schema_metadata(None).to_pydict(), str(schema), meta)
    return out


def _same_index(path_a, path_b, column):
    """The two indexes hold the same: byte for byte when both list their documents in row order;
    otherwise (Lance's workers may take fragments in any order) the same documents, vocabulary and
    posting lists -- each word's search answers the same from both."""
    import lance as pylance

    a, b = _index_content(path_a, column), _index_content(path_b, column)
    assert a.keys() == b.keys()
    rows_a, rows_b = a["docs.lance"][0]["_rowid"], b["docs.lance"][0]["_rowid"]
    if rows_a == sorted(rows_a) and rows_b == sorted(rows_b):
        assert a == b
        return
    for f in a:
        assert a[f][1:] == b[f][1:], f  # schemas and metadata
    docs = lambda c: dict(zip(c["docs.lance"][0]["_rowid"], c["docs.lance"][0]["_num_tokens"]))
    assert docs(a) == docs(b)
    for key in ("_token_next_id", "_token_total_length"):
        assert a["tokens.lance"][0][key] == b["tokens.lance"][0][key]
    lists = lambda c: sorted(zip(c["invert.lance"][0]["_length"], c["invert.lance"][0]["_max_score"]))
    assert lists(a) == lists(b)
    for word in WORDS + STOP:
        q = pylance.query.MatchQuery(word, column)
        _same(_search(pylance, path_a, q), _search(pylance, path_b, q))


def _search(mod, path, query, **kw):
    t = mod.dataset(path).to_table(full_text_query=query, columns=["id", "_score"], with_row_id=True, **kw)
    return list(zip(t["_rowid"].to_pylist(), t["_score"].to_pylist()))


def _same(ref, got, limited=False):
    """Equal scores in the same (non-increasing) order; the same rows, except among rows tied at the
    cut of a limit."""
    assert [s for _, s in ref] == [s for _, s in got], (ref[:5], got[:5])
    if not limited:
        assert sorted(ref) == sorted(got)
        return
    last = ref[-1][1] if ref else None
    assert {r for r, s in ref if s != last} == {r for r, s in got if s != last}


def _queries(seed=3, n=60):
    rng = random.Random(seed)
    return [" ".join(rng.choice(WORDS + STOP) for _ in range(rng.randint(1, 4))) for _ in range(n)]


# ── building ─────────────────────────────────────────────────────────────────────────────────────


def test_index_matches_pylance_build(built):
    for column in ("text", "title"):
        _same_index(built["pylance"], built["nanolance"], column)


def test_index_listed_as_inverted(lance, built):
    for path in built.values():
        for mod in (lance, nl):
            kinds = {i["fields"][0]: i["type"] for i in mod.dataset(path).list_indices()}
            assert kinds == {"text": "Inverted", "title": "Inverted"}


@pytest.mark.parametrize("params", [
    {"stem": False}, {"remove_stop_words": False}, {"lower_case": False}, {"ascii_folding": False},
    {"max_token_length": None}, {"max_token_length": 6}, {"custom_stop_words": ["file", "python"]},
    {"base_tokenizer": "whitespace"}, {"base_tokenizer": "raw"},
])
def test_index_settings_match_pylance_build(lance, tmp_path, params):
    for who, mod in (("p", lance), ("n", nl)):
        mod.write_dataset(_table(800), str(tmp_path / f"{who}.lance")).create_scalar_index(
            "text", "INVERTED", **params)
    _same_index(str(tmp_path / "p.lance"), str(tmp_path / "n.lance"), "text")
    for q in _queries(n=15):
        _same(_search(lance, str(tmp_path / "p.lance"), q), _search(lance, str(tmp_path / "n.lance"), q))


def test_build_over_deletions_matches_pylance(lance, tmp_path):
    for who, mod in (("p", lance), ("n", nl)):
        path = str(tmp_path / f"{who}.lance")
        nl.write_dataset(_table(1500), path, max_rows_per_file=600)
        lance.dataset(path).delete("id % 5 = 1")
        mod.dataset(path).create_scalar_index("text", "INVERTED")
    _same_index(str(tmp_path / "p.lance"), str(tmp_path / "n.lance"), "text")


def test_create_index_fts_alias_and_replace(tmp_path):
    path = str(tmp_path / "a.lance")
    ds = nl.write_dataset(_table(300), path)
    ds.create_index("text", "FTS", name="t")
    with pytest.raises(Exception, match="already exists"):
        nl.dataset(path).create_scalar_index("text", "INVERTED", name="t", replace=False)
    nl.dataset(path).create_scalar_index("text", "INVERTED", name="t", stem=False)
    assert [i["name"] for i in nl.dataset(path).list_indices()] == ["t"]


def test_build_rejects_what_it_cannot_write(tmp_path):
    path = str(tmp_path / "a.lance")
    nl.write_dataset(_table(100), path)
    with pytest.raises(NotImplementedError):
        nl.dataset(path).create_scalar_index("text", "INVERTED", with_position=True)
    with pytest.raises(Exception, match="string column"):
        nl.dataset(path).create_scalar_index("id", "INVERTED")
    with pytest.raises(Exception):
        nl.dataset(path).create_scalar_index("text", "INVERTED", base_tokenizer="ngram")


# ── searching ────────────────────────────────────────────────────────────────────────────────────


@pytest.mark.parametrize("who", ["pylance", "nanolance"])
def test_plain_queries(lance, built, who):
    path = built[who]
    for q in _queries():
        ref = _search(lance, path, q)
        _same(ref, _search(nl, path, q))
        # And the other build answers the same.
        _same(ref, _search(lance, built["nanolance" if who == "pylance" else "pylance"], q))


def test_limit_offset_and_columns(lance, built):
    path = built["pylance"]
    for q in _queries(n=20):
        for kw in ({"limit": 1}, {"limit": 7}, {"limit": 10, "offset": 3}):
            _same(_search(lance, path, q, **kw), _search(nl, path, q, **kw), limited=True)
    for kw in ({}, {"columns": ["id", "_rowid"]}, {"columns": ["_score", "id"]},
               {"columns": ["id"], "with_row_id": True, "with_row_address": True}):
        a = lance.dataset(path).to_table(full_text_query="file", limit=3, **kw)
        b = nl.dataset(path).to_table(full_text_query="file", limit=3, **kw)
        assert a.schema.names == b.schema.names
        assert a.schema.field("_score").type == b.schema.field("_score").type == pa.float32()


def test_query_objects(lance, built):
    from lance import query as lq

    path = built["nanolance"]
    for q in _queries(n=25, seed=9):
        w = q.split()[0]
        pairs = [
            (lq.MatchQuery(q, "text"), nq.MatchQuery(q, "text")),
            (lq.MatchQuery(q, "title", operator=lq.FullTextOperator.AND),
             nq.MatchQuery(q, "title", operator=nq.FullTextOperator.AND)),
            (lq.MatchQuery(q, "text", boost=2.5), nq.MatchQuery(q, "text", boost=2.5)),
            (lq.MultiMatchQuery(q, ["text", "title"], boosts=[1.0, 2.0]),
             nq.MultiMatchQuery(q, ["text", "title"], boosts=[1.0, 2.0])),
            (lq.BoostQuery(lq.MatchQuery(q, "text"), lq.MatchQuery(w, "title"), negative_boost=0.3),
             nq.BoostQuery(nq.MatchQuery(q, "text"), nq.MatchQuery(w, "title"), negative_boost=0.3)),
            (lq.MatchQuery(q, "text") & lq.MatchQuery(w, "title"), nq.MatchQuery(q, "text") & nq.MatchQuery(w, "title")),
            (lq.MatchQuery(q, "text") | lq.MatchQuery(w, "title"), nq.MatchQuery(q, "text") | nq.MatchQuery(w, "title")),
            (lq.BooleanQuery([(lq.Occur.SHOULD, lq.MatchQuery(q, "text")), (lq.Occur.MUST_NOT, lq.MatchQuery(w, "text"))]),
             nq.BooleanQuery([(nq.Occur.SHOULD, nq.MatchQuery(q, "text")), (nq.Occur.MUST_NOT, nq.MatchQuery(w, "text"))])),
            ({"query": q, "columns": ["title"]}, {"query": q, "columns": ["title"]}),
        ]
        for pq, mq in pairs:
            _same(_search(lance, path, pq), _search(nl, path, mq))


def test_scanner_builder(built):
    path = built["nanolance"]
    ds = nl.dataset(path)
    a = ds.to_table(full_text_query={"query": "file input", "columns": ["text"]}, columns=["id"])
    builder = nl.ScannerBuilder(ds).full_text_search("file input", columns=["text"]).columns(["id"])
    c = builder.to_scanner().to_table()
    assert a.equals(c)
    assert ds.scanner(full_text_query="file", columns=["id"]).count_rows() == ds.to_table(
        full_text_query="file", columns=["id"]).num_rows
    assert "MatchQuery" in ds.scanner(full_text_query="file").explain_plan()


def test_filters(lance, built):
    path = built["pylance"]
    for q in _queries(n=20, seed=5):
        for kw, limited in (({"filter": "id % 3 = 0"}, False), ({"filter": "id % 3 = 0", "prefilter": True}, False),
                            ({"filter": "id < 1500", "prefilter": True, "limit": 8}, True)):
            _same(_search(lance, path, q, **kw), _search(nl, path, q, **kw), limited=limited)


def test_unindexed_fragments_and_deletions(lance, built, tmp_path):
    for who in ("pylance", "nanolance"):
        path = str(tmp_path / f"{who}.lance")
        shutil.copytree(built[who], path)
        nl.write_dataset(_table(600, start=N, seed=7), path, mode="append")
        lance.dataset(path).delete("id % 11 = 4")
        for q in _queries(n=25, seed=13):
            ref = _search(lance, path, q)
            _same(ref, _search(nl, path, q))
            _same(_search(lance, path, q, filter="id > 2000", prefilter=True),
                  _search(nl, path, q, filter="id > 2000", prefilter=True))
            _same(_search(lance, path, q, fast_search=True), _search(nl, path, q, fast_search=True))


def test_optimized_index_searched(lance, built, tmp_path):
    path = str(tmp_path / "o.lance")
    shutil.copytree(built["nanolance"], path)
    nl.write_dataset(_table(500, start=N, seed=8), path, mode="append")
    lance.dataset(path).optimize.optimize_indices()
    for q in _queries(n=20, seed=17):
        _same(_search(lance, path, q), _search(nl, path, q))


def test_errors(built, tmp_path):
    path = built["nanolance"]
    ds = nl.dataset(path)
    with pytest.raises(Exception, match="position is not found"):
        ds.to_table(full_text_query=nq.PhraseQuery("file input", "text"))
    with pytest.raises(Exception, match="fuzzy"):
        ds.to_table(full_text_query=nq.MatchQuery("fil", "text", fuzziness=1))
    with pytest.raises(Exception, match="at least one should/must"):
        ds.to_table(full_text_query=nq.BooleanQuery([(nq.Occur.MUST_NOT, nq.MatchQuery("file", "text"))]))
    with pytest.raises(Exception, match="INVERTED index"):
        ds.to_table(full_text_query=nq.MatchQuery("file", "id"))
    bare = str(tmp_path / "bare.lance")
    nl.write_dataset(_table(50), bare)
    with pytest.raises(Exception, match="INVERTED index has been created"):
        nl.dataset(bare).to_table(full_text_query="file")
    assert json.loads(json.dumps(nq.MatchQuery("a", "text").inner.spec))["match"]["column"] == "text"
    assert glob.glob(os.path.join(path, "_indices", "*", "metadata.lance"))
