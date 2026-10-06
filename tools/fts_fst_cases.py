#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Writes tests/golden/fts_fst: key/value cases (<name>.txt) and the Rust fst crate's map for each
(<name>.fst, from tools/fts_tables's `fst` mode).

usage: fts_fst_cases.py FTS_TABLES_BINARY OUT_DIR [SCALE]
"""
import os
import random
import subprocess
import sys


def escape(key: bytes) -> str:
    out = []
    for b in key:
        if 0x21 <= b < 0x7F and b != 0x5C:
            out.append(chr(b))
        else:
            out.append("\\x%02x" % b)
    return "".join(out)


def words(rng, n, syllables):
    out = set()
    while len(out) < n:
        out.add("".join(rng.choice(syllables) for _ in range(rng.randint(1, 4))).encode())
    return sorted(out)


def cases(scale):
    rng = random.Random(12)
    syl = ["a", "e", "i", "o", "u", "ka", "ri", "to", "sta", "ing", "tion", "er", "qu", "ph", "é", "ß", "東", "x"]
    yield "empty", []
    yield "empty_key_zero", [(b"", 0)]
    yield "empty_key_value", [(b"", 5), (b"a", 1), (b"ab", 0)]
    yield "words_ids", [(w, i) for i, w in enumerate(rng.sample(words(rng, 3000 * scale, syl), 3000 * scale))]
    ws = words(rng, 3000 * scale, syl)
    yield "words_sorted_ids", [(w, i) for i, w in enumerate(ws)]
    yield "words_zero", [(w, 0) for w in ws]
    yield "words_big", [(w, rng.getrandbits(64)) for w in ws[:500]]
    yield "bytes", sorted({(bytes([a]), a) for a in range(256)} | {(bytes([a, b]), rng.randrange(1 << 20)) for a in (0, 97, 255) for b in range(0, 256, 3)})
    yield "dense_33", [(bytes([0x30 + i]) + s, i * 7 + j) for i in range(33) for j, s in enumerate((b"", b"x", b"yz"))]
    suffixes = [b"ing", b"ation", b"ness", b"s", b""]
    stems = words(rng, 400 * scale, syl[:12] if scale == 1 else syl)
    yield "suffixes", sorted({(s + x, len(s) * 3 + k) for s in stems for k, x in enumerate(suffixes)})


def main():
    tool, out_dir = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    os.makedirs(out_dir, exist_ok=True)
    for name, pairs in cases(scale):
        # Unique, sorted keys.
        seen = {}
        for k, v in pairs:
            seen.setdefault(k, v)
        pairs = sorted(seen.items())
        text = "".join("%s\t%d\n" % (escape(k), v) for k, v in pairs)
        with open(os.path.join(out_dir, name + ".txt"), "w") as f:
            f.write(text)
        subprocess.run([tool, "fst", os.path.join(out_dir, name + ".fst")], input=text.encode(), check=True)


if __name__ == "__main__":
    main()
