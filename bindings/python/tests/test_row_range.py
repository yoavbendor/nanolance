"""Row-range reads: `read_table(..., offset=, length=)` and the same on `open_stream`.

The oracle throughout is `full_read.slice(offset, length)` -- a range must return *exactly* what
slicing a full read returns, for every encoding and every alignment. That is a cheap and total
oracle, so these tests use it rather than hand-written expectations.

Two alignments matter and are covered deliberately:

  - `offset % 8 != 0`, because the validity bitmap is LSB-first bits and any such offset shifts
    every bit. Seven fragment boundaries out of eight land on one.
  - offsets that fall inside, on, and either side of a fragment boundary, because that is where the
    plan switches from "skip this file" to "trim this file" to "take it whole".
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance
from tests.test_write_encoding_matrix import SHAPES

N = 20_000
ROWS_PER_FRAGMENT = 6_000  # -> fragments of 6000, 6000, 6000, 2000


def _read_range(path, offset, length, columns=None, stream=False):
    if stream:
        handle = nanolance.open_stream(path, columns, offset=offset, length=length)
        return pa.RecordBatchReader.from_stream(handle).read_all()
    return pa.table(nanolance.read_table(path, columns, offset=offset, length=length))


def _expected(full, offset, length):
    return full.slice(offset, full.num_rows - offset if length is None else length)


@pytest.fixture(scope="module")
def every_shape(tmp_path_factory):
    """One dataset holding every encoding, written across four fragments."""
    combined = pa.table({name: table.column("c") for name, table, _ in SHAPES})
    path = tmp_path_factory.mktemp("range") / "every_shape.lance"
    with nanolance.LanceWriter(path, max_rows_per_fragment=ROWS_PER_FRAGMENT) as writer:
        for batch in combined.to_batches(max_chunksize=2_000):
            writer.write_batch(batch)
    return path, combined


# Boundaries are at 6000 / 12000 / 18000. These probe every interesting position relative to them,
# and most are not multiples of 8.
SWEEP = [
    (0, 1), (0, 7), (0, 8), (0, 9), (0, 5_999), (0, 6_000), (0, 6_001), (0, None),
    (1, 1), (1, 6_000), (3, 11_999), (7, 8), (9, 17), (13, 6_013),
    (5_999, 1), (5_999, 2), (6_000, 1), (6_001, 1),
    (5_995, 10), (11_995, 10), (17_995, 10),
    (6_000, 6_000), (6_000, 12_000), (12_000, None), (18_000, None),
    (19_999, 1), (19_999, 5), (20_000, 0), (0, 0), (12_345, 0),
    (0, 20_000), (0, 99_999),  # a length past the end is clamped, not an error
]


@pytest.mark.parametrize("offset, length", SWEEP, ids=[f"{o}+{l}" for o, l in SWEEP])
def test_range_matches_a_sliced_full_read(every_shape, offset, length):
    """The headline contract, over every encoding at once."""
    path, _ = every_shape
    full = pa.table(nanolance.read_table(path))
    expected = _expected(full, offset, length)
    assert _read_range(path, offset, length).to_pydict() == expected.to_pydict()


@pytest.mark.parametrize("offset, length", SWEEP, ids=[f"{o}+{l}" for o, l in SWEEP])
def test_streamed_range_matches_a_sliced_full_read(every_shape, offset, length):
    path, _ = every_shape
    full = pa.table(nanolance.read_table(path))
    expected = _expected(full, offset, length)
    assert _read_range(path, offset, length, stream=True).to_pydict() == expected.to_pydict()


@pytest.mark.parametrize("name, table, _encoding", SHAPES, ids=[s[0] for s in SHAPES])
def test_every_encoding_slices_correctly_across_a_fragment_boundary(name, table, _encoding, tmp_path):
    """Each encoding on its own, at an offset that is neither byte- nor fragment-aligned.

    The combined dataset above covers these too, but a failure there names one dataset; a failure
    here names the encoding.
    """
    path = tmp_path / f"{name}.lance"
    with nanolance.LanceWriter(path, max_rows_per_fragment=ROWS_PER_FRAGMENT) as writer:
        for batch in table.to_batches(max_chunksize=2_000):
            writer.write_batch(batch)

    full = pa.table(nanolance.read_table(path))
    for offset, length in ((5_997, 11), (3, 6_005), (11_999, 2), (13, 19_987)):
        assert _read_range(path, offset, length).to_pydict() == _expected(full, offset, length).to_pydict()


def test_range_composes_with_projection(every_shape):
    path, _ = every_shape
    full = pa.table(nanolance.read_table(path, ["int_bitpack", "null_str_dict"]))
    got = _read_range(path, 6_001, 5_000, columns=["int_bitpack", "null_str_dict"])
    assert got.column_names == full.column_names
    assert got.to_pydict() == full.slice(6_001, 5_000).to_pydict()


def test_fragments_outside_the_range_are_never_opened(tmp_path):
    """The actual claim, tested the only way it can be: DELETE the data files the range does not
    need and check the read still succeeds.

    This is what separates a real row range from `read_table(...).slice(...)`. If the reader touched
    every fragment, this test could not pass. It also fails if the plan is merely *sloppy* about
    which files it opens -- reading one extra fragment is enough to break it.
    """
    table = pa.table(
        {
            "id": pa.array(range(N), type=pa.int64()),
            "s": pa.array([None if i % 7 == 0 else f"v{i}" for i in range(N)], type=pa.string()),
        }
    )
    path = tmp_path / "skipped.lance"
    with nanolance.LanceWriter(path, max_rows_per_fragment=ROWS_PER_FRAGMENT) as writer:
        for batch in table.to_batches(max_chunksize=2_000):
            writer.write_batch(batch)

    fragments = sorted((path / "data").glob("*.lance"))
    assert len(fragments) == 4, fragments
    expected = table.slice(12_100, 50).to_pydict()

    # Rows 12100..12150 live entirely in the third fragment. Remove the other three.
    for index in (0, 1, 3):
        fragments[index].unlink()

    assert _read_range(path, 12_100, 50).to_pydict() == expected
    assert _read_range(path, 12_100, 50, stream=True).to_pydict() == expected

    # ...and a read that DOES need a deleted fragment must fail loudly, not return short.
    with pytest.raises(RuntimeError):
        _read_range(path, 0, 50)


def test_an_offset_past_the_end_is_an_error(every_shape):
    """Clamping a too-long length is helpful; silently returning nothing for a bad offset is not.

    An offset past the end nearly always means the caller's arithmetic is wrong, and an empty table
    is the least useful way to find that out.
    """
    path, _ = every_shape
    with pytest.raises(RuntimeError, match="past the end"):
        _read_range(path, N + 1, 10)
    # The boundary itself is legal and yields nothing -- that is a real empty range, not a mistake.
    assert _read_range(path, N, None).num_rows == 0


@pytest.mark.parametrize("kwargs", [{"offset": -1}, {"length": -5}, {"offset": -1, "length": 3}])
@pytest.mark.parametrize("entry", [nanolance.read_table, nanolance.open_stream])
def test_negative_bounds_are_refused_rather_than_reinterpreted(entry, kwargs, every_shape):
    """`[-10:]` semantics would need the row count; guessing would read the wrong rows silently."""
    path, _ = every_shape
    with pytest.raises(ValueError):
        entry(path, **kwargs)


def test_range_is_keyword_only(every_shape):
    """Positional args stay (path, columns) so an existing call cannot silently become a range."""
    path, _ = every_shape
    with pytest.raises(TypeError):
        nanolance.read_table(path, None, 5)  # type: ignore[misc]


def test_stock_lance_agrees_with_the_rows_a_range_returns(every_shape):
    """Cross-check the range against a reader that is not ours.

    Our own full read is the oracle everywhere else in this file, so it is worth confirming once
    that the full read itself is right -- otherwise a decoder bug would make every range test agree
    with a wrong answer.
    """
    lance = require_pylance()
    path, combined = every_shape
    stock = lance.dataset(str(path)).to_table()
    assert _read_range(path, 6_003, 137).to_pydict() == stock.slice(6_003, 137).to_pydict()
    assert stock.to_pydict() == combined.to_pydict()
