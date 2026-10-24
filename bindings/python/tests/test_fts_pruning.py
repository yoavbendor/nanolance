# SPDX-License-Identifier: Apache-2.0
"""Multi-word full-text queries with a limit are answered with block-max MaxScore pruning (ROADMAP item 9).
The pruned search must return exactly what scoring every document returns: the same rows, the same scores
bit for bit. NANOLANCE_FTS_EXHAUSTIVE=1 (read once per process) turns the pruning off, so the exhaustive
answers come from a child process."""

import json
import os
import subprocess
import sys
import textwrap

import numpy as np
import pyarrow as pa
import pytest

from tests.support import require_pylance

import nanolance.lance as nl

QUERIES_SCRIPT = textwrap.dedent("""
    import json, sys
    import nanolance.lance as nl
    uri, cases = sys.argv[1], json.loads(sys.argv[2])
    ds = nl.dataset(uri)
    out = []
    for query, k, prefilter in cases:
        kw = {"filter": prefilter, "prefilter": True} if prefilter else {}
        t = ds.to_table(full_text_query=query, limit=k, columns=["id"], **kw)
        out.append([t["id"].to_pylist(), [float(s) for s in t["_score"].to_pylist()]])
    print(json.dumps(out))
""")


def _answers(uri: str, cases: list, exhaustive: bool) -> list:
    env = dict(os.environ)
    env.pop("NANOLANCE_FTS_EXHAUSTIVE", None)
    if exhaustive:
        env["NANOLANCE_FTS_EXHAUSTIVE"] = "1"
    proc = subprocess.run([sys.executable, "-c", QUERIES_SCRIPT, uri, json.dumps(cases)], env=env,
                          capture_output=True, text=True, timeout=600)
    assert proc.returncode == 0, proc.stderr
    return json.loads(proc.stdout.strip().splitlines()[-1])


def _corpus(rng, n: int, vocab: int = 3000) -> list:
    """Zipf-distributed words, so a few are in most documents and most are rare (what pruning skips)."""
    p = 1.0 / np.arange(1, vocab + 1) ** 1.05
    p /= p.sum()
    lengths = rng.integers(3, 60, n)
    words = rng.choice(vocab, size=int(lengths.sum()), p=p)
    docs, at = [], 0
    for length in lengths:
        docs.append(" ".join(f"w{w}" for w in words[at:at + length]))
        at += length
    return docs


def _cases(rng, vocab: int = 3000, count: int = 120) -> list:
    cases = []
    for _ in range(count):
        n = int(rng.integers(2, 9))
        # Mostly common words (where the bounds matter), some rare, a repeated word now and then.
        words = [f"w{int(x) % vocab}" for x in rng.zipf(1.4, n)]
        if rng.random() < 0.2:
            words.append(words[0])
        k = int(rng.choice([1, 3, 10, 40, 500]))
        prefilter = "id % 3 <> 0" if rng.random() < 0.25 else None
        cases.append([" ".join(words), k, prefilter])
    return cases


@pytest.fixture(scope="module")
def corpus_dataset(tmp_path_factory):
    """Several fragments and index partitions, deleted rows, and an unindexed appended fragment."""
    rng = np.random.default_rng(7)
    uri = str(tmp_path_factory.mktemp("prune") / "docs.lance")
    docs = _corpus(rng, 30_000)
    table = pa.table({"id": pa.array(range(len(docs)), pa.int64()), "text": pa.array(docs)})
    nl.write_dataset(table, uri, max_rows_per_file=7_000)
    ds = nl.dataset(uri)
    ds.create_scalar_index("text", "INVERTED")
    ds.delete("id % 11 = 0")
    more = _corpus(rng, 2_000)
    nl.write_dataset(pa.table({"id": pa.array(range(30_000, 32_000), pa.int64()), "text": pa.array(more)}),
                     uri, mode="append")
    return uri


def test_pruned_or_queries_equal_the_exhaustive_search(corpus_dataset):
    cases = _cases(np.random.default_rng(11))
    pruned, exhaustive = _answers(corpus_dataset, cases, False), _answers(corpus_dataset, cases, True)
    for case, a, b in zip(cases, pruned, exhaustive):
        assert a == b, case


def test_pruned_or_queries_on_list_element_documents():
    rng = np.random.default_rng(5)
    import tempfile

    uri = os.path.join(tempfile.mkdtemp(), "lists.lance")
    docs = _corpus(rng, 9_000, vocab=800)
    lists = pa.array([docs[i:i + 3] for i in range(0, len(docs), 3)], pa.list_(pa.string()))
    nl.write_dataset(pa.table({"id": pa.array(range(len(lists)), pa.int64()), "text": lists}), uri)
    nl.dataset(uri).create_scalar_index("text", "INVERTED")
    cases = [c for c in _cases(np.random.default_rng(3), vocab=800, count=60)]
    assert _answers(uri, cases, False) == _answers(uri, cases, True)


def test_pruned_or_queries_match_pylance(tmp_path):
    lance = require_pylance()
    rng = np.random.default_rng(13)
    docs = _corpus(rng, 12_000)
    table = pa.table({"id": pa.array(range(len(docs)), pa.int64()), "text": pa.array(docs)})
    ours, theirs = str(tmp_path / "nl.lance"), str(tmp_path / "py.lance")
    nl.write_dataset(table, ours)
    nl.dataset(ours).create_scalar_index("text", "INVERTED")
    lance.write_dataset(table, theirs)
    lance.dataset(theirs).create_scalar_index("text", "INVERTED")
    for query, k, _ in _cases(np.random.default_rng(17), count=40):
        a = nl.dataset(ours).to_table(full_text_query=query, limit=k, columns=["id"])
        b = lance.dataset(theirs).to_table(full_text_query=query, limit=k, columns=["id"])
        # Scores in order equal; rows equal where no tie at the cut can pick a different one.
        assert np.allclose(a["_score"].to_numpy(), b["_score"].to_numpy(), rtol=1e-5), query
        cut = b["_score"].to_pylist()[-1] if len(b) else None
        assert {i for i, s in zip(a["id"].to_pylist(), a["_score"].to_pylist()) if s != cut} == \
            {i for i, s in zip(b["id"].to_pylist(), b["_score"].to_pylist()) if s != cut}, query
