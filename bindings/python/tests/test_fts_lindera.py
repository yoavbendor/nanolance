# SPDX-License-Identifier: Apache-2.0
"""The lindera base tokenizer (ROADMAP item 11, phase L): INVERTED indexes with base_tokenizer="lindera/<model>",
built and searched by both engines with the same results and scores. A model is a directory under
LANCE_LANGUAGE_MODEL_HOME holding config.yml; the tests write their own Lindera 3 dictionaries
(tests/lindera_dict.py) with random words, costs and unknown-word entries."""

import random
from importlib import import_module

import numpy as np
import pyarrow as pa
import pytest

from tests import lindera_dict as L
from tests.support import require_pylance

import nanolance.lance as nl

POOL = "東京国際空港成田羽田大学生研究所日本語処理形態素解析すもももももものうちアイウ"
PIECES = " 、。abcＡ１２3ァー\tαБ!"


def _dictionary(path, seed):
    rng = random.Random(seed)
    n_ids = rng.randint(3, 15)
    words = []
    for _ in range(rng.randint(5, 300)):
        w = "".join(rng.choice(POOL) for _ in range(rng.randint(1, 5)))
        words.append((w, rng.randint(-2000, 8000), rng.randrange(n_ids), rng.randrange(n_ids)))
    costs = [[rng.choice([0, 0, rng.randint(-500, 3000)]) for _ in range(n_ids)] for _ in range(n_ids)]
    unknown = [[(rng.choice([0, rng.randint(0, 9000)]), rng.randrange(n_ids), rng.randrange(n_ids))
                for _ in range(rng.randint(1, 3))] for _ in L.CATEGORIES]
    L.write_dictionary(path, words, n_ids, costs, unknown)
    return words, n_ids


def _model(home, name, config):
    d = home / "lindera" / name
    d.mkdir(parents=True, exist_ok=True)
    (d / "config.yml").write_text(config, encoding="utf-8")
    return d


def _corpus(n, seed):
    rng = random.Random(seed)
    return ["".join(rng.choice(POOL + PIECES) for _ in range(rng.randint(1, 40))) for _ in range(n)]


@pytest.fixture()
def home(tmp_path, monkeypatch):
    monkeypatch.setenv("LANCE_LANGUAGE_MODEL_HOME", str(tmp_path / "models"))
    monkeypatch.delenv("LINDERA_CONFIG_PATH", raising=False)
    return tmp_path / "models"


def _search(engine, path, query, phrase):
    if phrase:
        query = import_module(engine.__name__ + ".query").PhraseQuery(query, "text")
    t = engine.dataset(path).to_table(full_text_query=query, columns=["id"], limit=400)
    return sorted(zip(np.round(t["_score"].to_numpy(), 4).tolist(), t["id"].to_pylist()))


def _check_engines_agree(tmp_path, model, docs, queries, with_position=False):
    lance = require_pylance()
    table = pa.table({"id": pa.array(range(len(docs)), pa.int64()), "text": pa.array(docs)})
    paths = []
    for tag, engine in (("py", lance), ("nl", nl)):
        path = str(tmp_path / f"{tag}_{model.replace('/', '_')}.lance")
        engine.write_dataset(table, path, max_rows_per_file=150)
        engine.dataset(path).create_scalar_index("text", "INVERTED", base_tokenizer=model, with_position=with_position)
        paths.append(path)
    hits = 0
    for query in queries:
        for phrase in ([False, True] if with_position else [False]):
            got = [_search(engine, p, query, phrase) for engine in (lance, nl) for p in paths]
            assert all(g == got[0] for g in got), (query, phrase, [len(g) for g in got])
            hits += bool(got[0])
    assert hits >= len(queries) // 2, hits  # most queries find something: the comparison is not vacuous


@pytest.mark.parametrize("seed", [1, 2])
@pytest.mark.parametrize("mode", ["normal", "decompose"])
@pytest.mark.parametrize("with_position", [False, True])
def test_lindera_index_built_and_searched_by_both_engines(home, tmp_path, seed, mode, with_position):
    words, _ = _dictionary(tmp_path / f"dict{seed}", seed)
    _model(home, "gen", f'segmenter:\n  mode: "{mode}"\n  dictionary: "{tmp_path / f"dict{seed}"}"\n')
    docs = _corpus(300, seed)
    rng = random.Random(seed)
    queries = [w for w, *_ in words[:15]] + ["abc", "１２", "αБ"]
    queries += [d[rng.randint(0, max(0, len(d) - 3)):][:rng.randint(1, 5)] for d in docs[:30]]
    _check_engines_agree(tmp_path, "lindera/gen", docs, queries, with_position)


def test_lindera_user_dictionaries(home, tmp_path):
    words, n_ids = _dictionary(tmp_path / "dict", 5)
    # A CSV in each of Lindera's shapes (surface, reading, pronunciation with the metadata's default cost and
    # ids; or the full row), and the same words as a .bin.
    user = [("成田国際空港", -9000, 1, 2), ("国際空港", 100, 0, 0), ("すもももも", -5000, 2, 1), ("東京大学", 3000, 1, 1),
            ("国際空港", -200, 2, 2)]
    rows = [f"{s},{lft % n_ids},{rgt % n_ids},{c},名詞,一般,*,*,*,*,{s},ヨミ,ヨミ" for s, c, lft, rgt in user]
    (tmp_path / "user.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    simple = ["成田国際空港,カスタム名詞,ナリタコクサイクウコウ", '"すもももも",名詞,"ス,モ"', "", "東京大学,名詞,ヨミ"]
    (tmp_path / "simple.csv").write_text("\r\n".join(simple) + "\r\n", encoding="utf-8")
    (tmp_path / "user.bin").write_bytes(L.user_bin([(s, c, lft % n_ids, rgt % n_ids) for s, c, lft, rgt in user]))
    docs = _corpus(200, 9) + ["成田国際空港", "東京国際空港", "すもももももものうち", "東京大学生"]
    queries = ["成田", "成田国際空港", "国際", "空港", "すもも", "東京大学", "大学生"] + [w for w, *_ in words[:10]]
    for name in ("user.csv", "simple.csv", "user.bin"):
        model = name.replace(".", "_")
        _model(home, model, f'segmenter:\n  dictionary: "{tmp_path / "dict"}"\n  user_dictionary: "{tmp_path / name}"\n')
        _check_engines_agree(tmp_path, f"lindera/{model}", docs, queries)


def test_lindera_config_features(home, tmp_path):
    """Comments, quoting, flow mappings, a file:// dictionary, keep_whitespace and mapping filters."""
    _dictionary(tmp_path / "dict", 7)
    config = textwrap_config(tmp_path)
    _model(home, "features", config)
    docs = _corpus(200, 4) + ["成田 空港", "羽田\t空港"]
    _check_engines_agree(tmp_path, "lindera/features", docs, ["成田", "ほげ", "空港", " ", "羽田", "abc", "大学"])


def textwrap_config(tmp_path):
    uri = "file://" + str(tmp_path / "dict").replace(" ", "%20")
    return (
        "# a tokenizer config\n"
        "segmenter:\n"
        "  mode: normal   # the default\n"
        f"  dictionary: '{uri}'\n"
        "  keep_whitespace: true\n"
        "character_filters:\n"
        "  - kind: mapping\n"
        "    args:\n"
        "      mapping:\n"
        '        成田: "ほげほげ"\n'
        "        羽田空港: 羽田 空港\n"
        "  - kind: mapping\n"
        "    args: {mapping: {ほげほげ: ほげ, \"abc\": ABC}}\n"
        "token_filters: []\n"
    )


def test_lindera_errors_as_pylance(home, tmp_path, monkeypatch):
    lance = require_pylance()
    _dictionary(tmp_path / "dict", 3)
    cases = {
        "no_such_model": None,  # not a directory: ValueError
        "no_dictionary": "segmenter:\n  mode: normal\n",
        "missing_dictionary": f'segmenter:\n  dictionary: "{tmp_path / "nope"}"\n',
        "missing_user_dictionary": f'segmenter:\n  dictionary: "{tmp_path / "dict"}"\n  user_dictionary: "{tmp_path / "u.bin"}"\n',
        "bad_user_extension": f'segmenter:\n  dictionary: "{tmp_path / "dict"}"\n  user_dictionary: "{tmp_path / "u.txt"}"\n',
        "bad_mode": f'segmenter:\n  mode: fast\n  dictionary: "{tmp_path / "dict"}"\n',
        "not_a_mapping": "- a\n- b\n",
        "unknown_filter": f'segmenter:\n  dictionary: "{tmp_path / "dict"}"\ncharacter_filters:\n  - kind: nope\n',
    }
    for name, config in cases.items():
        if config is not None:
            _model(home, name, config)
        for tag, engine in (("py", lance), ("nl", nl)):
            path = str(tmp_path / f"{tag}_{name}.lance")
            engine.write_dataset(pa.table({"text": ["成田国際空港"]}), path)
            with pytest.raises(ValueError if config is None else OSError):
                engine.dataset(path).create_scalar_index("text", "INVERTED", base_tokenizer=f"lindera/{name}")
    # A model without config.yml falls back to LINDERA_CONFIG_PATH.
    (home / "lindera" / "fallback").mkdir(parents=True)
    env_config = tmp_path / "env.yml"
    env_config.write_text(f'segmenter:\n  dictionary: "{tmp_path / "dict"}"\n', encoding="utf-8")
    monkeypatch.setenv("LINDERA_CONFIG_PATH", str(env_config))
    _check_engines_agree(tmp_path, "lindera/fallback", ["成田国際空港", "羽田空港"], ["空港", "羽田"])


def test_lindera_token_filters_not_supported(home, tmp_path):
    _dictionary(tmp_path / "dict", 3)
    _model(home, "filters", f'segmenter:\n  dictionary: "{tmp_path / "dict"}"\ntoken_filters:\n  - kind: lowercase\n')
    path = str(tmp_path / "f.lance")
    nl.write_dataset(pa.table({"text": ["成田国際空港"]}), path)
    with pytest.raises(NotImplementedError, match="token filter 'lowercase'"):
        nl.dataset(path).create_scalar_index("text", "INVERTED", base_tokenizer="lindera/filters")
