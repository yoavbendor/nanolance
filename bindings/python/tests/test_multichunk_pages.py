"""Fixed-width columns are written as MiniBlock pages of many chunks (~512 KiB of chunks per page).

They used to be written one chunk per page -- ~2,000 pages of ~2.5 KB for a 2M-row int64 column --
which Rust Lance read ~5x slower than its own files. What has to hold now:

  * every fixed-width path (flat, bit-packed, bool, byte-stream-split + zstd, fixed-size lists,
    decimals, nullable columns) writes pages of more than one chunk, and far fewer pages;
  * both readers return the data unchanged, including row ranges that start and end mid-page and
    straddle a page boundary, and after deletions;
  * the last chunk of a column (short, not a power of two) and a column that fits in one chunk are
    still fine.
"""

from __future__ import annotations

import glob
import math

import numpy as np
import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance

N = 600_001  # not a multiple of any chunk size, so every column ends in a short chunk


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _columns(n):
    rng = np.random.default_rng(7)
    valid = rng.random(n) > 0.1
    dim = 16
    return {
        "i64": pa.array(np.arange(n, dtype=np.int64) * 7 - 3),
        "i32_random": pa.array(rng.integers(-(2**31), 2**31 - 1, n, dtype=np.int32)),
        "u8": pa.array(rng.integers(0, 255, n, dtype=np.uint8)),
        "f64": pa.array(np.sin(np.arange(n) / 100.0)),
        "f32": pa.array(rng.random(n, dtype=np.float32)),
        "flag": pa.array(rng.random(n) < 0.3),
        "maybe_i64": pa.array(np.arange(n, dtype=np.int64), mask=~valid),
        "maybe_f64": pa.array(rng.random(n), mask=~valid),
        "maybe_flag": pa.array(rng.random(n) < 0.5, mask=~valid),
        "ts": pa.array(np.arange(n, dtype=np.int64) * 1_000_003, pa.timestamp("us")),
        "dec": pa.array([None if i % 17 == 0 else i * 10 - 5 for i in range(n)], pa.decimal128(18, 2)),
        "uuid": pa.array([rng.bytes(16) for _ in range(n)], pa.binary(16)),
        "vec": pa.FixedSizeListArray.from_arrays(pa.array(rng.random(n * dim, dtype=np.float32)), dim),
    }


def _pages_per_column(path):
    from lance.file import LanceFileReader

    (data_file,) = glob.glob(f"{path}/data/*.lance")
    return [len(c.pages) for c in LanceFileReader(data_file).metadata().columns]


@pytest.mark.parametrize("compression", [False, True])
@pytest.mark.parametrize("structural_encoding", [True, False])
def test_fixed_width_pages_hold_many_chunks(lance_mod, tmp_path, compression, structural_encoding):
    table = pa.table(_columns(N))
    path = tmp_path / "multi.lance"
    nanolance.write_table(pa.Table.from_batches(table.to_batches(max_chunksize=65_536)), path,
                          compression=compression, structural_encoding=structural_encoding)
    pages = _pages_per_column(path)
    # One chunk is at most 32 KiB, so one chunk per page would mean >= bytes / 32 KiB pages. The
    # widest column (vec, 64 bytes a row) is ~38 MB: ~1,200 single-chunk pages, ~75 now.
    for name, count in zip(table.column_names, pages):
        column_bytes = table.column(name).nbytes
        assert count <= math.ceil(column_bytes / (512 << 10)) + 2, (name, count)
    assert max(pages) > 1, "the columns are big enough to need several pages"

    expected = table.to_pydict()
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == expected
    assert lance_mod.dataset(str(path)).to_table().to_pydict() == expected


def test_row_ranges_across_page_boundaries(lance_mod, tmp_path):
    table = pa.table({k: v for k, v in _columns(N).items() if k in ("i64", "f64", "maybe_f64", "flag", "vec")})
    path = tmp_path / "ranges.lance"
    nanolance.write_table(table, path)
    # 512 KiB of int64 is ~65,000 rows; float64 (flat) is cut into 16 KiB chunks of 2,048 rows.
    for offset, length in ((0, 1), (65_500, 100), (131_000, 200), (131_071, 2), (262_100, 70_000), (N - 5, 5), (1_000, N - 2_000)):
        got = pa.table(nanolance.read_table(path, offset=offset, length=length))
        assert got.to_pydict() == table.slice(offset, length).to_pydict(), (offset, length)
    ds = lance_mod.dataset(str(path))
    idx = [0, 2047, 2048, 65_535, 65_536, 131_071, 131_072, 400_000, N - 1]
    assert ds.take(idx).to_pydict() == table.take(idx).to_pydict()


def test_deletions_over_multichunk_pages(lance_mod, tmp_path):
    table = pa.table({k: v for k, v in _columns(200_000).items() if k in ("i64", "maybe_i64", "f32", "flag")})
    path = tmp_path / "del.lance"
    nanolance.write_table(table, path)
    lance_mod.dataset(str(path)).delete("i64 % 5 = 1")
    expected = lance_mod.dataset(str(path)).to_table().to_pydict()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected
    assert len(expected["i64"]) == 160_000


@pytest.mark.parametrize("rows", [1, 2, 1023, 1024, 1025, 2048, 4097])
def test_small_columns(lance_mod, tmp_path, rows):
    table = pa.table({k: v for k, v in _columns(rows).items() if k not in ("uuid",)})
    path = tmp_path / f"small{rows}.lance"
    nanolance.write_table(table, path)
    assert pa.table(nanolance.read_table(path)).to_pydict() == table.to_pydict()
    assert lance_mod.dataset(str(path)).to_table().to_pydict() == table.to_pydict()
