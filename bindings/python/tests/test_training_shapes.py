"""Training datasets end to end: COCO-shaped and Speech-Commands-shaped tables, synthetic.

tools/bench_multimodal.py runs the real COCO 2017 val and Speech Commands through nanolance and Rust
Lance, and found bugs the unit tests had not: images over 32 KB written corrupt, Rust's per-value
zstd image pages unreadable, bounding boxes (fixed-size lists inside a list of structs) refused, a
map-like constant column misread, pylance's one-page audio column read whole for every mini-batch.
Each has a focused test of its own; this file keeps the SHAPE that found them -- the same schemas
and value sizes, a few hundred rows -- and puts it through what a training job does, with files
from both writers and on one thread and several:

  build the dataset  ->  read an epoch  ->  read everything but the images / audio  ->  read a
  row range  ->  read a shuffled epoch in mini-batches (take)

checking every result against the source table and against pylance. tests/test_real_datasets.py
does the same on the real files when they have been downloaded.
"""

from __future__ import annotations

import numpy as np
import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance

BATCH = 64


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


OBJECT = pa.struct([
    ("id", pa.int64()), ("category", pa.utf8()), ("supercategory", pa.utf8()),
    ("bbox", pa.list_(pa.float32(), 4)), ("area", pa.float32()), ("iscrowd", pa.bool_()),
    ("segmentation", pa.list_(pa.list_(pa.float32()))), ("keypoints", pa.list_(pa.int32())),
])
CATEGORIES = [("person", "person"), ("dog", "animal"), ("car", "vehicle"), ("pizza", "food"), ("chair", "furniture")]


def coco_like(n=300, seed=0) -> pa.Table:
    """COCO 2017's shape: JPEG-sized images (some over 32 KB and one over 1 MB, the sizes that broke
    the plain string pages), 0-7 captions, 0-12 objects with a box, polygons (none for a crowd
    region), and keypoints for people only."""
    rng = np.random.default_rng(seed)
    sizes = rng.integers(3_000, 160_000, n)
    sizes[7] = 1_300_000
    images, captions, objects = [], [], []
    oid = 0
    for i in range(n):
        images.append(rng.bytes(int(sizes[i])))
        captions.append([f"a photo of {CATEGORIES[(i + k) % 5][0]} number {k} in scene {i}"
                         for k in range(int(rng.integers(0, 8)))] if i % 29 else [])
        row = []
        for _ in range(int(rng.integers(0, 13)) if i % 31 else 0):
            name, sup = CATEGORIES[int(rng.integers(0, 5))]
            crowd = bool(rng.random() < 0.05)
            row.append({
                "id": oid, "category": name, "supercategory": sup,
                "bbox": [float(v) for v in rng.random(4, dtype=np.float32) * 640],
                "area": float(rng.random() * 50_000), "iscrowd": crowd,
                "segmentation": [] if crowd else [[float(v) for v in rng.random(int(rng.integers(6, 40)), dtype=np.float32)]
                                                  for _ in range(int(rng.integers(1, 3)))],
                "keypoints": [int(v) for v in rng.integers(0, 640, 51)] if name == "person" else None,
            })
            oid += 1
        objects.append(row)
    return pa.table({
        "image_id": pa.array(np.arange(n) * 7 + 139, pa.int64()),
        "file_name": pa.array([f"{i * 7 + 139:012d}.jpg" for i in range(n)]),
        "width": pa.array(rng.integers(200, 640, n), pa.int32()),
        "height": pa.array(rng.integers(200, 640, n), pa.int32()),
        "license": pa.array(rng.integers(1, 8, n), pa.int32()),
        "image": pa.array(images, pa.binary()),
        "captions": pa.array(captions, pa.list_(pa.utf8())),
        "objects": pa.array(objects, pa.list_(OBJECT)),
    })


def speech_like(n=300, seed=1) -> pa.Table:
    """Speech Commands' shape: one-second 16 kHz int16 clips (some shorter, as in the real set)."""
    rng = np.random.default_rng(seed)
    words = ["yes", "no", "up", "down", "left", "right", "on", "off", "stop", "go"]
    lengths = np.where(rng.random(n) < 0.1, rng.integers(5_000, 16_000, n), 16_000)
    waves = [rng.integers(-8_000, 8_000, int(k), dtype=np.int16) for k in lengths]
    offsets = np.concatenate([[0], np.cumsum(lengths)]).astype(np.int32)
    return pa.table({
        "file": pa.array([f"{words[i % 10]}/{i:08x}_nohash_{i % 3}.wav" for i in range(n)]),
        "label": pa.array([words[i % 10] for i in range(n)]),
        "speaker": pa.array([f"{(i * 2654435761) % (1 << 32):08x}" for i in range(n)]),
        "utterance": pa.array([i % 3 for i in range(n)], pa.int32()),
        "sample_rate": pa.array([16_000] * n, pa.int32()),
        "waveform": pa.ListArray.from_arrays(pa.array(offsets), pa.array(np.concatenate(waves), pa.int16())),
    })


SHAPES = {
    "coco": (coco_like, ["image"], ["image", "captions", "objects"]),
    "speech": (speech_like, ["waveform"], ["waveform", "label"]),
}


@pytest.fixture(scope="module", params=list(SHAPES))
def dataset(request, lance_mod, tmp_path_factory):
    build, heavy, train = SHAPES[request.param]
    table = build()
    work = tmp_path_factory.mktemp(request.param)
    paths = {"nanolance": work / "nl.lance", "rust": work / "rust.lance"}
    nanolance.write_table(table, paths["nanolance"])
    lance_mod.write_dataset(table, str(paths["rust"]), data_storage_version="2.2")
    return request.param, table, heavy, train, paths


@pytest.fixture(params=[1, 4], ids=["1 thread", "4 threads"])
def n_threads(request):
    before = nanolance.get_threads()
    nanolance.set_threads(request.param)
    yield request.param
    nanolance.set_threads(before)


@pytest.mark.parametrize("writer", ["nanolance", "rust"])
def test_an_epoch(lance_mod, dataset, writer, n_threads):
    name, table, heavy, train, paths = dataset
    path = paths[writer]
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == table.to_pydict()
    if writer == "nanolance":
        assert lance_mod.dataset(str(path)).to_table().to_pydict() == table.to_pydict()


@pytest.mark.parametrize("writer", ["nanolance", "rust"])
def test_everything_but_the_media(dataset, writer, n_threads):
    name, table, heavy, train, paths = dataset
    cols = [c for c in table.column_names if c not in heavy]
    got = pa.table(nanolance.read_table(paths[writer], columns=cols))
    assert got.to_pydict() == table.select(cols).to_pydict()


@pytest.mark.parametrize("writer", ["nanolance", "rust"])
def test_a_row_range(dataset, writer, n_threads):
    name, table, heavy, train, paths = dataset
    got = pa.table(nanolance.read_table(paths[writer], offset=37, length=150))
    assert got.to_pydict() == table.slice(37, 150).to_pydict()


@pytest.mark.parametrize("writer", ["nanolance", "rust"])
def test_a_shuffled_epoch(lance_mod, dataset, writer, n_threads):
    name, table, heavy, train, paths = dataset
    order = np.random.default_rng(5).permutation(table.num_rows).tolist()
    want = table.select(train)
    ds = lance_mod.dataset(str(paths[writer]))
    for start in range(0, len(order), BATCH):
        batch = order[start:start + BATCH]
        got = pa.table(nanolance.take(paths[writer], batch, columns=train))
        assert got.to_pydict() == want.take(pa.array(batch)).to_pydict(), (name, writer, start)
        if start == 0:
            assert got.to_pydict() == ds.take(batch, columns=train).to_pydict()


def test_the_file_does_not_depend_on_the_thread_count(dataset, tmp_path):
    name, table, heavy, train, paths = dataset
    before = nanolance.get_threads()
    try:
        nanolance.set_threads(1)
        nanolance.write_table(table, tmp_path / "one.lance")
        nanolance.set_threads(4)
        nanolance.write_table(table, tmp_path / "four.lance")
    finally:
        nanolance.set_threads(before)
    one = sorted((tmp_path / "one.lance" / "data").iterdir())
    four = sorted((tmp_path / "four.lance" / "data").iterdir())
    assert [p.read_bytes() for p in one] == [p.read_bytes() for p in four]
