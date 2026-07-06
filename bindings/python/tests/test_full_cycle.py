"""Full Python -> nanolance -> Python cycles."""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


def test_lance_full_cycle(sample_table, pylance_interop_table, tmp_path):
    path = tmp_path / "cycle.lance"

    nanolance.write_table(sample_table, path, compression=True)
    native = pa.table(nanolance.read_table(path))
    assert native.equals(sample_table)

    lance = require_pylance()
    interop_path = tmp_path / "interop.lance"
    nanolance.write_table(pylance_interop_table, interop_path, compression=False)
    stock = lance.dataset(str(interop_path)).to_table()
    assert stock.to_pydict() == pylance_interop_table.to_pydict()

    path2 = tmp_path / "cycle2.lance"
    nanolance.write_table(stock, path2, compression=False)
    again = pa.table(nanolance.read_table(path2))
    assert again.to_pydict() == pylance_interop_table.to_pydict()
