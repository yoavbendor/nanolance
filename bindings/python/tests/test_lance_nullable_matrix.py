"""Reading pylance-written NULLABLE columns, across the encodings Lance picks by itself.

Nulls are the axis where "nanolance reads stock Lance" is most easily overstated. Lance does not have
one definition-level encoding, it has several, and which one a column gets depends on the row count
and the null pattern -- not on anything the writer is asked for. A suite that tests one size and one
pattern will pass while common datasets fail.

This walks the cross-product directly. Every cell is compared against **pylance's own read** of the
same dataset, so the oracle is Lance, not our expectations.

The two cells that do not work yet are asserted to fail *with their specific message*, not skipped.
A skip rots silently; an assertion that a gap still exists fails the moment it is fixed, which is
exactly when someone should come back and delete it.
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

# 1025 is deliberate: one more than a FastLanes block, which is where a bool page first puts more
# than 1024 values in a single chunk.
SIZES = (200, 400, 999, 1025, 3000)


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


# Known, reproducible read gaps. Each entry is (predicate, expected message fragment); see
# docs/PROGRESS.md for the decoded on-disk grammar behind both.
KNOWN_GAPS = (
    # A bool page packs more than 1024 values into one chunk, so its definition levels span several
    # FastLanes blocks; the decoder handles exactly one.
    (lambda n, t, p: t == "bool" and n > 1024 and p not in ("all_null", "none"),
     "definition-level chunk covers more than one FastLanes block"),
    # A run-shaped null pattern on a float column makes Lance RLE both the values and the levels,
    # which gives the chunk two value buffers and a chunk header longer than the 8 bytes the decoder
    # assumes.
    (lambda n, t, p: t == "float64" and p == "first_half" and n in (200, 400),
     "rle chunk buffer sizes invalid"),
)


def _expected_gap(n, type_name, pattern_name):
    for predicate, message in KNOWN_GAPS:
        if predicate(n, type_name, pattern_name):
            return message
    return None


@pytest.mark.parametrize("n", SIZES)
@pytest.mark.parametrize("type_name", sorted(VALUES))
@pytest.mark.parametrize("pattern_name", sorted(PATTERNS))
def test_nullable_column_matches_pylance(lance_mod, tmp_path, n, type_name, pattern_name):
    path = _dataset(lance_mod, tmp_path, n, type_name, pattern_name)
    expected = lance_mod.dataset(path).to_table()
    gap = _expected_gap(n, type_name, pattern_name)

    if gap is not None:
        with pytest.raises(Exception) as excinfo:
            pa.table(nanolance.read_table(path))
        assert gap in str(excinfo.value), (
            f"the known gap for n={n} {type_name}/{pattern_name} now fails differently: "
            f"{excinfo.value}. If it was fixed, delete its entry from KNOWN_GAPS."
        )
        return

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
