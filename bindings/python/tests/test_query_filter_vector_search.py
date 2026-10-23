# SPDX-License-Identifier: Apache-2.0
"""A full-text query as the filter of a vector search (Lance's query filter), checked against pylance:
`filter=MatchQuery(...)` or `{"expr_filter": ..., "search_filter": ...}`, with and without prefilter."""

import numpy as np
import pyarrow as pa
import pytest

from tests.support import require_pylance

import nanolance.lance as nl


@pytest.fixture()
def datasets(tmp_path):
    lance = require_pylance()
    rng = np.random.default_rng(3)
    n = 600
    words = ["alpha", "beta", "gamma", "delta"]
    table = pa.table({
        "id": pa.array(range(n), pa.int64()),
        "vector": pa.FixedSizeListArray.from_arrays(pa.array(rng.standard_normal(n * 8).astype(np.float32)), 8),
        "text": [" ".join(words[j] for j in range(4) if (i >> j) & 1) or "none" for i in range(n)],
        "cat": [["a", "b", "c"][i % 3] for i in range(n)],
    })
    out = {}
    for tag, engine in (("py", lance), ("nl", nl)):
        path = str(tmp_path / f"{tag}.lance")
        engine.write_dataset(table, path)
        engine.dataset(path).create_scalar_index("text", "INVERTED", with_position=True)
        out[tag] = (engine, path)
    return out


@pytest.mark.parametrize("prefilter", [True, False])
@pytest.mark.parametrize("shape", ["match", "match_and_expr", "phrase", "phrase_and_expr"])
def test_query_filter_as_pylance(datasets, prefilter, shape):
    q = np.random.default_rng(9).standard_normal(8).astype(np.float32)
    got = {}
    for tag, (engine, path) in datasets.items():
        query = import_query(engine)
        match = query.MatchQuery("gamma", "text")
        phrase = query.PhraseQuery("alpha beta", "text")
        f = {"match": match, "phrase": phrase,
             "match_and_expr": {"expr_filter": "cat = 'b'", "search_filter": match},
             "phrase_and_expr": {"expr_filter": "cat = 'b'", "search_filter": phrase}}[shape]
        scanner = engine.dataset(path).scanner(nearest={"column": "vector", "q": q, "k": 10, "use_index": False},
                                               prefilter=prefilter, filter=f, columns=["id"])
        try:
            got[tag] = scanner.to_table()["id"].to_pylist()
        except Exception as e:  # noqa: BLE001 - compared below
            got[tag] = (type(e).__name__, str(e).split(",")[0])
    assert got["nl"] == got["py"]


def test_query_filter_without_a_search(datasets):
    for engine, path in datasets.values():
        query = import_query(engine)
        with pytest.raises(ValueError, match="Query filter can only be used"):
            engine.dataset(path).scanner(filter=query.MatchQuery("gamma", "text")).to_table()


def import_query(engine):
    from importlib import import_module

    return import_module(engine.__name__ + ".query")
