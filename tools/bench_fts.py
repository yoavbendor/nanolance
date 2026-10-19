#!/usr/bin/env python3
"""Full-text search: nanolance's C++ library against Rust Lance (the lance crate, 12.0.0, no Python in the process).

    python tools/bench_fts.py [--rows 500000] [--min-seconds 10] [--work DIR]   # writes bench/results/fts_index.json

The data is generated (seeded): documents of 5 to 200 words (log-normal lengths, about 40 on average)
drawn from a Zipf-distributed vocabulary of 30,000 made-up words plus English stop words, written by
pylance as one fragment (data generation, not timed). Each engine builds an INVERTED index with Lance's
default analyzer on its own copy, then searches the index it built. Before the times are reported, the
two engines' answers must agree: the same rows (ties at the cut-off may differ) and the same BM25 scores.
"""

from __future__ import annotations

import argparse
import json
import shutil
import tempfile
import warnings
from pathlib import Path

import numpy as np
import pyarrow as pa

from index_bench_common import (CPP_CMD, RUST_CMD, check_drivers, combine_stats, environment, run_driver,
                                write_workload)

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


def same_answers(rust: dict, cpp: dict, name: str) -> None:
    """Both engines return the same rows and scores for every text; ties at the cut-off may differ."""
    for text_index, (ids_r, ids_c, sc_r, sc_c) in enumerate(zip(rust["hits"][name], cpp["hits"][name],
                                                               rust["scores"][name], cpp["scores"][name])):
        a = sorted(zip(sc_r, ids_r), reverse=True)
        b = sorted(zip(sc_c, ids_c), reverse=True)
        if len(a) != len(b) or any(abs(x[0] - y[0]) > 1e-5 * max(1.0, abs(x[0])) for x, y in zip(a, b)):
            raise SystemExit(f"{name} text {text_index}: nanolance's scores differ from Rust Lance's")
        if not a:
            continue
        cut = a[-1][0]
        if {i for s, i in a if s > cut + 1e-5} != {i for s, i in b if s > cut + 1e-5}:
            raise SystemExit(f"{name} text {text_index}: the rows differ from Rust Lance's")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=int, default=500_000)
    ap.add_argument("--min-seconds", type=float, default=10.0)
    ap.add_argument("--work", type=Path, default=None)
    ap.add_argument("--out", type=Path, default=ROOT / "bench" / "results" / "fts_index.json")
    args = ap.parse_args()
    warnings.filterwarnings("ignore")
    check_drivers()

    rng = np.random.default_rng(0)
    vocab = vocabulary(30_000, rng)
    work = Path(args.work or tempfile.mkdtemp(prefix="nl_fts_"))
    work.mkdir(parents=True, exist_ok=True)
    base = work / "fts_base.lance"
    if not base.exists():
        import lance  # data generation only
        lance.write_dataset(pa.table({"id": pa.array(np.arange(args.rows)),
                                      "text": pa.array(documents(args.rows, vocab, rng))}), str(base))

    qrng = np.random.default_rng(1)
    common = [vocab[i] for i in qrng.integers(0, 50, 50)]
    rare = [vocab[i] for i in qrng.integers(2000, 30000, 50)]
    mixed = lambda k: [" ".join(vocab[i] for i in qrng.integers(0, 3000, k)) for _ in range(50)]
    two, three = mixed(2), mixed(3)
    searches = [
        ("1 common word, limit 10", common, 10, False),
        ("1 rare word, limit 10", rare, 10, False),
        ("2 words, limit 10", two, 10, False),
        ("3 words, limit 10", three, 10, False),
        ("3 words, limit 100", three, 100, False),
        ("2 words, AND, limit 10", two, 10, True),
        ("1 rare word, all matches", rare, None, False),
    ]
    lines = ["fts_config\ttext"]
    for name, texts, limit, and_op in searches:
        lines.append(f"fts_search\t{name}\t{limit or 0}\t{1 if and_op else 0}")
        lines += [f"fts_text\t{t}" for t in texts]
    workload = write_workload(work / "fts.txt", lines, args.min_seconds)

    rust = run_driver(RUST_CMD, "fts", workload, work / "rust", base, work / "rust_fts.json")["fts"]
    cpp = run_driver(CPP_CMD, "fts", workload, work / "cpp", base, work / "cpp_fts.json")["fts"]
    for name, *_ in searches:
        same_answers(rust, cpp, name)

    out = {
        "environment": environment("fts"),
        "rows": args.rows, "min_seconds": args.min_seconds,
        "builds": {"INVERTED": combine_stats(rust["builds"]["INVERTED"], cpp["builds"]["INVERTED"])},
        "searches": {name: combine_stats(rust["searches"][name], cpp["searches"][name]) for name, *_ in searches},
    }
    args.out.write_text(json.dumps(out, indent=1))
    print(f"wrote {args.out}")
    if args.work is None:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
