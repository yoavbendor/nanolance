"""Reading pylance-written NULLABLE columns, across the encodings Lance picks by itself.

Nulls are the axis where "nanolance reads stock Lance" is most easily overstated. Lance does not have
one definition-level encoding, it has several, and which one a column gets depends on the row count
and the null pattern -- not on anything the writer is asked for. A suite that tests one size and one
pattern will pass while common datasets fail.

This walks the cross-product directly. Every cell is compared against **pylance's own read** of the
same dataset, so the oracle is Lance, not our expectations.

Every cell passes. It did not when this file was written: the sweep found three separate failures --
`InlineBitpacking(16)` levels, a chunk header whose shape depends on the page descriptor, and
definition levels spanning more than one FastLanes block. All three are fixed; see docs/PROGRESS.md
for the on-disk grammar behind each.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture
def lance_mod():
    return require_pylance()


VALUES = {
    "int64": lambda i: i,
    "float64": lambda i: i * 0.5,
    "string": lambda i: f"v{i}",
    "binary": lambda i: bytes([i % 251]) * ((i % 5) + 1),
    "bool": lambda i: i % 3 == 0,
}

# Null patterns chosen for the encoder, not for variety: a scattered pattern and a run-shaped one
# take different paths (bitpacked levels vs. RLE levels), and all-null / no-null are the edges.
PATTERNS = {
    "every7": lambda i: i % 7 == 0,
    "first_half": lambda i: i < 50,
    "sparse": lambda i: i % 97 == 0,
    "all_null": lambda i: True,
    "none": lambda i: False,
}

# 1024 and 1025 are deliberate: exactly one FastLanes block, and one more than one, which is where a
# bool page first puts more than 1024 values into a single chunk. 20000 reaches a chunk whose tail is
# stored raw rather than packed.
SIZES = (100, 200, 400, 999, 1024, 1025, 3000, 20_000)


def _dataset(lance_mod, tmp_path, n, type_name, pattern_name):
    path = str(tmp_path / f"{n}_{type_name}_{pattern_name}.lance")
    value = VALUES[type_name]
    is_null = PATTERNS[pattern_name]
    table = pa.table(
        {"c": pa.array([None if is_null(i) else value(i) for i in range(n)],
                       type=pa.type_for_alias(type_name))}
    )
    lance_mod.write_dataset(table, path)
    return path


@pytest.mark.parametrize("n", SIZES)
@pytest.mark.parametrize("type_name", sorted(VALUES))
@pytest.mark.parametrize("pattern_name", sorted(PATTERNS))
def test_nullable_column_matches_pylance(lance_mod, tmp_path, n, type_name, pattern_name):
    path = _dataset(lance_mod, tmp_path, n, type_name, pattern_name)
    expected = lance_mod.dataset(path).to_table()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()


def test_inline_bitpacked_definition_levels(lance_mod, tmp_path):
    """The sizes that used to be unreadable, called out on their own.

    For a nullable string column, Lance encodes the definition levels as InlineBitpacking(16) --
    a FastLanes block whose bit width is the buffer's first u16 rather than a number in the page
    descriptor -- somewhere between a couple of hundred and a couple of thousand rows. 100 and 2000
    do not hit it; 200 through 1000 do. It was refused by name, so an ordinary pylance dataset of a
    few hundred rows read as an error.
    """
    for n in (100, 150, 200, 300, 500, 700, 1000, 1024, 2000):
        path = _dataset(lance_mod, tmp_path, n, "string", "every7")
        expected = lance_mod.dataset(path).to_table()
        got = pa.table(nanolance.read_table(path))
        assert got.to_pydict() == expected.to_pydict(), f"n={n}"


def test_definition_levels_spanning_several_fastlanes_blocks(lance_mod, tmp_path):
    """A bool column puts more than 1024 values in one chunk, so its levels are several blocks.

    Bools pack a bit per value, so a chunk sized by bytes holds far more of them than of anything
    else -- 1025 at the first size that overflows. The level buffer is then a whole packed FastLanes
    block followed by a raw u16 tail (128 + 2 bytes), and the decoder handled exactly one block.
    """
    for n in (1024, 1025, 1026, 2048, 3000, 20_000):
        path = _dataset(lance_mod, tmp_path, n, "bool", "every7")
        expected = lance_mod.dataset(path).to_table()
        assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict(), f"n={n}"


def test_run_shaped_nulls_on_a_float_column(lance_mod, tmp_path):
    """Runs of nulls make Lance RLE both the values and the levels.

    That gives the page two value buffers AND a definition-level slot, and sets has_large_chunk so the
    buffer sizes are u32 -- a chunk header three fields longer than the fixed eight bytes the decoder
    assumed. It read the definition block 8 bytes early and failed on the sizes it found there.
    """
    for n in (200, 400, 999, 3000):
        path = _dataset(lance_mod, tmp_path, n, "float64", "first_half")
        expected = lance_mod.dataset(path).to_table()
        assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict(), f"n={n}"
