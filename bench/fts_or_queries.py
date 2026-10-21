#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Multi-word full-text queries (OR, limit 10) over a Zipf corpus: nanolance vs pylance, best of seven.

    python3 bench/fts_or_queries.py [--docs 500000] [--dir /tmp/fts-or]

ROADMAP item 9. 2026-10-21, 500k documents, 4 cores: before block-max MaxScore pruning nanolance took
6-9 ms per query with a common word (0.2-0.5x pylance); with it 0.4-2.2 ms (1.8-6x pylance).
NANOLANCE_FTS_EXHAUSTIVE=1 times the exhaustive search for comparison.
"""

import argparse
import os
import shutil
import time

import numpy as np
import pyarrow as pa

import nanolance.lance as nl

QUERIES = {
    "2 common": "w3 w10",
    "2 mixed": "w5 w2000",
    "3 mixed": "w1 w50 w9000",
    "5 mixed": "w0 w7 w300 w4000 w20000",
    "5 rare": "w1000 w2000 w3000 w4000 w5000",
    "8 mixed": "w2 w9 w40 w150 w600 w2500 w8000 w30000",
}


def corpus(n: int, vocab: int = 50_000) -> pa.Table:
    rng = np.random.default_rng(1)
    names = np.array([f"w{i}" for i in range(vocab)])
    p = 1.0 / np.arange(1, vocab + 1) ** 1.05
    p /= p.sum()
    lengths = rng.integers(20, 120, n)
    words = rng.choice(vocab, size=int(lengths.sum()), p=p)
    docs, at = [], 0
    for length in lengths:
        docs.append(" ".join(names[words[at:at + length]]))
        at += length
    return pa.table({"id": pa.array(np.arange(n), pa.int64()), "text": pa.array(docs)})


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--docs", type=int, default=500_000)
    ap.add_argument("--dir", default="/tmp/fts-or-bench")
    args = ap.parse_args()
    engines = [("nanolance", nl)]
    try:
        import lance

        engines.append(("pylance", lance))
    except ImportError:
        print("pylance not installed: nanolance only")
    table = corpus(args.docs)
    for name, engine in engines:
        path = os.path.join(args.dir, f"{name}.lance")
        shutil.rmtree(path, ignore_errors=True)
        engine.write_dataset(table, path)
        engine.dataset(path).create_scalar_index("text", "INVERTED")
    for label, query in QUERIES.items():
        times = {}
        for name, engine in engines:
            ds = engine.dataset(os.path.join(args.dir, f"{name}.lance"))
            ds.to_table(full_text_query=query, limit=10, columns=["id"])  # warm the index cache
            best = float("inf")
            for _ in range(7):
                start = time.perf_counter()
                ds.to_table(full_text_query=query, limit=10, columns=["id"])
                best = min(best, time.perf_counter() - start)
            times[name] = best
        line = "  ".join(f"{name} {t * 1000:7.2f} ms" for name, t in times.items())
        ratio = f"  {times['pylance'] / times['nanolance']:.1f}x" if "pylance" in times else ""
        print(f"{label:9} {line}{ratio}", flush=True)


if __name__ == "__main__":
    main()
