"""Choosing an encoding by whether it actually pays, rather than by type.

nanolance used to bitpack every integer column, unconditionally, because integers are bitpackable.
That is a statement about the type, not about the data, and it is wrong twice over:

  * A FastLanes chunk always covers 1024 values, padded. A column with fewer rows than that is
    inflated to a full block -- an 8-row int64 column occupied 528 bytes of page payload where flat
    needs 64.
  * Values that need the full width pack to exactly their own size, plus a width word per chunk. So a
    column of random ids or hashes comes out LARGER bitpacked, and pays a FastLanes transpose on
    every read to get back what it started with.

The rule is now the one stock Lance uses (`rust/lance-encoding/src/compression.rs`): estimate the
packed size per chunk and only bitpack when it is strictly smaller than raw. These tests pin both
directions -- the columns that must stop being bitpacked, and the ones that must keep being.
"""

from __future__ import annotations

import os
import random

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


def _size(path) -> int:
    return sum(
        os.path.getsize(os.path.join(root, f))
        for root, _, files in os.walk(path)
        for f in files
    )


def _write(table, path, **kwargs):
    nanolance.write_table(table, path, **kwargs)
    return _size(path)


def test_a_small_integer_column_is_not_inflated_to_a_full_fastlanes_block(tmp_path):
    """Eight int64 values are 64 bytes. They must not cost a padded 1024-value block."""
    table = pa.table({"k": pa.array([4, 4, 4, 7, 7, 9, 2, 5], type=pa.int64())})
    size = _write(table, tmp_path / "small.lance")

    # Measured: 562 bytes now, 1041 when the column was padded to a full block (528 bytes of page
    # payload for 64 bytes of data). The bound sits between the two rather than at a round number,
    # so the test actually fails if the padding comes back -- a loose bound passed either way.
    assert size < 750, f"an 8-row int64 column produced a {size}-byte dataset"
    assert pa.table(nanolance.read_table(tmp_path / "small.lance")).to_pydict() == table.to_pydict()


def test_incompressible_integers_are_not_bitpacked(tmp_path):
    """Random uint64 ids need all 64 bits, so bitpacking can only add a width word per chunk."""
    random.seed(7)
    n = 50_000
    table = pa.table({"id": pa.array([random.getrandbits(64) for _ in range(n)], type=pa.uint64())})

    auto = _write(table, tmp_path / "auto.lance")
    flat = _write(table, tmp_path / "flat.lance", structural_encoding=False)

    assert auto <= flat, (
        f"structural encoding made an incompressible column larger: {auto} vs {flat} bytes flat"
    )
    assert pa.table(nanolance.read_table(tmp_path / "auto.lance")).to_pydict() == table.to_pydict()


def test_narrow_integers_are_still_bitpacked(tmp_path):
    """The guard in the other direction: a column that bitpacking genuinely shrinks must keep it.

    Without this, 'only bitpack when it pays' could quietly become 'never bitpack' and every test
    above would still pass.
    """
    n = 50_000
    table = pa.table({"small": pa.array([i % 100 for i in range(n)], type=pa.int64())})

    auto = _write(table, tmp_path / "auto.lance")
    flat = _write(table, tmp_path / "flat.lance", structural_encoding=False)

    assert auto < flat * 0.5, (
        f"a 7-bit-wide column should pack far below flat, got {auto} vs {flat} bytes"
    )
    assert pa.table(nanolance.read_table(tmp_path / "auto.lance")).to_pydict() == table.to_pydict()


def test_stock_lance_still_reads_both_choices(tmp_path):
    """Whichever way the choice goes, the file stays a Lance file."""
    lance_mod = require_pylance()
    random.seed(11)
    n = 5_000
    table = pa.table(
        {
            "id": pa.array([random.getrandbits(64) for _ in range(n)], type=pa.uint64()),
            "small": pa.array([i % 50 for i in range(n)], type=pa.int64()),
            "tiny": pa.array([1, 2, 3] + [0] * (n - 3), type=pa.int64()),
        }
    )
    path = tmp_path / "mixed.lance"
    nanolance.write_table(table, path)
    assert lance_mod.dataset(str(path)).to_table().to_pydict() == table.to_pydict()
