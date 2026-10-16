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


def _untagged(field):
    """The field without nanolance's own tags (nanolance:packing marks the integer columns it
    bit-packs; Lance ignores them), its list children too."""
    typ = field.type
    if isinstance(typ, pa.ListType):
        typ = pa.list_(_untagged(typ.value_field))
    meta = {k: v for k, v in (field.metadata or {}).items() if not k.startswith(b"nanolance:")}
    return pa.field(field.name, typ, field.nullable, meta or None)


def _index_content(path, column):
    from lance.file import LanceFileReader

    d = _index_dir(path, column)
    out = {}
    for f in sorted(os.listdir(d)):
        t = LanceFileReader(os.path.join(d, f)).read_all().to_table()
        meta = dict(t.schema.metadata or {})
        meta.pop(b"partitions", None)  # which partition number Lance's workers end up with varies
        key = f.split("_", 2)[-1] if f.startswith("part_") else f
        schema = pa.schema([_untagged(f) for f in t.schema])
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
    with pytest.raises(Exception, match="string column"):
        nl.dataset(path).create_scalar_index("id", "INVERTED")


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


def test_match_without_index(lance, tmp_path):
    """A match on a column without an index searches every row, as pylance does: the simple tokenizer
    alone, the documents' own statistics, rows in scan order (best first with a limit)."""
    path = str(tmp_path / "bare.lance")
    nl.write_dataset(_table(1200), path, max_rows_per_file=500)
    lance.dataset(path).delete("id % 9 = 2")
    from lance import query as lq

    for q in _queries(n=20, seed=23) + ["The", "the", "FILE files", "résumé École"]:
        assert _search(lance, path, lq.MatchQuery(q, "text")) == _search(nl, path, nq.MatchQuery(q, "text"))
        _same(_search(lance, path, lq.MatchQuery(q, "text"), limit=5),
              _search(nl, path, nq.MatchQuery(q, "text"), limit=5), limited=True)


def test_errors(built, tmp_path):
    path = built["nanolance"]
    ds = nl.dataset(path)
    with pytest.raises(Exception, match="position is not found"):
        ds.to_table(full_text_query=nq.PhraseQuery("file input", "text"))
    with pytest.raises(Exception, match="fuzzy"):
        ds.to_table(full_text_query=nq.MatchQuery("fil", "text", fuzziness=1))
    with pytest.raises(Exception, match="at least one should/must"):
        ds.to_table(full_text_query=nq.BooleanQuery([(nq.Occur.MUST_NOT, nq.MatchQuery("file", "text"))]))
    with pytest.raises(Exception, match="string column"):
        ds.to_table(full_text_query=nq.MatchQuery("file", "id"))
    bare = str(tmp_path / "bare.lance")
    nl.write_dataset(_table(50), bare)
    with pytest.raises(Exception, match="INVERTED index has been created"):
        nl.dataset(bare).to_table(full_text_query="file")
    assert json.loads(json.dumps(nq.MatchQuery("a", "text").inner.spec))["match"]["column"] == "text"
    assert glob.glob(os.path.join(path, "_indices", "*", "metadata.lance"))


# ── index format versions (format 1 and 3 at block 128) ─────────────────────────────────────────


def _phrases(texts, n=12):
    """Two-word phrases that occur in the texts (phrase queries need positions)."""
    rng = random.Random(11)
    usable = [t for t in texts if t and len(t.split(" ")) >= 2]
    out = []
    for text in rng.sample(usable, n):
        words = text.split(" ")
        at = rng.randrange(len(words) - 1)
        out.append(" ".join(words[at:at + 2]))
    return out


def _same_scores(ref, got):
    """The same rows, each score within 1e-5 (or 1e-6 relative: float32 sums of many terms, e.g. an
    ngram query's, differ in their last bits with the summation order)."""
    assert sorted(r for r, _ in ref) == sorted(r for r, _ in got)
    by_row = dict(got)
    for row, score in ref:
        assert by_row[row] == pytest.approx(score, abs=1e-5, rel=1e-6)


def _match(mod, text, operator):
    if mod is nl:
        return nq.MatchQuery(text, "text", operator=nq.FullTextOperator[operator])
    return mod.query.MatchQuery(text, "text", operator=mod.query.FullTextOperator[operator])


def _phrase(mod, text):
    if mod is nl:
        return nq.PhraseQuery(text, "text")
    return mod.query.PhraseQuery(text, "text")


@pytest.mark.parametrize(("format_version", "with_position"),
                         [(1, False), (1, True), (2, False), (2, True), (3, False), (3, True)])
def test_format_version_builds_match_pylance(lance, tmp_path, format_version, with_position):
    texts = _texts(800, 0)
    for who, mod in (("p", lance), ("n", nl)):
        # One fragment: pylance numbers the tokens in the order its fragment workers finish, so only a
        # single fragment's index is the same from one build to the next.
        path = str(tmp_path / f"{who}.lance")
        mod.write_dataset(_table(800), path)
        mod.dataset(path).create_scalar_index("text", "INVERTED", format_version=format_version,
                                              with_position=with_position)
    p, n = str(tmp_path / "p.lance"), str(tmp_path / "n.lance")
    for path in (p, n):
        assert lance.dataset(path).describe_indices()[0].segments[0].index_version == format_version
        assert nl.dataset(path).describe_indices()[0].segments[0].index_version == format_version
    _same_index(p, n, "text")
    specs = [("match", q, op) for q in _queries(n=10) for op in ("OR", "AND")]
    if with_position:
        specs += [("phrase", phrase, None) for phrase in _phrases(texts)]
    for kind, text, op in specs:
        query = (lambda mod: _match(mod, text, op)) if kind == "match" else (lambda mod: _phrase(mod, text))
        ref = _search(lance, p, query(lance))
        _same_scores(ref, _search(lance, n, query(lance)))  # pylance reads nanolance's build
        _same_scores(ref, _search(nl, p, query(nl)))  # nanolance reads pylance's build
        _same_scores(ref, _search(nl, n, query(nl)))


@pytest.mark.parametrize("format_version", [1, 3])
def test_phrase_queries_on_positions(lance, tmp_path, format_version):
    """Phrase queries answer the same from either engine's build, and only where the phrase is."""
    texts = _texts(800, 0)
    phrase = _phrases(texts, n=1)[0]
    for who, mod in (("p", lance), ("n", nl)):
        mod.write_dataset(_table(800), str(tmp_path / f"{who}.lance"), max_rows_per_file=300)
        mod.dataset(str(tmp_path / f"{who}.lance")).create_scalar_index(
            "text", "INVERTED", format_version=format_version, with_position=True)
    p, n = str(tmp_path / "p.lance"), str(tmp_path / "n.lance")
    ref = _search(lance, p, _phrase(lance, phrase))
    assert ref
    _same_scores(ref, _search(nl, p, _phrase(nl, phrase)))
    _same_scores(ref, _search(lance, n, _phrase(lance, phrase)))
    _same_scores(ref, _search(nl, n, _phrase(nl, phrase)))


def test_format_3_at_block_128_is_format_2_with_its_version(tmp_path):
    """V3 at block 128 writes V2's bytes, except the format version and the files that carry it."""
    for fv in (2, 3):
        nl.write_dataset(_table(800), str(tmp_path / f"v{fv}.lance"), max_rows_per_file=300).create_scalar_index(
            "text", "INVERTED", format_version=fv)
    two, three = str(tmp_path / "v2.lance"), str(tmp_path / "v3.lance")
    a, b = _index_content(two, "text"), _index_content(three, "text")
    assert a.keys() == b.keys()
    for key in ("docs.lance", "tokens.lance"):
        assert a[key] == b[key]
    for key in ("invert.lance", "metadata.lance"):
        assert a[key][:2] == b[key][:2]
        assert a[key][2].pop(b"format_version") == b"2"
        assert b[key][2].pop(b"format_version") == b"3"
        assert a[key][2] == b[key][2]


@pytest.mark.parametrize(("kwargs", "error", "message"), [
    ({"format_version": "v5"}, ValueError, "unsupported FTS format version v5, expected 1, 2, or 3"),
    ({"format_version": 4}, ValueError, "unsupported FTS format version 4, expected 1, 2, or 3"),
    ({"format_version": 0}, ValueError, "unsupported FTS format version 0, expected 1, 2, or 3"),
    ({"format_version": 2.0}, ValueError, "format_version must be 1, 2, 3, 'v1', 'v2', or 'v3'"),
    ({"block_size": 129}, ValueError, "block_size must be one of 128 or 256, got 129"),
    ({"block_size": 512}, ValueError, "block_size must be one of 128 or 256, got 512"),
    ({"format_version": 1, "block_size": 256}, ValueError,
     "FTS format_version=1 is incompatible with block_size=256; use format_version=3"),
    ({"format_version": 2, "block_size": 256}, ValueError,
     "FTS format_version=2 is incompatible with block_size=256; use format_version=3"),
])
def test_inverted_format_and_block_options_are_validated(tmp_path, kwargs, error, message):
    path = str(tmp_path / "a.lance")
    nl.write_dataset(_table(100), path)
    with pytest.raises(error, match=message):
        nl.dataset(path).create_scalar_index("text", "INVERTED", **kwargs)
    assert nl.dataset(path).describe_indices() == []


# ── block size 256 (format 3): BitPacker8x doc ids, patched-FOR frequencies, no score prefix ───────


def _block_table(n=1200, seed=5):
    """Documents with "alpha" in each (several full 256-document posting blocks and a tail), "beta"
    in two thirds, "gamma" in half, "delta" in a quarter, and "epsilon" repeated 150 times in three
    documents (frequencies above 100: patched-FOR exceptions) as a run, so "epsilon epsilon" is a
    phrase there."""
    rng = random.Random(seed)
    texts = []
    for i in range(n):
        words = [rng.choice(WORDS) for _ in range(rng.randint(3, 12))] + ["alpha"]
        rng.shuffle(words)
        words += ["beta"] if i % 3 else []
        words += ["gamma"] if i % 2 else []
        words += ["delta"] if i % 4 == 0 else []
        words += ["epsilon"] * 150 if i in (7, 300, 901) else []
        texts.append(" ".join(words))
    return pa.table({"id": pa.array(range(n), pa.int64()), "text": pa.array(texts, pa.string())}), texts


@pytest.mark.parametrize("with_position", [False, True])
def test_block_256_builds_match_pylance(lance, tmp_path, with_position):
    table, texts = _block_table()
    for who, mod in (("p", lance), ("n", nl)):
        # One fragment: pylance numbers the tokens in the order its fragment workers finish.
        mod.write_dataset(table, str(tmp_path / f"{who}.lance"))
        mod.dataset(str(tmp_path / f"{who}.lance")).create_scalar_index(
            "text", "INVERTED", block_size=256, with_position=with_position)
    p, n = str(tmp_path / "p.lance"), str(tmp_path / "n.lance")
    for path in (p, n):  # block 256 defaults to format 3
        assert lance.dataset(path).describe_indices()[0].segments[0].index_version == 3
        assert nl.dataset(path).describe_indices()[0].segments[0].index_version == 3
    assert _index_content(p, "text")["invert.lance"][2][b"posting_block_size"] == b"256"
    _same_index(p, n, "text")
    # The posting blobs (and impacts, positions) of both builds are equal, column by column.
    assert _index_content(p, "text")["invert.lance"][0] == _index_content(n, "text")["invert.lance"][0]
    specs = [("match", q, op) for q in ("alpha", "alpha beta", "gamma delta", "epsilon", "beta gamma delta")
             for op in ("OR", "AND")]
    specs += [("match", q, op) for q in _queries(n=10) for op in ("OR", "AND")]
    if with_position:
        specs += [("phrase", phrase, None) for phrase in _phrases(texts, n=10) + ["epsilon epsilon"]]
    for kind, text, op in specs:
        query = (lambda mod: _match(mod, text, op)) if kind == "match" else (lambda mod: _phrase(mod, text))
        ref = _search(lance, p, query(lance))
        _same_scores(ref, _search(lance, n, query(lance)))  # pylance reads nanolance's build
        _same_scores(ref, _search(nl, p, query(nl)))  # nanolance reads pylance's build
        _same_scores(ref, _search(nl, n, query(nl)))


def test_block_256_top_k_scores_match(lance, tmp_path):
    """The top-k (WAND) path: the bounds are read from the posting blocks, not a score prefix."""
    table, _ = _block_table()
    for who, mod in (("p", lance), ("n", nl)):
        mod.write_dataset(table, str(tmp_path / f"{who}.lance"))
        mod.dataset(str(tmp_path / f"{who}.lance")).create_scalar_index("text", "INVERTED", block_size=256)
    p, n = str(tmp_path / "p.lance"), str(tmp_path / "n.lance")
    for text in ("alpha", "alpha gamma", "epsilon delta", "beta gamma delta"):
        for limit in (5, 50):
            ref = [s for _, s in _search(lance, p, _match(lance, text, "OR"), limit=limit)]
            assert len(ref) == limit
            for path, mod in ((p, nl), (n, lance), (n, nl)):
                got = [s for _, s in _search(mod, path, _match(mod, text, "OR"), limit=limit)]
                assert got == pytest.approx(ref, abs=1e-5)



# ── the code analyzer ─────────────────────────────────────────────────────────────────────────────

CODE_PIECES = (
    "getUserName set_user_name user-name username parseHTML2JSON utf8_reader XMLHttpRequest HTTP2Server "
    "snake_case_id __init__ value.parse::<usize>() value.parse<usize>() a->b c->d x!=y x=>y std::vec::Vec "
    "Option<T> Vec::new() i+=1 ptr?.next naïveCAFÉ ÉtatCivil 0x1F count user name").split(" ")
CODE_QUERIES = [("user", "OR"), ("user name", "AND"), ("getUserName", "AND"), ("parse", "OR"), ("::", "OR"),
                ("->", "OR"), ("value.parse::<usize>()", "OR"), ("HTTP", "OR"), ("vec new", "AND"),
                ("snake_case_id __init__", "OR")]
CODE_OPTIONS = [
    pytest.param({}, id="defaults"),
    pytest.param({"split_identifiers": True}, id="split"),
    pytest.param({"split_identifiers": True, "split_on_numerics": False, "preserve_original": False,
                  "index_operators": True}, id="operators"),
]


def _code_table(n=400, seed=7):
    rng = random.Random(seed)
    texts = [" ".join(rng.choice(CODE_PIECES) for _ in range(rng.randint(2, 6))) for _ in range(n)]
    return pa.table({"id": pa.array(range(n), pa.int64()), "text": pa.array(texts, pa.string())}), texts


def _index_params(mod, path, column="text"):
    return mod.dataset(path).stats.index_stats(f"{column}_idx")["indices"][0]["params"]


@pytest.mark.parametrize("block_size", [128, 256])
@pytest.mark.parametrize("options", CODE_OPTIONS)
def test_code_analyzer_index_matches_pylance(lance, tmp_path, block_size, options):
    table, _ = _code_table()
    for who, mod in (("p", lance), ("n", nl)):  # one fragment: the same token numbering in both builds
        mod.write_dataset(table, str(tmp_path / f"{who}.lance"))
        mod.dataset(str(tmp_path / f"{who}.lance")).create_scalar_index(
            "text", "INVERTED", analyzer="code", block_size=block_size, **options)
    p, n = str(tmp_path / "p.lance"), str(tmp_path / "n.lance")
    for path in (p, n):
        assert lance.dataset(path).describe_indices()[0].segments[0].index_version == 3
        assert nl.dataset(path).describe_indices()[0].segments[0].index_version == 3
    assert _index_params(lance, p) == _index_params(nl, n)
    assert _index_params(nl, n)["base_tokenizer"] == "code"
    assert lance.dataset(p).describe_indices()[0].details == nl.dataset(n).describe_indices()[0].details
    for text, operator in CODE_QUERIES:
        ref = _search(lance, p, _match(lance, text, operator))
        _same_scores(ref, _search(lance, n, _match(lance, text, operator)))  # pylance reads nanolance's build
        _same_scores(ref, _search(nl, p, _match(nl, text, operator)))  # nanolance reads pylance's build
        _same_scores(ref, _search(nl, n, _match(nl, text, operator)))
    if options.get("index_operators"):
        assert _search(lance, p, _match(lance, "::", "OR")) and _search(lance, p, _match(lance, "->", "OR"))


@pytest.mark.parametrize(("kwargs", "message"), [
    ({"analyzer": "text", "base_tokenizer": "code"}, "base_tokenizer='code' requires analyzer='code'"),
    ({"analyzer": "code", "base_tokenizer": "simple"}, "analyzer='code' requires base_tokenizer='code', got 'simple'"),
    ({"analyzer": "text", "split_identifiers": True}, "code analyzer flags require analyzer='code'"),
    ({"analyzer": "bogus"}, "unknown analyzer 'bogus', expected 'text' or 'code'"),
    ({"analyzer": "code", "format_version": 2}, "base_tokenizer='code' requires FTS format_version=3, got 2"),
])
def test_code_analyzer_options_are_validated(tmp_path, kwargs, message):
    path = str(tmp_path / "t.lance")
    nl.write_dataset(pa.table({"text": ["getUserName"]}), path)
    with pytest.raises(ValueError) as excinfo:
        nl.dataset(path).create_scalar_index("text", "INVERTED", **kwargs)
    assert str(excinfo.value) == message


# ── the ngram base tokenizer (Lance's NgramTokenizer) ──────────────────────────────────────────

NGRAM_OPTIONS = [
    pytest.param({}, id="defaults"),
    pytest.param({"min_ngram_length": 2, "max_ngram_length": 4}, id="two-to-four"),
    pytest.param({"min_ngram_length": 1, "max_ngram_length": 3, "prefix_only": True}, id="prefix-only"),
]
NGRAM_WORDS = ["lance", "Database", "theatre", "Café", "crème", "naïve", "日本語", "🚀", "hello", "World",
               "ÅNGSTRÖM", "ab", "x9", "resume", "Ǆǅǆ"]


def _ngram_table(n=300, seed=11):
    rng = random.Random(seed)
    texts = [""]
    for _ in range(n - 1):
        words = [rng.choice(NGRAM_WORDS) for _ in range(rng.randint(1, 4))]
        texts.append(rng.choice([" ", ", ", "-", "_", "  "]).join(words))
    return pa.table({"id": pa.array(range(n), pa.int64()), "text": pa.array(texts, pa.string())}), texts


def _ngram_queries(texts, seed=5, n=40):
    """Queries over the ngram index, OR and AND. Every gram sits at position 0, and Lance unions the
    tokens sharing a query position, so AND over several grams answers as OR does."""
    rng = random.Random(seed)
    queries = [("lan", "OR"), ("the", "OR"), ("nce", "AND"), ("ab", "OR"), ("Café", "OR"), ("日本語", "AND"),
               ("🚀", "OR"), ("hello world", "OR"), ("hello", "AND"), ("ÅNG", "OR"), ("x", "OR"),
               ("helloworld", "AND"), ("hello world", "AND"), ("aaaa", "AND")]
    for _ in range(n):
        text = rng.choice(texts)
        if text:
            start = rng.randrange(len(text))
            queries.append((text[start:start + rng.randint(1, 6)], rng.choice(["OR", "AND"])))
    return queries


@pytest.mark.parametrize("block_size", [128, 256])
@pytest.mark.parametrize("options", NGRAM_OPTIONS)
def test_ngram_tokenizer_index_matches_pylance(lance, tmp_path, block_size, options):
    table, texts = _ngram_table()
    for who, mod in (("p", lance), ("n", nl)):  # one fragment: the same token numbering in both builds
        mod.write_dataset(table, str(tmp_path / f"{who}.lance"))
        mod.dataset(str(tmp_path / f"{who}.lance")).create_scalar_index(
            "text", "INVERTED", base_tokenizer="ngram", block_size=block_size, **options)
    p, n = str(tmp_path / "p.lance"), str(tmp_path / "n.lance")
    params = _index_params(nl, n)
    assert params["base_tokenizer"] == "ngram"
    assert params["stem"] is False and params["remove_stop_words"] is False
    assert params["block_size"] == block_size
    assert _index_params(lance, p) == params
    assert lance.dataset(p).describe_indices()[0].details == nl.dataset(n).describe_indices()[0].details
    hits = 0
    for text, operator in _ngram_queries(texts):
        ref = _search(lance, p, _match(lance, text, operator))
        hits += bool(ref)
        _same_scores(ref, _search(lance, n, _match(lance, text, operator)))  # pylance reads nanolance's build
        _same_scores(ref, _search(nl, p, _match(nl, text, operator)))  # nanolance reads pylance's build
        _same_scores(ref, _search(nl, n, _match(nl, text, operator)))
    assert hits > 20


@pytest.mark.parametrize(("kwargs", "message"), [
    ({"min_ngram_length": 0}, "min_gram must be greater than 0"),
    ({"min_ngram_length": 4, "max_ngram_length": 3}, "min_gram must not be greater than max_gram"),
])
def test_ngram_tokenizer_lengths_are_validated(tmp_path, kwargs, message):
    path = str(tmp_path / "t.lance")
    nl.write_dataset(pa.table({"text": ["hello"]}), path)
    with pytest.raises(ValueError) as excinfo:
        nl.dataset(path).create_scalar_index("text", "INVERTED", base_tokenizer="ngram", **kwargs)
    assert str(excinfo.value) == message
