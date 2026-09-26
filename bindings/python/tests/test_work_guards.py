"""How results are produced, not only what they are: guards on the work nanolance does.

Each test here pins a performance problem that real training data (COCO 2017, Speech Commands; see
tools/bench_multimodal.py) exposed and a fix removed. A correctness test would still pass if any of
them came back -- the rows are right, just slow or bloated -- and a timing test would flake on a
shared CI machine. So these read nanolance's work counters (nanolance._work_stats: bytes read, the
largest single read, page windows, row ranges, write buffering, buffer-pool and take-cache hits) and
bound them, with room to spare, from the values measured when the fix went in.

  * take() on pylance's one-page audio column decoded the whole 150 MB page per mini-batch: 118 s
    for a shuffled Speech Commands epoch. It now reads the chunks holding the rows.
  * A full read of that page held it three or four times over (921 MB). It now reads ~4 MiB windows.
  * nanolance wrote rows of 32 KB as one-row pages; Rust Lance then paid a page per row (3x slower
    shuffled epochs). Large-row list pages are now ~1 MiB.
  * The read planner sized work by encoded bytes and never split a dictionary-coded list of strings
    (1.3 MB on disk, 19 ms to decode). It now counts decoded values.
  * Parallel writes buffered every column in memory -- for COCO's images (800 of 828 MB) that made
    4 threads slower than one. A column that is most of the data now streams.
  * Parallel reads faulted their output memory in on every read: the buffer pool reuses it, and
    row-range slicing keeps the capacity it reuses.

Tests that depend on a tuning knob skip when the environment overrides it (the stress settings the
suite is also run with: NANOLANCE_MORSEL_KB, NANOLANCE_TAKE_CACHE_MB, ...).
"""

from __future__ import annotations

import gc
import glob
import os

import numpy as np
import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


@pytest.fixture
def threads():
    """Set nanolance's thread count for one test, then put it back."""
    before = nanolance.get_threads()
    yield nanolance.set_threads
    nanolance.set_threads(before)


def _knob_overridden(*names):
    return any(os.environ.get(n) for n in names)


def _stats_of(fn):
    nanolance._reset_work_stats()
    result = fn()
    return nanolance._work_stats(), result


def _pages(path, column):
    from lance.file import LanceFileReader

    (data_file,) = glob.glob(f"{path}/data/*.lance")
    return LanceFileReader(data_file).metadata().columns[column].pages


def _audio(n=1_000, samples=16_000, seed=0):
    """Speech Commands' shape: one-second 16 kHz int16 clips and their word."""
    rng = np.random.default_rng(seed)
    return pa.table({
        "label": pa.array([f"word{i % 35}" for i in range(n)]),
        "waveform": pa.array([rng.integers(-3_000, 3_000, samples, dtype=np.int16) for _ in range(n)],
                             pa.list_(pa.int16())),
    })


@pytest.fixture(scope="module")
def rust_audio(lance_mod, tmp_path_factory):
    path = tmp_path_factory.mktemp("audio") / "rust.lance"
    table = _audio()
    lance_mod.write_dataset(table, str(path), data_storage_version="2.2")
    return path, table


def test_pylance_still_writes_audio_as_one_giant_page(rust_audio):
    """What the guards below are about: if pylance stops doing this, they stop exercising it."""
    path, table = rust_audio
    pages = _pages(path, 1)
    assert len(pages) == 1
    assert sum(b.size for b in pages[0].buffers) > 30_000_000


@pytest.mark.skipif(_knob_overridden("NANOLANCE_TAKE_CACHE_MB", "NANOLANCE_LIST_WINDOW_KB"), reason="tuning knob set")
def test_take_reads_the_chunks_holding_the_rows_not_the_page(rust_audio):
    path, table = rust_audio
    page_bytes = sum(b.size for b in _pages(path, 1)[0].buffers)
    rng = np.random.default_rng(1)
    nanolance.take(path, [0], columns=["waveform"])  # the page's chunk index, read once and kept
    rows = rng.choice(table.num_rows, 64, replace=False).tolist()
    stats, got = _stats_of(lambda: pa.table(nanolance.take(path, rows, columns=["waveform"])))
    assert got.column("waveform").to_pylist() == table.column("waveform").take(pa.array(rows)).to_pylist()
    requested = 64 * 16_000 * 2
    # Measured: 4.6 MB for 2.0 MB of rows (each window also decodes its first row's chunk), out of 32.5.
    assert stats["data_bytes_read"] < 4 * requested, stats
    assert stats["data_bytes_read"] < page_bytes // 4, stats
    assert stats["page_windows"] >= 8, stats


@pytest.mark.skipif(_knob_overridden("NANOLANCE_LIST_WINDOW_KB", "NANOLANCE_MORSEL_KB"), reason="tuning knob set")
@pytest.mark.parametrize("n_threads", [1, 4])
def test_a_giant_page_is_read_in_windows(rust_audio, threads, n_threads):
    path, table = rust_audio
    threads(n_threads)
    stats, got = _stats_of(lambda: pa.table(nanolance.read_table(path)))
    assert got.num_rows == table.num_rows
    assert got.column("waveform").to_pylist()[::97] == table.column("waveform").to_pylist()[::97]
    # Measured: largest read 4.2 MB, 8 windows, of a 32.5 MB page.
    assert stats["largest_read"] < 16 * 1024 * 1024, stats
    assert stats["page_windows"] >= 4, stats


def test_large_rows_get_pages_of_about_a_mebibyte(tmp_path):
    table = _audio(n=300)
    path = tmp_path / "ours.lance"
    nanolance.write_table(table, path)
    sizes = [sum(b.size for b in p.buffers) for p in _pages(path, 1)]
    # Measured: 0.90-1.04 MB each. Not one row per page (32 KB), not one page for the column (10 MB).
    assert all(512 * 1024 <= s <= 2 * 1024 * 1024 for s in sizes[:-1]), sizes
    assert len(sizes) >= 4, sizes


@pytest.mark.skipif(_knob_overridden("NANOLANCE_TAKE_CACHE_MB", "NANOLANCE_LIST_WINDOW_KB"), reason="tuning knob set")
def test_take_from_nanolances_own_audio_pages(tmp_path):
    table = _audio(n=600)
    path = tmp_path / "ours.lance"
    nanolance.write_table(table, path)
    rows = np.random.default_rng(2).choice(table.num_rows, 64, replace=False).tolist()
    nanolance.take(path, rows[:1], columns=["waveform"])
    stats, got = _stats_of(lambda: pa.table(nanolance.take(path, rows, columns=["waveform"])))
    assert got.column("waveform").to_pylist() == table.column("waveform").take(pa.array(rows)).to_pylist()
    assert stats["data_bytes_read"] < 4 * 64 * 16_000 * 2, stats


def _list_of_strings(n=300_000, seed=3):
    rng = np.random.default_rng(seed)
    lengths = rng.integers(0, 10, n)
    values = pa.array([f"tag{i % 500}" for i in range(int(lengths.sum()))])
    offsets = pa.array(np.concatenate([[0], np.cumsum(lengths)]).astype(np.int32))
    return pa.table({"c": pa.ListArray.from_arrays(offsets, values)})


@pytest.mark.skipif(_knob_overridden("NANOLANCE_MORSEL_KB"), reason="tuning knob set")
def test_a_dictionary_coded_list_column_is_read_in_parallel(tmp_path, threads):
    table = _list_of_strings()
    path = tmp_path / "tags.lance"
    nanolance.write_table(table, path)
    threads(4)
    stats, got = _stats_of(lambda: pa.table(nanolance.read_table(path)))
    assert got.column("c").to_pylist()[::1001] == table.column("c").to_pylist()[::1001]
    # Small on disk (~1.3 MB), 19 ms of decoding on one core: worth cutting. Measured: 8 row ranges.
    assert stats["fragment_reads"] == 1 and stats["read_morsels"] >= 4, stats


@pytest.mark.skipif(_knob_overridden("NANOLANCE_MORSEL_KB"), reason="tuning knob set")
def test_a_small_column_is_not_split(tmp_path, threads):
    path = tmp_path / "small.lance"
    nanolance.write_table(pa.table({"c": pa.array(np.arange(100_000, dtype=np.int32))}), path)
    threads(4)
    stats, _ = _stats_of(lambda: pa.table(nanolance.read_table(path)))
    assert stats["read_morsels"] == stats["fragment_reads"] == 1, stats


def test_a_column_that_is_most_of_the_data_is_not_buffered(tmp_path, threads):
    """COCO's shape: small id and label columns beside the images."""
    rng = np.random.default_rng(4)
    n = 1_500
    table = pa.table({
        "id": pa.array(range(n), pa.int64()),
        "label": pa.array([f"class{i % 80}" for i in range(n)]),
        "image": pa.array([rng.bytes(20_000) for _ in range(n)], pa.binary()),
    })
    threads(4)
    stats, _ = _stats_of(lambda: nanolance.write_table(table, tmp_path / "images.lance"))
    assert stats["parallel_column_writes"] == 0 and stats["write_buffered_bytes"] == 0, stats
    assert pa.table(nanolance.read_table(tmp_path / "images.lance")).to_pydict() == table.to_pydict()


def test_columns_of_similar_size_are_encoded_side_by_side(tmp_path, threads):
    rng = np.random.default_rng(5)
    n = 200_000
    table = pa.table({
        "a": pa.array(rng.integers(0, 1 << 40, n)),
        "b": pa.array(rng.random(n)),
        "c": pa.array([f"user{i}" for i in range(n)]),
    })
    threads(4)
    stats, _ = _stats_of(lambda: nanolance.write_table(table, tmp_path / "mixed.lance"))
    assert stats["parallel_column_writes"] == 1 and stats["write_buffered_bytes"] > 0, stats
    threads(1)
    stats, _ = _stats_of(lambda: nanolance.write_table(table, tmp_path / "mixed1.lance"))
    assert stats["parallel_column_writes"] == 0, stats


@pytest.mark.skipif(_knob_overridden("NANOLANCE_BUFFER_POOL_MB", "NANOLANCE_MORSEL_KB"), reason="tuning knob set")
def test_a_repeated_parallel_read_reuses_its_buffers(tmp_path, threads):
    rng = np.random.default_rng(6)
    path = tmp_path / "ints.lance"
    nanolance.write_table(pa.table({"c": pa.array(rng.integers(0, 1 << 60, 2_000_000))}), path)
    threads(4)
    for _ in range(2):  # the first read fills the pool
        got = pa.table(nanolance.read_table(path))
        del got
        gc.collect()
    stats, got = _stats_of(lambda: pa.table(nanolance.read_table(path)))
    assert got.num_rows == 2_000_000
    assert stats["buffer_pool_hits"] >= 1 and stats["buffer_pool_misses"] == 0, stats


@pytest.mark.skipif(_knob_overridden("NANOLANCE_TAKE_CACHE_MB"), reason="tuning knob set")
def test_a_small_column_is_decoded_once_for_many_takes(tmp_path):
    table = _list_of_strings(n=20_000)
    path = tmp_path / "tags.lance"
    nanolance.write_table(table, path)
    nanolance.take(path, [1, 2, 3])
    stats, got = _stats_of(lambda: pa.table(nanolance.take(path, [4, 5, 6])))
    assert got.column("c").to_pylist() == table.column("c").take(pa.array([4, 5, 6])).to_pylist()
    assert stats["take_cache_hits"] >= 1 and stats["data_bytes_read"] == 0, stats


@pytest.mark.parametrize("writer", ["nanolance", "rust"])
def test_take_of_images_reads_just_those_images(lance_mod, tmp_path, writer):
    rng = np.random.default_rng(7)
    n = 800
    table = pa.table({"image": pa.array([rng.bytes(20_000) for _ in range(n)], pa.binary())})
    path = tmp_path / "images.lance"
    if writer == "nanolance":
        nanolance.write_table(table, path)
    else:
        lance_mod.write_dataset(table, str(path), data_storage_version="2.2")
    rows = list(range(0, n, 20))
    nanolance.take(path, [1])
    stats, got = _stats_of(lambda: pa.table(nanolance.take(path, rows)))
    assert got.column("image").to_pylist() == table.column("image").take(pa.array(rows)).to_pylist()
    # Measured (nanolance's file): 808 KB for 800 KB of images -- each image plus its index entry.
    assert stats["data_bytes_read"] < 1.25 * len(rows) * 20_000 + 65_536, stats
