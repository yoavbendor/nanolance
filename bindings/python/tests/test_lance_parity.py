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


def test_nullable_schema_without_nulls_roundtrips(nullable_table, tmp_path):
    """ignore_nullability accepts a nullable *schema* -- the common pyarrow case."""
    path = tmp_path / "no_nulls.lance"
    # Same nullable-flagged schema, with every null filled in.
    table = nullable_table.drop(["nul"]).drop_null()
    assert table.num_rows > 0
    nanolance.write_table(
        table,
        path,
        options=nanolance.WriteOptions(ignore_nullability=True, compression=True),
    )
    back = pa.table(nanolance.read_table(path))
    # Every column, not just a null-free control column: this is a real round trip.
    assert back.to_pydict() == table.to_pydict()


@pytest.mark.parametrize(
    "column, arrow_type",
    [
        ("a", pa.int32()),
        ("s", pa.string()),
        ("f", pa.bool_()),
    ],
)
def test_null_values_are_refused(column, arrow_type, tmp_path):
    """A null *value* must never be written.

    nanolance writes no Lance validity information, so a null slot has nowhere to go. This used to
    copy the slot's raw bytes instead: [10, None, 30] came back as [10, 0, 30], and stock Lance read
    those wrong values without complaint. The predecessor of this test asserted only on a null-free
    control column, so it passed throughout.
    """
    values = [None if i == 1 else _sample_value(arrow_type, i) for i in range(4)]
    table = pa.table({column: pa.array(values, type=arrow_type)})
    path = tmp_path / "nulls.lance"

    with pytest.raises(RuntimeError) as excinfo:
        nanolance.write_table(table, path)

    message = str(excinfo.value)
    # The message has to be actionable: which column, which row, and what to do.
    assert column in message
    assert "null at row 1" in message
    assert "fill_null" in message
    # A refused write must not leave a half-built dataset a reader could pick up.
    assert not path.exists()


def _sample_value(arrow_type, i):
    if arrow_type == pa.string():
        return f"v{i}"
    if arrow_type == pa.bool_():
        return i % 2 == 0
    return i * 10


def test_null_in_nested_struct_is_refused(tmp_path):
    """A null on a parent struct makes every child row null with no child validity bit set."""
    table = pa.table(
        {
            "st": pa.array(
                [{"x": 1}, None, {"x": 3}],
                type=pa.struct([pa.field("x", pa.int32(), nullable=False)]),
            )
        }
    )
    with pytest.raises(RuntimeError) as excinfo:
        nanolance.write_table(table, tmp_path / "struct_nulls.lance")
    assert "null at row 1" in str(excinfo.value)


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
