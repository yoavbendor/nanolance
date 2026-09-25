"""Large string and binary values: images, audio clips, documents.

A plain (not dictionary-, not FSST-encoded) variable-width column used to be written as single-chunk
MiniBlock pages whose chunk header records the value bytes in a u16. One value over ~32 KB -- a JPEG,
a second of audio -- then produced a file Rust Lance could not read ("the offset + length of the
sliced Buffer cannot exceed the existing length"), and over 64 KB one nanolance could not read either.
Such columns now go through the multi-chunk page writer, whose chunks carry u32 sizes. What has to
hold, for every size from a few KB to several MB, binary and large_binary and utf8, with and without
nulls, with compression and with structural encodings off:

  * both readers return the values unchanged, whole and as row ranges;
  * a column whose pages differ in having nulls reads back with the right validity (a page without
    a null carries no definition levels, whichever reader wrote it).
"""

from __future__ import annotations

import numpy as np
import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance

SIZES = [1_000, 16_000, 32_800, 66_000, 250_000, 3_000_000]


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _values(kind, size, n, nulls, seed=1):
    rng = np.random.default_rng(seed)
    if kind in (pa.utf8(), pa.large_utf8()):
        vals = [rng.bytes(size // 2).hex() for _ in range(n)]  # hex: not FSST material
    else:
        vals = [rng.bytes(size) for _ in range(n)]
    if nulls:
        vals[1] = None
        vals[n - 2] = None
    return vals


def _check(lance_mod, path, table, offset=None, length=None):
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == table.to_pydict()
    assert lance_mod.dataset(str(path)).to_table().to_pydict() == table.to_pydict()
    if offset is not None:
        part = pa.table(nanolance.read_table(path, offset=offset, length=length))
        assert part.to_pydict() == table.slice(offset, length).to_pydict()


@pytest.mark.parametrize("size", SIZES)
@pytest.mark.parametrize("kind", [pa.binary(), pa.large_binary(), pa.utf8()])
@pytest.mark.parametrize("nulls", [False, True])
def test_large_values_round_trip(lance_mod, tmp_path, size, kind, nulls):
    n = 12 if size >= 1_000_000 else 40
    table = pa.table({"id": pa.array(range(n)), "v": pa.array(_values(kind, size, n, nulls), kind)})
    path = tmp_path / "v.lance"
    nanolance.write_table(table, path)
    _check(lance_mod, path, table, offset=3, length=n - 5)


@pytest.mark.parametrize("compression", [True, False])
@pytest.mark.parametrize("structural_encoding", [True, False])
def test_large_values_under_every_write_mode(lance_mod, tmp_path, compression, structural_encoding):
    """zstd pages hold only values that fit a chunk; a column with a bigger one takes the multi-chunk
    pages, and plain mode writes them without dictionary or FSST."""
    sizes = [100, 70_000, 500, 1_200_000, 40]  # small and large in one column
    rng = np.random.default_rng(2)
    table = pa.table({"v": pa.array([rng.bytes(s) for s in sizes * 5] + [None], pa.binary())})
    path = tmp_path / "m.lance"
    nanolance.write_table(table, path, compression=compression, structural_encoding=structural_encoding)
    _check(lance_mod, path, table, offset=4, length=17)


def test_an_image_table(lance_mod, tmp_path):
    """The shape of an image dataset: ids, JPEG-sized blobs, labels and captions."""
    rng = np.random.default_rng(3)
    n = 300
    table = pa.table({
        "id": pa.array(range(n), pa.int64()),
        "image": pa.array([rng.bytes(int(s)) for s in rng.integers(20_000, 300_000, n)], pa.binary()),
        "label": pa.array([f"class-{i % 80}" for i in range(n)]),
        "caption": pa.array([f"a photo of thing {i} next to thing {i * 7 % 13}" for i in range(n)]),
        "width": pa.array(rng.integers(200, 640, n), pa.int32()),
    })
    path = tmp_path / "images.lance"
    nanolance.write_table(table, path)
    _check(lance_mod, path, table, offset=100, length=77)


@pytest.mark.parametrize("compressible", [False, True])
@pytest.mark.parametrize("nulls", [False, True])
@pytest.mark.parametrize("kind", [pa.binary(), pa.large_binary(), pa.utf8()])
def test_reading_rusts_per_value_compressed_pages(lance_mod, tmp_path, compressible, nulls, kind):
    """pylance writes large values as FullZip pages with each value zstd-compressed on its own
    (General{zstd, Variable}); nanolance refused them as "unsupported page layout"."""
    rng = np.random.default_rng(4)
    n = 60
    if compressible:
        raw = [(f"row {i} " * (2_000 + i * 50)).encode() for i in range(n)]
    else:
        raw = [rng.bytes(int(s)) for s in rng.integers(300, 90_000, n)]
    vals = [r.hex() if kind == pa.utf8() else r for r in raw]
    if nulls:
        vals[5] = None
        vals[40] = None
    table = pa.table({"v": pa.array(vals, kind)})
    path = tmp_path / "rust.lance"
    lance_mod.write_dataset(table, str(path), data_storage_version="2.2")
    from lance.file import LanceFileReader
    import glob
    (data_file,) = glob.glob(f"{path}/data/*.lance")
    encoding = str(LanceFileReader(data_file).metadata().columns[0].pages[0].encoding)
    assert "FullZip" in encoding, "pylance no longer writes these as FullZip pages"
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == table.to_pydict()
    part = pa.table(nanolance.read_table(path, offset=7, length=40))
    assert part.to_pydict() == table.slice(7, 40).to_pydict()
