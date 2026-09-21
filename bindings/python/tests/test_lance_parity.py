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


def test_struct_column_roundtrips(tmp_path):
    """A plain (non-blob) struct column reads back through nanolance's own reader.

    The writer always produced a correct file for these -- pylance reads one fine -- but the reader
    refused with "nested struct children are not supported in this reader build", so nanolance could
    not read back a file it had just written, for a feature README.md advertises.
    """
    table = pa.table(
        {
            "id": pa.array([1, 2, 3], type=pa.int64()),
            "st": pa.array(
                [{"u": "alpha", "n": 10}, {"u": "beta", "n": 20}, {"u": "gamma", "n": 30}],
                type=pa.struct(
                    [
                        pa.field("u", pa.string(), nullable=False),
                        pa.field("n", pa.int32(), nullable=False),
                    ]
                ),
            ),
        }
    )
    path = tmp_path / "struct.lance"
    nanolance.write_table(table, path)
    assert pa.table(nanolance.read_table(path)).to_pydict() == table.to_pydict()


def test_struct_child_name_shadowing_a_top_level_column(tmp_path):
    """Nested field lookup is scoped to the parent, so `st.id` does not resolve to top-level `id`."""
    table = pa.table(
        {
            "id": pa.array([1, 2], type=pa.int64()),
            "st": pa.array(
                [{"id": 100}, {"id": 200}],
                type=pa.struct([pa.field("id", pa.int32(), nullable=False)]),
            ),
        }
    )
    path = tmp_path / "shadow.lance"
    nanolance.write_table(table, path)
    assert pa.table(nanolance.read_table(path)).to_pydict() == table.to_pydict()


def test_dictionary_column_is_refused(tmp_path):
    """Writing an Arrow dictionary column stored only the indices and discarded the values."""
    table = pa.table({"d": pa.array(["a", "b", "a"]).dictionary_encode()})
    with pytest.raises(RuntimeError) as excinfo:
        nanolance.write_table(table, tmp_path / "dict.lance")
    assert "dictionary" in str(excinfo.value)
    assert "cast" in str(excinfo.value)
    # The suggested remedy works, and costs nothing on disk (nanolance dictionary-encodes
    # low-cardinality string columns by itself).
    plain = table.cast(pa.schema([pa.field("d", pa.string())]))
    path = tmp_path / "plain.lance"
    nanolance.write_table(plain, path)
    assert pa.table(nanolance.read_table(path)).to_pydict() == plain.to_pydict()


@pytest.mark.parametrize("arrow_type", [pa.large_string(), pa.large_binary()])
def test_large_offset_types_are_refused(arrow_type, tmp_path):
    """These produced a file stock Lance rejects as corrupt (64-bit offsets in a u32 chunk)."""
    values = ["a", "bb", "ccc"] if arrow_type == pa.large_string() else [b"a", b"bb", b"ccc"]
    table = pa.table({"v": pa.array(values, type=arrow_type)})
    with pytest.raises(RuntimeError) as excinfo:
        nanolance.write_table(table, tmp_path / "large.lance")
    assert "cannot write yet" in str(excinfo.value)


# ── Temporal and decimal types ───────────────────────────────────────────────────────────────────
# All of these are fixed-width integers on the wire, so they needed no encoder work -- only the
# Arrow format strings and the Lance logical-type names, which were read back out of manifests
# written by pylance 12.0.0 rather than invented.

TEMPORAL_AND_DECIMAL = [
    pytest.param(pa.timestamp("s"), id="timestamp_s"),
    pytest.param(pa.timestamp("ms"), id="timestamp_ms"),
    pytest.param(pa.timestamp("us"), id="timestamp_us"),
    pytest.param(pa.timestamp("ns"), id="timestamp_ns"),
    pytest.param(pa.timestamp("us", tz="UTC"), id="timestamp_tz_utc"),
    pytest.param(pa.timestamp("ms", tz="Europe/Berlin"), id="timestamp_tz_iana"),
    pytest.param(pa.date32(), id="date32"),
    pytest.param(pa.date64(), id="date64"),
    pytest.param(pa.time32("s"), id="time32_s"),
    pytest.param(pa.time32("ms"), id="time32_ms"),
    pytest.param(pa.time64("us"), id="time64_us"),
    pytest.param(pa.time64("ns"), id="time64_ns"),
    pytest.param(pa.decimal128(18, 4), id="decimal128"),
    pytest.param(pa.decimal256(40, 6), id="decimal256"),
]


def _values_for(arrow_type):
    import decimal

    if pa.types.is_decimal(arrow_type):
        return [decimal.Decimal("1.5"), decimal.Decimal("-2.25"), decimal.Decimal("3")]
    if pa.types.is_date64(arrow_type):
        return [0, 86_400_000, 172_800_000]  # date64 values must be whole days in ms
    return [1, 2, 3]


@pytest.mark.parametrize("arrow_type", TEMPORAL_AND_DECIMAL)
def test_temporal_and_decimal_roundtrip(arrow_type, tmp_path):
    """Type AND values must survive, including the unit, timezone, precision and scale.

    Compared with Table.equals rather than to_pydict(): a nanosecond timestamp cannot be converted to
    datetime.datetime without pandas, so to_pydict() raises on ns columns regardless of correctness.
    """
    table = pa.table({"v": pa.array(_values_for(arrow_type), type=arrow_type)})
    path = tmp_path / "temporal.lance"
    nanolance.write_table(table, path)
    back = pa.table(nanolance.read_table(path))
    assert back.schema.field(0).type == arrow_type
    assert back.column(0).equals(table.column(0))


@pytest.mark.parametrize("arrow_type", TEMPORAL_AND_DECIMAL)
def test_temporal_and_decimal_readable_by_stock_lance(arrow_type, tmp_path):
    """The values, and the full parameterised type, must survive to the reference implementation.

    Only the column is compared, not the schema: nanolance writes non-null Lance fields for every
    type (see the nullability tests above), so the field's nullable flag differs by design.
    """
    lance = require_pylance()
    table = pa.table({"v": pa.array(_values_for(arrow_type), type=arrow_type)})
    path = tmp_path / "temporal_interop.lance"
    nanolance.write_table(table, path)
    back = lance.dataset(str(path)).to_table()
    assert back.schema.field(0).type == arrow_type
    assert back.column(0).equals(table.column(0))


@pytest.mark.parametrize("arrow_type", TEMPORAL_AND_DECIMAL)
def test_temporal_and_decimal_written_by_stock_lance(arrow_type, tmp_path):
    """The other direction: nanolance reads these straight out of a dataset pylance wrote.

    Temporal and decimal columns were previously refused at the manifest-mapping stage
    ("unsupported on-disk logical type for manifest recovery: timestamp:ms:-"), so this is new.
    """
    lance = require_pylance()
    table = pa.table({"v": pa.array(_values_for(arrow_type), type=arrow_type)})
    path = tmp_path / "from_lance.lance"
    lance.write_dataset(table, str(path), mode="overwrite")
    back = pa.table(nanolance.read_table(path))
    assert back.schema.field(0).type == arrow_type
    assert back.column(0).equals(table.column(0))


@pytest.mark.parametrize(
    "arrow_type",
    [
        pytest.param(pa.int16(), id="int16"),
        pytest.param(pa.uint16(), id="uint16"),
        pytest.param(pa.binary(6), id="fixed_size_binary_6"),
        pytest.param(pa.binary(3), id="fixed_size_binary_3"),
    ],
)
@pytest.mark.parametrize("structural", [True, False], ids=["structural", "plain"])
def test_declared_width_matches_the_real_width(arrow_type, structural, tmp_path):
    """A flat page must declare its REAL width, not a nearby one.

    The width token snapped anything it did not recognise to 32 bits, so a 16-bit column and any
    fixed_size_binary(N != 4) were mis-declared. Stock Lance panicked reading the first
    ("range end index 12 out of range for slice of length 6") and errored on the second -- including
    the 6-byte MAC address README.md recommends the type for. With structural encodings on, integers
    escape through InlineBitpacking (which declares its own width) so int16 happened to survive;
    fixed_size_binary is not bitpackable and did not, making this reachable by default.
    """
    lance = require_pylance()
    if pa.types.is_fixed_size_binary(arrow_type):
        width = arrow_type.byte_width
        values = [bytes([i] * width) for i in range(3)]
    else:
        values = [1, 2, 3]
    table = pa.table({"v": pa.array(values, type=arrow_type)})
    path = tmp_path / "width.lance"
    nanolance.write_table(table, path, structural_encoding=structural)

    assert pa.table(nanolance.read_table(path)).column(0).equals(table.column(0))
    assert lance.dataset(str(path)).to_table().column(0).equals(table.column(0))


def test_offset_timezone_is_refused(tmp_path):
    """Lance supports IANA zone names only; a UTC offset makes it panic, on read and on write.

    pylance cannot produce such a file either -- `lance.write_dataset` on a tz="+05:30" column raises
    PanicException from lance-core's schema layer ("Unsupported timestamp type: timestamp:us:+05:30").
    So refusing here is not nanolance being stricter than the format; it is declining to write
    something no reference reader will open.
    """
    table = pa.table({"v": pa.array([1, 2, 3], type=pa.timestamp("us", tz="+05:30"))})
    with pytest.raises(RuntimeError) as excinfo:
        nanolance.write_table(table, tmp_path / "offset_tz.lance")
    message = str(excinfo.value)
    assert "offset" in message
    assert "IANA" in message
    # The suggested remedy works.
    named = table.set_column(
        0, "v", table.column(0).cast(pa.timestamp("us", tz="Europe/Berlin"))
    )
    path = tmp_path / "named_tz.lance"
    nanolance.write_table(named, path)
    assert pa.table(nanolance.read_table(path)).column(0).equals(named.column(0))
