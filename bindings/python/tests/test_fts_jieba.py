# SPDX-License-Identifier: Apache-2.0
"""The jieba base tokenizer (ROADMAP item 11, phase J): INVERTED indexes with base_tokenizer="jieba/<model>",
built and searched by both engines with the same results and scores. A model is a directory under
LANCE_LANGUAGE_MODEL_HOME; the test writes its own dictionaries (a small one, so the HMM decides most words)."""

import json
import os
import random
import subprocess
import sys
import textwrap

import numpy as np
import pyarrow as pa
import pytest

from tests.support import require_pylance

import nanolance.lance as nl

WORDS = ["我们", "光明", "前途", "中国", "大学", "大学生", "学生", "北京", "人民", "共和国", "中华人民共和国",
         "研究", "研究生", "生命", "起源", "今天", "天气", "不错", "数据", "数据库", "索引", "搜索", "引擎",
         "机器", "学习", "机器学习", "深度", "网络", "神经网络", "语言", "模型", "自然", "处理", "自然语言",
         "分词", "工具", "测试", "文档", "向量", "检索", "增强", "生成", "问题", "答案", "上海", "广州", "深圳",
         "公司", "技术", "发展", "经济", "社会", "文化", "历史", "科学", "电脑", "手机", "软件", "硬件", "开发"]
PIECES = ["，", "。", "！", " ", "\n", "、", "abc", "Python3", "3.14", "100%", "C++", "x86_64", "😀", "１２３", "㐀"]


def _model(root, name, words, freqs=None, users=None):
    d = root / "jieba" / name
    d.mkdir(parents=True)
    rng = random.Random(name)
    lines = [f"{w} {freqs[w] if freqs else rng.randint(1, 5000)} n" for w in words]
    (d / "dict.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    if users is not None:
        (d / "user.txt").write_text(users, encoding="utf-8")
        (d / "config.json").write_text('{"main": "dict.txt", "users": ["user.txt"]}', encoding="utf-8")
    return d


def _corpus(n, seed):
    rng = random.Random(seed)
    docs = []
    for _ in range(n):
        parts = []
        for _ in range(rng.randint(3, 25)):
            r = rng.random()
            if r < 0.5:
                parts.append(rng.choice(WORDS))
            elif r < 0.8:
                parts.append("".join(chr(rng.randint(0x4E00, 0x9FA5)) for _ in range(rng.randint(1, 3))))
            else:
                parts.append(rng.choice(PIECES))
        docs.append("".join(parts))
    return docs


@pytest.fixture()
def models(tmp_path, monkeypatch):
    monkeypatch.setenv("LANCE_LANGUAGE_MODEL_HOME", str(tmp_path))
    _model(tmp_path, "small", WORDS)
    _model(tmp_path, "user", WORDS[:20], users="神经网络 900\n自然语言处理 40 n\n")
    return tmp_path


def _search(engine, path, query, phrase):
    if phrase:
        from importlib import import_module

        query = import_module(engine.__name__ + ".query").PhraseQuery(query, "text")
    t = engine.dataset(path).to_table(full_text_query=query, limit=20, columns=["id"])
    return t["id"].to_pylist(), t["_score"].to_numpy()


# pylance 12 aborts the process (a 96 GiB allocation) on some jieba queries whose search-mode tokens step
# back in position (a word after its own sub-words), on its own index and on nanolance's alike. Its answers
# are taken in a child process, restarted after such a query.
PYLANCE_CHILD = textwrap.dedent("""
    import json, resource, sys
    # That allocation fails (and pylance aborts) only where memory runs out first: with a terabyte of RAM it
    # succeeds and the query crawls for many minutes. Capping the child's address space makes it fail
    # everywhere.
    cap = 64 << 30  # below the 96 GiB request; room for many threads' malloc arenas
    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    if hard == resource.RLIM_INFINITY or hard > cap:
        resource.setrlimit(resource.RLIMIT_AS, (cap, hard))
    import lance
    from lance.query import PhraseQuery
    path, cases, skip = sys.argv[1], json.loads(sys.argv[2]), int(sys.argv[3])
    ds = lance.dataset(path)
    for q, phrase in cases[skip:]:
        t = ds.to_table(full_text_query=PhraseQuery(q, "text") if phrase else q, limit=20, columns=["id"])
        print(json.dumps([t["id"].to_pylist(), [float(x) for x in t["_score"].to_pylist()]]), flush=True)
""")


def _pylance_answers(path, cases):
    out = []
    while len(out) < len(cases):
        p = subprocess.run([sys.executable, "-c", PYLANCE_CHILD, path, json.dumps(cases), str(len(out))],
                           capture_output=True, text=True, timeout=1800, env=dict(os.environ))
        out += [json.loads(line) for line in p.stdout.splitlines() if line.startswith("[")]
        if p.returncode != 0:
            assert "memory allocation" in p.stderr, p.stderr[-500:]  # only that pylance abort is expected
            out.append(None)
    return out


@pytest.mark.parametrize("model", ["small", "user"])
@pytest.mark.parametrize("with_position", [False, True])
def test_jieba_index_built_and_searched_by_both_engines(models, tmp_path, model, with_position):
    lance = require_pylance()
    docs = _corpus(400, seed=7)
    table = pa.table({"id": pa.array(range(len(docs)), pa.int64()), "text": pa.array(docs)})
    paths = {}
    for name, engine in (("py", lance), ("nl", nl)):
        paths[name] = str(tmp_path / f"{name}.lance")
        engine.write_dataset(table, paths[name], max_rows_per_file=150)
        engine.dataset(paths[name]).create_scalar_index("text", "INVERTED", base_tokenizer=f"jieba/{model}",
                                                        with_position=with_position)
    rng = random.Random(11)
    queries = [w for w in WORDS[:30]] + ["我们的大学生", "神经网络模型", "Python3", "100%"]
    for _ in range(30):
        d = rng.choice(docs)
        a = rng.randint(0, max(0, len(d) - 4))
        queries.append(d[a:a + rng.randint(2, 4)])
    cases = [[q, phrase] for q in queries for phrase in ([False, True] if with_position else [False])]
    on_theirs, on_ours = _pylance_answers(paths["py"], cases), _pylance_answers(paths["nl"], cases)
    answered = 0
    for (query, phrase), ref, theirs_on_ours in zip(cases, on_theirs, on_ours):
        ours = [_search(nl, paths["nl"], query, phrase), _search(nl, paths["py"], query, phrase)]
        if ref is None:
            assert theirs_on_ours is None, (query, phrase)  # pylance aborts on either index, not on ours alone
            assert ours[0][0] == ours[1][0], (query, phrase)
            continue
        answered += 1
        ref_ids, ref_scores = ref[0], np.array(ref[1])
        for ids, scores in ours + [(theirs_on_ours[0], np.array(theirs_on_ours[1]))]:
            assert len(scores) == len(ref_scores), (query, phrase)
            assert np.allclose(scores, ref_scores, rtol=1e-5), (query, phrase)
            cut = ref_scores[-1] if len(ref_scores) else None
            assert {i for i, s in zip(ids, scores) if s != cut} == \
                {i for i, s in zip(ref_ids, ref_scores) if s != cut}, (query, phrase)
    assert answered >= len(cases) * 0.8, (answered, len(cases))


def test_jieba_alias_and_default_model(models, tmp_path):
    lance = require_pylance()
    _model(models, "default", WORDS)
    t = pa.table({"text": ["我们都有光明的前途", "光明的前途"]})
    for engine in (lance, nl):
        path = str(tmp_path / f"{engine.__name__}.lance")
        engine.write_dataset(t, path)
        engine.dataset(path).create_scalar_index("text", "INVERTED", base_tokenizer="jieba")
        assert sorted(engine.dataset(path).to_table(full_text_query="我们", with_row_id=True)["_rowid"].to_pylist()) == [0]


@pytest.mark.parametrize("setup, error", [
    ("missing_model", ValueError),
    ("missing_user_dict", OSError),
    ("missing_main_dict", OSError),
])
def test_jieba_model_errors_as_pylance(models, tmp_path, setup, error):
    lance = require_pylance()
    if setup == "missing_user_dict":
        d = _model(models, setup, WORDS)
        (d / "config.json").write_text('{"users": ["nope.txt"]}', encoding="utf-8")
    elif setup == "missing_main_dict":
        d = models / "jieba" / setup
        d.mkdir(parents=True)
        (d / "config.json").write_text('{"main": "nope.txt"}', encoding="utf-8")
    for engine in (lance, nl):
        path = str(tmp_path / f"{engine.__name__}_{setup}.lance")
        engine.write_dataset(pa.table({"text": ["我们都有光明的前途"]}), path)
        with pytest.raises(error):
            engine.dataset(path).create_scalar_index("text", "INVERTED", base_tokenizer=f"jieba/{setup}")


def test_other_tokenizers_are_unchanged_by_jieba(models, tmp_path):
    """The simple tokenizer's index over the same text is pylance's (jieba changes nothing else)."""
    lance = require_pylance()
    docs = _corpus(100, seed=3)
    table = pa.table({"id": pa.array(range(len(docs)), pa.int64()), "text": pa.array(docs)})
    for tag, engine in (("py", lance), ("nl", nl)):
        engine.write_dataset(table, str(tmp_path / f"s_{tag}.lance"))
        engine.dataset(str(tmp_path / f"s_{tag}.lance")).create_scalar_index("text", "INVERTED")
    for q in ["abc", "Python3", "C"]:
        a = lance.dataset(str(tmp_path / "s_py.lance")).to_table(full_text_query=q, columns=["id"])
        b = nl.dataset(str(tmp_path / "s_nl.lance")).to_table(full_text_query=q, columns=["id"])
        assert np.allclose(sorted(a["_score"].to_pylist()), sorted(b["_score"].to_pylist()), rtol=1e-6), q
