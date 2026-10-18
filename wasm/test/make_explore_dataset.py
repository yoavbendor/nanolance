#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Writes the small local image dataset the explore page's browser test reads over the Range server.

3,000 rows: image (PNG of random pixels, 64x64), label_name (5 classes, shuffled), a 32-dim float32
vector (fixed_size_list) drawn around one well-separated center per class, and a caption. Written
with pylance in 500-row fragments, so a take crosses fragments.

Usage: make_explore_dataset.py OUTDIR
  -> OUTDIR/explore_small.lance and OUTDIR/expected.json (the values each row must come back with:
     vectors as float32, image sha256, label_name, class centers)
"""

import hashlib
import json
import os
import random
import struct
import sys
import zlib

import lance
import numpy as np
import pyarrow as pa

ROWS = 3000
DIM = 32
CLASSES = ["cat", "car", "boat", "tree", "cake"]


def png(width, height, rnd):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + rnd.randbytes(width * 3) for _ in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 1))
        + chunk(b"IEND", b"")
    )


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    rnd = random.Random(20261018)
    nprng = np.random.default_rng(20261018)
    centers = nprng.normal(size=(len(CLASSES), DIM)) * 10.0  # well separated
    label_ids = [rnd.randrange(len(CLASSES)) for _ in range(ROWS)]
    vectors = (centers[label_ids] + nprng.normal(size=(ROWS, DIM))).astype(np.float32)
    images, expected = [], []
    for i in range(ROWS):
        image = png(64, 64, rnd)
        images.append(image)
        expected.append({
            "label_name": CLASSES[label_ids[i]],
            "image_len": len(image),
            "image_sha256": hashlib.sha256(image).hexdigest(),
        })
    table = pa.table({
        "id": pa.array(range(ROWS), pa.int64()),
        "image": pa.array(images, pa.binary()),
        "label_name": pa.array([CLASSES[k] for k in label_ids], pa.string()),
        "caption": pa.array([f"a {CLASSES[k]} in test image {i}" for i, k in enumerate(label_ids)], pa.string()),
        "image_emb": pa.FixedSizeListArray.from_arrays(pa.array(vectors.ravel(), pa.float32()), DIM),
    })
    path = os.path.join(out, "explore_small.lance")
    lance.write_dataset(table, path, max_rows_per_file=500, mode="overwrite")
    ds = lance.dataset(path)
    # The values pylance reads back, which the module must match exactly.
    back = ds.to_table(columns=["image_emb"]).column("image_emb").combine_chunks()
    values = np.asarray(back.values.to_numpy(zero_copy_only=False), dtype=np.float32).reshape(ROWS, DIM)
    for i in range(ROWS):
        expected[i]["vector"] = values[i].tolist()
        expected[i]["vector_hex"] = values[i].tobytes().hex()
    with open(os.path.join(out, "expected.json"), "w") as f:
        json.dump({"rows": expected, "dim": DIM, "classes": CLASSES, "fragments": len(ds.get_fragments()),
                   "version": ds.version}, f)
    print(f"wrote {path}: {ds.count_rows()} rows, {len(ds.get_fragments())} fragments, version {ds.version}")


if __name__ == "__main__":
    main()
