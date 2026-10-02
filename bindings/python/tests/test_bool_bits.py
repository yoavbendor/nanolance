"""Booleans through the bit <-> byte conversions (include/nanolance/bool_bitpack.hpp).

Arrow and Lance both store booleans as LSB-first bits; nanolance holds them a byte per value in
between. Those conversions went eight values per table lookup / per multiply: this pins them to the
plain definition at every bit alignment -- Arrow slices start mid-byte, lengths are not multiples of
eight -- with and without nulls, through both readers.
"""

from __future__ import annotations

import numpy as np
import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


@pytest.mark.parametrize("offset", [0, 1, 3, 7, 8, 13])
@pytest.mark.parametrize("length", [1, 7, 8, 9, 63, 64, 65, 5_003])
@pytest.mark.parametrize("nulls", [False, True])
def test_sliced_bools_round_trip(lance_mod, tmp_path, offset, length, nulls):
    rng = np.random.default_rng(offset * 1_000 + length)
    n = offset + length + 11
    values = rng.random(n) < 0.4
    mask = (rng.random(n) < 0.2) if nulls else None
    whole = pa.array(values, pa.bool_(), mask=mask)
    part = whole.slice(offset, length)  # Arrow array with a nonzero bit offset
    table = pa.table({"b": part, "i": pa.array(range(length))})
    path = tmp_path / "b.lance"
    nanolance.write_table(table, path)
    expected = table.to_pydict()
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == expected
    assert lance_mod.dataset(str(path)).to_table().to_pydict() == expected


def test_rust_written_bools(lance_mod, tmp_path):
    rng = np.random.default_rng(9)
    table = pa.table({"b": pa.array(rng.random(100_003) < 0.5, mask=rng.random(100_003) < 0.05)})
    path = tmp_path / "rust.lance"
    lance_mod.write_dataset(table, str(path), data_storage_version="2.2")
    assert pa.table(nanolance.read_table(path)).to_pydict() == table.to_pydict()
