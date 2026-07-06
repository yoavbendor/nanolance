"""Lance binding parity and full-cycle tests."""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


def test_lance_write_read_roundtrip(sample_table, tmp_path):
    path = tmp_path / "sample.lance"
    nanolance.write_table(sample_table, path, compression=True)
    back = pa.table(nanolance.read_table(path))
    assert back.equals(sample_table)


def test_lance_nullable_roundtrip(nullable_table, tmp_path):
    path = tmp_path / "nulls.lance"
    table = nullable_table.drop(["nul"])
    nanolance.write_table(
        table,
        path,
        options=nanolance.WriteOptions(ignore_nullability=True, compression=True),
    )
    back = pa.table(nanolance.read_table(path))
    assert back.column("ctrl").equals(table.column("ctrl"))
    assert back.num_rows == table.num_rows


def test_lance_pylance_reader(pylance_interop_table, tmp_path):
    """Datasets written by nanolance must be readable by pylance (import lance)."""
    lance = require_pylance()
    path = tmp_path / "stock.lance"
    nanolance.write_table(pylance_interop_table, path, compression=False)

    back = lance.dataset(str(path)).to_table()
    assert back.to_pydict() == pylance_interop_table.to_pydict()


def test_lance_polars_cycle(sample_table, tmp_path):
    polars = pytest.importorskip("polars")
    path = tmp_path / "pl.lance"
    nanolance.write_table(sample_table, path, compression=True)
    exported = nanolance.read_table(path)
    pl_back = polars.from_arrow(pa.table(exported))
    pl_orig = polars.from_arrow(sample_table)
    assert pl_back.equals(pl_orig)
