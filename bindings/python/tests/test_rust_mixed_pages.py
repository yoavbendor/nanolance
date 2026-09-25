"""Columns whose pages Rust Lance encoded differently from one another.

Rust Lance picks an encoding per page. Two shapes the benchmark matrix turned up, both of which
nanolance's fixed-width reader refused because it decoded every page with the first page's plan:

  * a float64 column written in 64K-row batches bit-packs its full pages and writes the short last
    page Flat ("bitpacked chunk has invalid bit width");
  * a nullable column stores definition levels only on pages that have a null, so a page without
    one carries none ("column declares definition levels but a chunk carries none") -- whether the
    null-free pages come before the first null or after it.
"""

from __future__ import annotations

import glob
import re

import numpy as np
import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _page_encodings(path):
    from lance.file import LanceFileReader

    return [str(p.encoding) for data_file in sorted(glob.glob(f"{path}/data/*.lance"))
            for p in LanceFileReader(data_file).metadata().columns[0].pages]


def _write_rust(lance_mod, table, path, batch=65_536):
    lance_mod.write_dataset(pa.Table.from_batches(table.to_batches(max_chunksize=batch)), str(path),
                            data_storage_version="2.2")


def _same(path, lance_mod, table):
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == table.to_pydict()
    assert got.equals(lance_mod.dataset(str(path)).to_table())


def test_bitpacked_pages_then_a_flat_one(lance_mod, tmp_path):
    # tools/bench_matrix.py's float64_smooth, where it turned up: pylance writes 14 bit-packed pages
    # and a Flat last one. (At a few hundred thousand rows every page is bit-packed.)
    n = 2_000_000
    table = pa.table({"x": pa.array(np.sin(np.arange(n) * 0.001) * 100.0)})
    path = tmp_path / "f64.lance"
    _write_rust(lance_mod, table, path)
    kinds = {re.search(r"(InlineBitpacking|Flat)\(", e).group(1) for e in _page_encodings(path)}
    assert kinds == {"InlineBitpacking", "Flat"}, f"pylance no longer mixes page encodings here: {kinds}"
    _same(path, lance_mod, table)
    got = pa.table(nanolance.read_table(path, offset=1_950_000, length=30_000))
    assert got.to_pydict() == table.slice(1_950_000, 30_000).to_pydict()


@pytest.mark.parametrize("nulls_where", ["late", "early", "middle"])
@pytest.mark.parametrize("dtype", [pa.int64(), pa.float64(), pa.int32()])
def test_pages_without_levels_in_a_nullable_column(lance_mod, tmp_path, nulls_where, dtype):
    n = 1_000_000  # pylance cuts it into six pages; a few hundred thousand rows make one
    values = np.arange(n) * 3 + 7
    mask = np.zeros(n, bool)
    region = {"late": slice(800_000, n), "early": slice(0, 150_000), "middle": slice(400_000, 600_000)}[nulls_where]
    mask[region] = np.arange(n)[region] % 7 == 0
    table = pa.table({"x": pa.array(values, type=dtype, mask=mask)})
    path = tmp_path / f"{nulls_where}.lance"
    _write_rust(lance_mod, table, path)
    layers = [("NullableItem" in e) for e in _page_encodings(path)]
    assert any(layers) and not all(layers), "expected pages both with and without definition levels"
    _same(path, lance_mod, table)
    assert pa.table(nanolance.read_table(path)).column("x").null_count == int(mask.sum())
