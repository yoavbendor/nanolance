"""Full-text search over text in many scripts, with nanolance's own analyzer settings.

remove_marks: pointed and unpointed Hebrew, voweled and plain Arabic find each other. arabic_normalize: the
hamza forms of alef, teh marbuta and alef maqsura unify. unicode_normalize: a decomposed é is é, Romanian's
cedilla letters are the comma ones. ascii_folding_keep_original: a query without diacritics finds every
spelling, one with them ranks the exact spelling first, and phrases match either way. The settings survive
appending rows and optimizing the index. pylance refuses such an index with a clear error (it names the
tokenizer nanolance/simple) rather than searching it with other tokens; an index without them stays pylance's.
"""
from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance.query import PhraseQuery
from tests.support import require_pylance

DOCS = [
    "שלום עולם",            # 0
    "שָׁלוֹם עוֹלָם",          # 1
    "مَرْحَبًا بالعالم",       # 2
    "مرحبا بالعالم",        # 3
    "أحمد في المدرسة",      # 4
    "احمد في المدرسه",      # 5
    "fată frumoasă",        # 6
    "fata frumoasa",        # 7
    "fața casei",           # 8
    "ştiinţă veche",        # 9 (cedilla)
    "știință nouă",         # 10 (comma)
    "café noir",      # 11 (decomposed)
    "café crème",           # 12
    "hello world",          # 13
]

SETTINGS = dict(stem=False, remove_stop_words=False, with_position=True, unicode_normalize=True, remove_marks=True,
                arabic_normalize=True, ascii_folding_keep_original=True)


def _write(path, docs, start=0, mode="create"):
    nl.write_dataset(pa.table({"id": pa.array(range(start, start + len(docs)), pa.int64()), "t": docs}), path,
                     mode=mode)


@pytest.fixture()
def ds(tmp_path):
    path = str(tmp_path / "ms.lance")
    _write(path, DOCS)
    nl.dataset(path).create_scalar_index("t", "INVERTED", **SETTINGS)
    return nl.dataset(path)


def ranked(ds, query):
    t = ds.to_table(full_text_query=query, columns=["id"])
    return t.column("id").to_pylist()


@pytest.mark.parametrize("query, want", [
    ("שלום", {0, 1}), ("שָׁלוֹם", {0, 1}), ("עוֹלָם", {0, 1}),
    ("مرحبا", {2, 3}), ("مَرْحَبًا", {2, 3}),
    ("احمد", {4, 5}), ("أحمد", {4, 5}), ("المدرسة", {4, 5}),
    ("fata", {6, 7, 8}), ("stiinta", {9, 10}), ("știință", {9, 10}), ("ştiinţă", {9, 10}),
    ("café", {11, 12}), ("cafe", {11, 12}), ("hello", {13}),
])
def test_finds_every_spelling(ds, query, want):
    assert set(ranked(ds, query)) == want


@pytest.mark.parametrize("query, first", [("fată", 6), ("fața", 8), ("fata", 7)])
def test_exact_spelling_ranks_first(ds, query, first):
    assert ranked(ds, query)[0] == first


def test_phrases_match_either_spelling(ds):
    for phrase in ("fată frumoasă", "fata frumoasa"):
        got = ds.to_table(full_text_query=PhraseQuery(phrase, "t"), columns=["id"]).column("id").to_pylist()
        assert sorted(got) == [6, 7], phrase
    got = ds.to_table(full_text_query=PhraseQuery("שלום עולם", "t"), columns=["id"]).column("id").to_pylist()
    assert sorted(got) == [0, 1]


def test_settings_survive_append_and_optimize(ds):
    _write(ds.uri, ["שָׁלוֹם חָדָשׁ", "fată nouă"], start=100, mode="append")
    fresh = nl.dataset(ds.uri)
    fresh.optimize.optimize_indices()
    fresh = nl.dataset(ds.uri)
    assert set(ranked(fresh, "שלום")) == {0, 1, 100}
    assert ranked(fresh, "fată")[0] in (6, 101)
    assert set(ranked(fresh, "fata")) == {6, 7, 8, 101}


def test_pylance_refuses_an_index_with_the_settings(ds):
    lance = require_pylance()
    other = lance.dataset(ds.uri)
    assert other.count_rows() == len(DOCS)  # the dataset itself is pylance's to read
    with pytest.raises(Exception, match="nanolance/simple"):
        other.to_table(full_text_query="fata", columns=["id"])


def test_an_index_without_the_settings_stays_pylances(tmp_path):
    lance = require_pylance()
    path = str(tmp_path / "plain.lance")
    _write(path, DOCS)
    nl.dataset(path).create_scalar_index("t", "INVERTED", stem=False, remove_stop_words=False)
    for query in ("hello", "fata", "שלום"):
        want = sorted(lance.dataset(path).to_table(full_text_query=query, columns=["id"]).column("id").to_pylist())
        got = sorted(ranked(nl.dataset(path), query))
        assert got == want, query


def test_keep_original_requires_folding(tmp_path):
    path = str(tmp_path / "x.lance")
    _write(path, DOCS)
    with pytest.raises(Exception, match="requires ascii_folding"):
        nl.dataset(path).create_scalar_index("t", "INVERTED", ascii_folding=False, ascii_folding_keep_original=True)
