#!/usr/bin/env python3
"""Full-text search: nanolance against Rust Lance (pylance), building an INVERTED index and searching.

    python tools/bench_fts.py [--rows 500000] [--runs 5] [--work DIR]   # writes bench/results/fts_index.json

The data is generated (seeded): documents of 5 to 200 words (log-normal lengths, about 40 on
average) drawn from a Zipf-distributed vocabulary of 30,000 made-up words plus English stop words,
written by pylance. Each engine builds the index (Lance's default analyzer) on its own copy, then
searches the index it built. Before timing, nanolance's answers on pylance's index are checked
against pylance's (the same scores; rows tied at the cut may differ). Search times are the median of
--runs passes over 50 queries of each kind (warm, files in the page cache), per query; builds run
once.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import tempfile
import time
import warnings
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import pyarrow as pa

ROOT = Path(__file__).resolve().parents[1]
STOP = "the a an and of to in is it that for on with as by at from this be are was".split()
SYLLABLES = ("ka ri to sta mon ex pre ing tion er al ver com pro con dis ar el im in ol us ent "
             "ble ly ness ment ful ous ive ize ate ism ist ")


def vocabulary(n, rng):
    syl = SYLLABLES.split()
    words = set()
    while len(words) < n:
        words.add("".join(rng.choice(syl, rng.integers(1, 5))))
    return sorted(words)


def documents(rows, vocab, rng):
    ranks = np.minimum(rng.zipf(1.15, size=rows * 60) - 1, len(vocab) - 1)
    lengths = np.clip(rng.lognormal(3.4, 0.7, rows).astype(int), 5, 200)
    stops = rng.random(rows * 60) < 0.3
    out, at = [], 0
    for n in lengths:
        words = [STOP[r % len(STOP)] if s else vocab[r] for r, s in zip(ranks[at:at + n], stops[at:at + n])]
        at = (at + n) % (len(ranks) - 200)
        out.append(" ".join(words))
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=500_000)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--work", type=Path, default=None)
    ap.add_argument("--out", type=Path, default=ROOT / "bench" / "results" / "fts_index.json")
    args = ap.parse_args()
    warnings.filterwarnings("ignore")
    os.environ.setdefault("RUST_LOG", "error")
    import lance
    import nanolance.lance as nl
    from lance import query as lq
    from nanolance.lance import query as nq

    rng = np.random.default_rng(0)
    vocab = vocabulary(30_000, rng)
    work = Path(args.work or tempfile.mkdtemp(prefix="nl_fts_"))
    base = work / "base.lance"
    if not base.exists():
        lance.write_dataset(pa.table({"id": pa.array(np.arange(args.rows)),
                                      "text": pa.array(documents(args.rows, vocab, rng))}), str(base))

    qrng = np.random.default_rng(1)
    common = [vocab[i] for i in qrng.integers(0, 50, 50)]
    rare = [vocab[i] for i in qrng.integers(2000, 30000, 50)]
    mixed = lambda k: [" ".join(vocab[i] for i in qrng.integers(0, 3000, k)) for _ in range(50)]
    two, three = mixed(2), mixed(3)
    searches = [
        ("1 common word, limit 10", common, {"limit": 10}),
        ("1 rare word, limit 10", rare, {"limit": 10}),
        ("2 words, limit 10", two, {"limit": 10}),
        ("3 words, limit 10", three, {"limit": 10}),
        ("3 words, limit 100", three, {"limit": 100}),
        ("2 words, AND, limit 10", two, {"limit": 10, "and": True}),
        ("1 rare word, all matches", rare, {}),
    ]

    paths, build = {}, {}
    for who, mod in (("pylance", lance), ("nanolance", nl)):
        path = work / f"{who}.lance"
        shutil.rmtree(path, ignore_errors=True)
        shutil.copytree(base, path)
        paths[who] = str(path)
        t0 = time.perf_counter()
        mod.dataset(str(path)).create_scalar_index("text", "INVERTED")
        build[who] = time.perf_counter() - t0
        print(who, "build", round(build[who], 2), "s")

    def query(mod_q, text, extra):
        if extra.get("and"):
            return mod_q.MatchQuery(text, "text", operator=mod_q.FullTextOperator.AND)
        return text

    # nanolance answers as pylance does, on pylance's index and on its own.
    theirs, ours = lance.dataset(paths["pylance"]), nl.dataset(paths["pylance"])
    for name, texts, extra in searches:
        limit = extra.get("limit")
        for text in texts[:10]:
            a = theirs.to_table(full_text_query=query(lq, text, extra), columns=["id", "_score"], limit=limit)
            b = ours.to_table(full_text_query=query(nq, text, extra), columns=["id", "_score"], limit=limit)
            c = lance.dataset(paths["nanolance"]).to_table(full_text_query=query(lq, text, extra),
                                                           columns=["id", "_score"], limit=limit)
            sa, sb, sc = (t.column("_score").to_pylist() for t in (a, b, c))
            if sa != sb or sa != sc:
                raise SystemExit(f"{name} {text!r}: the answers differ")
            cut = sa[-1] if sa else None
            if {i for i, s in zip(a.column("id").to_pylist(), sa) if s != cut} != {
                    i for i, s in zip(b.column("id").to_pylist(), sb) if s != cut}:
                raise SystemExit(f"{name} {text!r}: the rows differ")

    results = {}
    for name, texts, extra in searches:
        row = {}
        for who, mod, mq in (("pylance", lance, lq), ("nanolance", nl, nq)):
            ds = mod.dataset(paths[who])
            qs = [query(mq, t, extra) for t in texts]
            run = lambda: [ds.to_table(full_text_query=q, columns=["id", "_score"], limit=extra.get("limit"))
                           for q in qs]
            run()
            xs = []
            for _ in range(args.runs):
                t0 = time.perf_counter()
                run()
                xs.append((time.perf_counter() - t0) / len(qs))
            row[who] = statistics.median(xs) * 1000
        results[name] = row
        print(name, json.dumps(row))

    sizes = {who: sum(f.stat().st_size for f in (Path(p) / "_indices").rglob("*") if f.is_file())
             for who, p in paths.items()}
    commit = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True,
                            text=True).stdout.strip()
    if subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"],
                      capture_output=True, text=True).stdout.strip():
        commit += " with uncommitted changes"
    out = {
        "environment": {"pylance": lance.__version__, "nanolance_commit": commit, "cores": os.cpu_count(),
                        "machine": platform.processor() or platform.machine(),
                        "date": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")},
        "rows": args.rows, "runs": args.runs, "build_s": build, "index_bytes": sizes, "search_ms": results,
    }
    args.out.write_text(json.dumps(out, indent=1))
    print(f"wrote {args.out}")
    if args.work is None:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
