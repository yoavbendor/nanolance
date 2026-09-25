#!/usr/bin/env python3
"""Training-data benchmark on two widely used datasets: nanolance against Rust Lance (pylance), with
Parquet as the familiar yardstick.

    python tools/bench_multimodal.py --data DIR [--runs 3]   # writes bench/results/multimodal.json

DIR holds the downloads (the script does not fetch them):

  COCO 2017 validation   val2017.zip (5,000 JPEGs, 815 MB) and annotations/{instances,captions,
                         person_keypoints}_val2017.json from annotations_trainval2017.zip
                         -- https://cocodataset.org; mirrored at s3.amazonaws.com/fast-ai-coco/
                         Annotations CC BY 4.0; images under their Flickr licenses (not redistributed).
  Speech Commands v0.02  speech/ -- the test set speech_commands_test_set_v0.02.tar.gz extracted
                         (4,890 one-second 16 kHz clips over 35 words), CC BY 4.0
                         -- storage.googleapis.com/download.tensorflow.org/data/

Each becomes one Arrow table in the shape a training pipeline keeps it:

  coco    image_id, file_name, width, height, license, image (JPEG bytes), captions list<utf8>,
          objects list<struct<id, category, supercategory, bbox fixed_size_list<float32, 4>, area,
          iscrowd bool, segmentation list<list<float32>>, keypoints list<int32> (people only)>>
  speech  file, label, speaker, utterance, sample_rate, waveform list<int16> (the PCM samples)

and is put through what a training job does with its data, by each engine:

  write         build the dataset from the table
  scan          read every column: an epoch in file order
  meta          read everything but the images / waveforms: filtering, statistics, label maps
  shuffled      an epoch in random mini-batches of 64 rows of the training columns, by take():
                every row once, in a seeded random order (Parquet has no row-level random access,
                so it has no number here)

Every operation reports wall time and CPU time (user + system of the whole process, so every thread
an engine starts is counted): a single-threaded reader and a thread-pool reader are compared on the
same cost. Each is the median of --runs runs after a warm-up, files in the page cache. Before any
time is kept, the result is checked against the source table.
"""

from __future__ import annotations

import argparse
import datetime as dt
import io
import json
import os
import platform
import re
import resource
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import wave
import zipfile
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

import lance
import nanolance

ROOT = Path(__file__).resolve().parents[1]
BATCH = 64


# ── Datasets ─────────────────────────────────────────────────────────────────────────────────────

def build_coco(data: Path) -> pa.Table:
    instances = json.loads((data / "annotations" / "instances_val2017.json").read_text())
    captions = json.loads((data / "annotations" / "captions_val2017.json").read_text())
    keypoints = json.loads((data / "annotations" / "person_keypoints_val2017.json").read_text())
    cats = {c["id"]: c for c in instances["categories"]}
    kp_by_ann = {a["id"]: a["keypoints"] for a in keypoints["annotations"]}
    caps: dict[int, list[str]] = {}
    for c in captions["annotations"]:
        caps.setdefault(c["image_id"], []).append(c["caption"].strip())
    objs: dict[int, list[dict]] = {}
    for a in instances["annotations"]:
        seg = a["segmentation"] if isinstance(a["segmentation"], list) else []  # crowd regions are RLE
        objs.setdefault(a["image_id"], []).append({
            "id": a["id"],
            "category": cats[a["category_id"]]["name"],
            "supercategory": cats[a["category_id"]]["supercategory"],
            "bbox": [float(v) for v in a["bbox"]],
            "area": float(a["area"]),
            "iscrowd": bool(a["iscrowd"]),
            "segmentation": [[float(v) for v in poly] for poly in seg],
            "keypoints": kp_by_ann.get(a["id"]),
        })
    images = sorted(instances["images"], key=lambda im: im["id"])
    with zipfile.ZipFile(data / "val2017.zip") as z:
        jpegs = [z.read(f"val2017/{im['file_name']}") for im in images]
    obj_type = pa.struct([
        ("id", pa.int64()), ("category", pa.utf8()), ("supercategory", pa.utf8()),
        ("bbox", pa.list_(pa.float32(), 4)), ("area", pa.float32()), ("iscrowd", pa.bool_()),
        ("segmentation", pa.list_(pa.list_(pa.float32()))), ("keypoints", pa.list_(pa.int32())),
    ])
    return pa.table({
        "image_id": pa.array([im["id"] for im in images], pa.int64()),
        "file_name": pa.array([im["file_name"] for im in images]),
        "width": pa.array([im["width"] for im in images], pa.int32()),
        "height": pa.array([im["height"] for im in images], pa.int32()),
        "license": pa.array([im["license"] for im in images], pa.int32()),
        "image": pa.array(jpegs, pa.binary()),
        "captions": pa.array([caps.get(im["id"], []) for im in images], pa.list_(pa.utf8())),
        "objects": pa.array([objs.get(im["id"], []) for im in images], pa.list_(obj_type)),
    })


def build_speech(data: Path) -> pa.Table:
    root = data / "speech"
    files, labels, speakers, utterances, rates, waves = [], [], [], [], [], []
    for path in sorted(root.glob("*/*.wav")):
        with wave.open(str(path)) as w:
            rate = w.getframerate()
            samples = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
        speaker, _, utt = path.name.partition("_nohash_")
        digits = re.match(r"\d+", utt)
        files.append(f"{path.parent.name}/{path.name}")
        labels.append(path.parent.name)
        speakers.append(speaker)
        utterances.append(int(digits.group()) if digits else 0)
        rates.append(rate)
        waves.append(samples)
    offsets = np.zeros(len(waves) + 1, np.int32)
    np.cumsum([len(w) for w in waves], out=offsets[1:])
    waveform = pa.ListArray.from_arrays(pa.array(offsets), pa.array(np.concatenate(waves), pa.int16()))
    return pa.table({
        "file": pa.array(files), "label": pa.array(labels), "speaker": pa.array(speakers),
        "utterance": pa.array(utterances, pa.int32()), "sample_rate": pa.array(rates, pa.int32()),
        "waveform": waveform,
    })


DATASETS = {
    "coco": {"build": build_coco, "heavy": ["image"], "train": ["image", "captions", "objects"],
             "title": "COCO 2017 val", "what": "images, captions, boxes, polygons, keypoints"},
    "speech": {"build": build_speech, "heavy": ["waveform"], "train": ["waveform", "label"],
               "title": "Speech Commands v0.02 test", "what": "one-second audio clips and their words"},
}


# ── Measuring ────────────────────────────────────────────────────────────────────────────────────

def cpu_seconds():
    r = resource.getrusage(resource.RUSAGE_SELF)
    return r.ru_utime + r.ru_stime


def measure(fn, runs):
    """(median wall ms, median CPU ms) over `runs` runs after one warm-up."""
    fn()
    walls, cpus = [], []
    for _ in range(runs):
        c0, t0 = cpu_seconds(), time.perf_counter()
        fn()
        walls.append((time.perf_counter() - t0) * 1e3)
        cpus.append((cpu_seconds() - c0) * 1e3)
    return statistics.median(walls), statistics.median(cpus)


def same(got: pa.Table, want: pa.Table) -> bool:
    got = got.select(want.column_names) if set(got.column_names) == set(want.column_names) else got
    return got.num_rows == want.num_rows and got.combine_chunks().equals(want.combine_chunks())


def dir_bytes(path: Path) -> int:
    if path.is_file():
        return path.stat().st_size
    return sum(p.stat().st_size for p in path.rglob("*") if p.is_file())


def run_dataset(name, spec, table, runs, work: Path):
    rec = {"title": spec["title"], "what": spec["what"], "rows": table.num_rows, "arrow_bytes": table.nbytes,
           "columns": table.column_names, "ops": {}, "size": {}, "errors": {}}
    paths = {"nanolance": work / f"{name}_nl.lance", "rust-lance": work / f"{name}_rust.lance",
             "parquet": work / f"{name}.parquet"}

    def fresh(p):
        shutil.rmtree(p, ignore_errors=True) if p.is_dir() else p.unlink(missing_ok=True)

    writers = {
        "nanolance": lambda p: nanolance.write_table(table, p),
        "rust-lance": lambda p: lance.write_dataset(table, str(p), data_storage_version="2.2"),
        "parquet": lambda p: pq.write_table(table, p, compression="zstd"),
    }
    rec["ops"]["write"] = {}
    for engine, write in writers.items():
        p = paths[engine]
        wall, cpu = measure(lambda: (fresh(p), write(p)), runs)
        rec["ops"]["write"][engine] = {"wall_ms": wall, "cpu_ms": cpu}
        rec["size"][engine] = dir_bytes(p)
        print(f"  {name} write   {engine:10s} {wall:9.1f} ms wall {cpu:9.1f} ms CPU  {rec['size'][engine] / 1e6:8.1f} MB",
              flush=True)

    meta_cols = [c for c in table.column_names if c not in spec["heavy"]]
    readers = {
        "nanolance": lambda p, cols: pa.table(nanolance.read_table(p, columns=cols)),
        "rust-lance": lambda p, cols: lance.dataset(str(p)).to_table(columns=cols),
        "parquet": lambda p, cols: pq.read_table(p, columns=cols),
    }
    for op, cols in (("scan", None), ("meta", meta_cols)):
        rec["ops"][op] = {}
        want = table if cols is None else table.select(cols)
        for engine, read in readers.items():
            p = paths[engine]
            if not same(read(p, cols), want):
                rec["errors"][f"{op} {engine}"] = "returned different data"
                continue
            wall, cpu = measure(lambda: read(p, cols), runs)
            rec["ops"][op][engine] = {"wall_ms": wall, "cpu_ms": cpu}
            print(f"  {name} {op:7s} {engine:10s} {wall:9.1f} ms wall {cpu:9.1f} ms CPU", flush=True)

    # Shuffled epoch: every row once, in seeded random mini-batches, training columns only.
    order = np.random.default_rng(0).permutation(table.num_rows)
    batches = [order[i:i + BATCH].tolist() for i in range(0, len(order), BATCH)]
    cols = spec["train"]
    want = table.select(cols)
    rust_ds = {}

    def nl_epoch(p=paths["nanolance"]):
        for b in batches:
            nanolance.take(p, b, columns=cols)

    def rust_epoch(p=paths["rust-lance"]):
        ds = lance.dataset(str(p))  # opened once per epoch, as a loader would
        for b in batches:
            ds.take(b, columns=cols)

    for engine, p, epoch, take in (
        ("nanolance", paths["nanolance"], nl_epoch, lambda p, b: pa.table(nanolance.take(p, b, columns=cols))),
        ("rust-lance", paths["rust-lance"], rust_epoch, lambda p, b: lance.dataset(str(p)).take(b, columns=cols)),
    ):
        ok = all(same(take(p, b), want.take(pa.array(b))) for b in batches[:5] + batches[-2:])
        if not ok:
            rec["errors"][f"shuffled {engine}"] = "returned different data"
            continue
        wall, cpu = measure(epoch, runs)
        rec["ops"].setdefault("shuffled", {})[engine] = {"wall_ms": wall, "cpu_ms": cpu}
        print(f"  {name} shuffled {engine:10s} {wall:9.1f} ms wall {cpu:9.1f} ms CPU  ({len(batches)} batches)",
              flush=True)
    rec["shuffled_batches"] = len(batches)
    for p in paths.values():
        fresh(p)
    return rec


def environment():
    def git(*args):
        try:
            return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()
        except OSError:
            return ""
    cpu = next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")), "")
    return {"date": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"), "cpu": cpu,
            "cores": os.cpu_count(), "os": f"{platform.system()} {platform.release()}",
            "pylance": lance.__version__, "pyarrow": pa.__version__, "nanolance_commit": git("rev-parse", "--short", "HEAD")}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", required=True, type=Path)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--only", nargs="*", choices=list(DATASETS))
    ap.add_argument("--out", default=str(ROOT / "bench" / "results" / "multimodal.json"))
    args = ap.parse_args(argv)
    results = {"environment": environment(), "runs": args.runs, "batch": BATCH, "datasets": {}}
    print(f"environment: {results['environment']}", flush=True)
    work = Path(tempfile.mkdtemp(prefix="nlmm-", dir=os.environ.get("NL_BENCH_TMP")))
    try:
        for name in args.only or list(DATASETS):
            spec = DATASETS[name]
            t0 = time.perf_counter()
            table = spec["build"](args.data)
            print(f"{name}: {table.num_rows} rows, {table.nbytes / 1e6:.0f} MB in memory "
                  f"(built in {time.perf_counter() - t0:.1f} s)", flush=True)
            results["datasets"][name] = run_dataset(name, spec, table, args.runs, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    Path(args.out).write_text(json.dumps(results, indent=1))
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
