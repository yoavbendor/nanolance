#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Writes the small local dataset the weblance browser test reads over the Range server.

500 rows shaped like the COCO embeddings table (image_id, file_name, caption, coco_url, split,
width, height, vector, image_bytes). Written with pylance in 200-row fragments, so a take crosses
fragments. The image bytes are real PNGs of random pixels, deterministic per row.

Usage: make_local_dataset.py OUTDIR
  -> OUTDIR/coco_small.lance and OUTDIR/expected.json (the values each row must come back with)
"""

import hashlib
import json
import os
import random
import struct
import sys
import zlib

import lance
import pyarrow as pa

ROWS = 500
VECTOR_DIM = 16


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
    rnd = random.Random(20260617)
    cols = {k: [] for k in ["image_id", "file_name", "caption", "coco_url", "split", "width", "height", "vector", "image_bytes"]}
    expected = []
    for i in range(ROWS):
        image_id = 1000 + i * 7
        width, height = rnd.randint(64, 160), rnd.randint(64, 160)
        image = png(width, height, rnd)
        file_name = f"{image_id:012d}.png"
        row = {
            "image_id": image_id,
            "file_name": file_name,
            "caption": f"a test caption for row {i}: a {rnd.choice(['red', 'blue', 'green'])} thing",
            "coco_url": f"http://images.cocodataset.org/val2017/{file_name}",
            "split": "val" if i % 5 == 0 else "train",
            "width": width,
            "height": height,
        }
        cols["image_id"].append(row["image_id"])
        cols["file_name"].append(row["file_name"])
        cols["caption"].append(row["caption"])
        cols["coco_url"].append(row["coco_url"])
        cols["split"].append(row["split"])
        cols["width"].append(width)
        cols["height"].append(height)
        cols["vector"].append([rnd.uniform(-1, 1) for _ in range(VECTOR_DIM)])
        cols["image_bytes"].append(image)
        row["image_len"] = len(image)
        row["image_sha256"] = hashlib.sha256(image).hexdigest()
        expected.append(row)

    table = pa.table(
        {
            "image_id": pa.array(cols["image_id"], pa.int64()),
            "file_name": pa.array(cols["file_name"], pa.string()),
            "caption": pa.array(cols["caption"], pa.string()),
            "coco_url": pa.array(cols["coco_url"], pa.string()),
            "split": pa.array(cols["split"], pa.string()),
            "width": pa.array(cols["width"], pa.int64()),
            "height": pa.array(cols["height"], pa.int64()),
            "vector": pa.FixedSizeListArray.from_arrays(
                pa.array([x for v in cols["vector"] for x in v], pa.float32()), VECTOR_DIM
            ),
            "image_bytes": pa.array(cols["image_bytes"], pa.binary()),
        }
    )
    path = os.path.join(out, "coco_small.lance")
    lance.write_dataset(table, path, max_rows_per_file=200, mode="overwrite")
    ds = lance.dataset(path)
    with open(os.path.join(out, "expected.json"), "w") as f:
        json.dump({"rows": expected, "fragments": len(ds.get_fragments()), "version": ds.version}, f)
    print(f"wrote {path}: {ds.count_rows()} rows, {len(ds.get_fragments())} fragments, version {ds.version}")


if __name__ == "__main__":
    main()
