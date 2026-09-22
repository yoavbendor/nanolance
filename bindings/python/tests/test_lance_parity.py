"""Lance binding parity and full-cycle tests."""

from __future__ import annotations

import random

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
        ("f", pa.bool_()),
        ("s", pa.string()),
    ],
)
def test_null_values_are_stored(column, arrow_type, tmp_path):
    """A null *value* round-trips.

    This test has asserted three different behaviours in turn, which is the history of the bug: the
    original silent corruption ([10, None, 30] came back as [10, 0, 30], and stock Lance read those
    wrong values without complaint), then the refusal that replaced it, and now the real thing.
    """
    values = [None if i == 1 else _sample_value(arrow_type, i) for i in range(4)]
    table = pa.table({column: pa.array(values, type=arrow_type)})
    path = tmp_path / "nulls.lance"
    nanolance.write_table(table, path)
    assert pa.table(nanolance.read_table(path)).column(0).to_pylist() == values


def _sample_value(arrow_type, i):
    if arrow_type == pa.string():
        return f"v{i}"
    if arrow_type == pa.bool_():
        return i % 2 == 0
    return i * 10


def test_null_struct_is_refused(tmp_path):
    """A null STRUCT is not the same as a struct whose fields are all null.

    Lance distinguishes them with a deeper definition level; nanolance writes only one. Folding the
    parent's nulls into its children would silently turn "no struct here" into "a struct with
    nothing in it", so this is refused rather than approximated. A null on a struct's FIELD is fine
    and is covered above.
    """
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
    message = str(excinfo.value)
    assert "struct with a null at row 1" in message
    assert "second definition level" in message


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


# ── Reading nullable columns written by stock Lance ──────────────────────────────────────────────
# Lance stores one definition level per value, FastLanes-bit-packed, where level 1 means NULL -- the
# opposite polarity to Arrow's validity bit. The levels live in a second buffer inside each miniblock
# chunk, announced by the chunk header's first u16 (the value count the levels cover) and by
# MiniBlockLayout.layers == [3]. None of that was read before, so every nullable stock-Lance column
# failed with "unexpected miniblock payload prefix".


@pytest.mark.parametrize(
    "pattern, name",
    [
        pytest.param(lambda i: i % 3 == 0, "one_in_three", id="one_in_three"),
        pytest.param(lambda i: i % 2 == 0, "alternating", id="alternating"),
        pytest.param(lambda i: (i * 7) % 10 == 0, "scattered_10pct", id="scattered_10pct"),
        pytest.param(lambda i: False, "no_nulls", id="no_nulls"),
        pytest.param(lambda i: True, "all_null", id="all_null"),
    ],
)
def test_reads_nullable_columns_written_by_stock_lance(pattern, name, tmp_path):
    lance = require_pylance()
    n = 3000  # spans several 1024-value chunks, including a short final one
    values = [None if pattern(i) else i for i in range(n)]
    table = pa.table({"v": pa.array(values, type=pa.int64())})
    path = tmp_path / f"{name}.lance"
    lance.write_dataset(table, str(path), mode="overwrite")

    back = pa.table(nanolance.read_table(path))
    assert back.column(0).null_count == table.column(0).null_count
    assert back.column(0).to_pylist() == values


def test_reads_multi_chunk_pages_written_by_stock_lance(tmp_path):
    """A page is not a chunk.

    nanolance's writer emits exactly one chunk per page, so treating a page's payload as a single
    chunk worked on its own files. Stock Lance packs many -- a 5000-row int64 page arrives as five
    1024-value chunks -- and the concatenated buffer failed the per-chunk size check with
    "bitpacked chunk size does not match bit width". This is the plainest possible stock-Lance
    column, and it did not decode.
    """
    lance = require_pylance()
    table = pa.table({"v": pa.array(list(range(5000)), type=pa.int64())})
    path = tmp_path / "multichunk.lance"
    lance.write_dataset(table, str(path), mode="overwrite")
    assert pa.table(nanolance.read_table(path)).column(0).to_pylist() == list(range(5000))


# ── Reading string columns written by stock Lance (FSST) ─────────────────────────────────────────
# Stock Lance wraps every variable-width column in FSST (CompressiveEncoding field 6), whether or not
# it actually compressed: below 32 KiB of input the encoder declines and the "compressed" bytes are
# the originals, but the wrapper is there either way. nanolance refused the whole variant, which made
# `utf8` the last common type a pylance file could carry that nanolance could not read at all.

# Repetitive sentences from a small vocabulary, so the total comfortably clears the 32 KiB threshold
# and FSST really does build a symbol table (120 symbols, in practice) rather than passing through.
# Every value ends in its own row number: without that the column is low-cardinality enough that
# Lance picks a *dictionary* page instead and FSST never appears.
_FSST_WORDS = ["alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel"]


def _fsst_sentence(i):
    rng = random.Random(i)
    words = " ".join(rng.choice(_FSST_WORDS) for _ in range(rng.randint(3, 12)))
    return f"{words} {i}"


@pytest.mark.parametrize("arrow_type", [pa.string(), pa.large_string(), pa.binary()],
                         ids=["utf8", "large_utf8", "binary"])
@pytest.mark.parametrize(
    "null_at",
    [
        pytest.param(lambda i: False, id="none"),
        pytest.param(lambda i: i % 11 == 0, id="scattered"),
    ],
)
def test_reads_fsst_string_columns_written_by_stock_lance(arrow_type, null_at, tmp_path):
    lance = require_pylance()
    n = 20000

    def value_for(i):
        text = _fsst_sentence(i)
        return text.encode() if arrow_type == pa.binary() else text

    values = [None if null_at(i) else value_for(i) for i in range(n)]
    table = pa.table({"s": pa.array(values, type=arrow_type)})
    path = tmp_path / "fsst.lance"
    lance.write_dataset(table, str(path), mode="overwrite")

    back = pa.table(nanolance.read_table(path))
    assert back.column(0).null_count == table.column(0).null_count
    assert back.column(0).to_pylist() == values


def test_reads_small_string_columns_written_by_stock_lance(tmp_path):
    """Under 32 KiB Lance still writes the FSST wrapper, with an empty symbol table and the encoder
    switched off -- so the bytes are verbatim. That passthrough case is most small files, not an
    edge case."""
    lance = require_pylance()
    values = [f"s{i}" for i in range(500)]
    table = pa.table({"s": pa.array(values, type=pa.string())})
    path = tmp_path / "small.lance"
    lance.write_dataset(table, str(path), mode="overwrite")
    assert pa.table(nanolance.read_table(path)).column(0).to_pylist() == values


def test_reads_empty_and_null_strings_written_by_stock_lance(tmp_path):
    """An empty string and a null are distinct rows that both occupy zero data bytes."""
    lance = require_pylance()
    n = 20000
    values = ["" if i % 3 == 0 else (None if i % 5 == 0 else _fsst_sentence(i)) for i in range(n)]
    table = pa.table({"s": pa.array(values, type=pa.string())})
    path = tmp_path / "empties.lance"
    lance.write_dataset(table, str(path), mode="overwrite")
    assert pa.table(nanolance.read_table(path)).column(0).to_pylist() == values


# ── Run-length-encoded definition levels ─────────────────────────────────────────────────────────
# Lance run-length-encodes the definition levels whenever the nulls come in runs or are very sparse,
# which makes "one null in 5000 rows" a *different* repdef encoding from "a null every 13th row" --
# `Rle{Flat(16), Flat(8)}` rather than the bit-packed block. The block is
# [u64 values_size][run values][run lengths]; a run too long for the length type is split into
# several entries carrying the same value, so decoding is a plain expansion.


@pytest.mark.parametrize(
    "null_at, name",
    [
        pytest.param(lambda i: i == 7, "one_null", id="one_null"),
        pytest.param(lambda i: 100 <= i < 400 or 3000 <= i < 3100, "runs", id="two_runs"),
        pytest.param(lambda i: i % 997 == 0, "sparse", id="very_sparse"),
        pytest.param(lambda i: i < 1500, "leading", id="leading_run"),
        pytest.param(lambda i: i >= 4200, "trailing", id="trailing_run"),
        pytest.param(lambda i: i % 2 == 0, "alternating", id="alternating"),
    ],
)
def test_reads_run_length_encoded_definition_levels(null_at, name, tmp_path):
    """Whichever repdef encoding Lance picks, the values and the nulls must both come back.

    `alternating` is here to keep the bit-packed path covered by the same assertions: Lance chooses
    between the two on its own, so a test that only fed it run-shaped nulls would stop exercising it
    the day that heuristic changed.
    """
    lance = require_pylance()
    n = 5000
    values = [None if null_at(i) else i for i in range(n)]
    table = pa.table({"v": pa.array(values, type=pa.int64())})
    path = tmp_path / f"{name}.lance"
    lance.write_dataset(table, str(path), mode="overwrite")

    back = pa.table(nanolance.read_table(path))
    assert back.column(0).null_count == table.column(0).null_count
    assert back.column(0).to_pylist() == values


def test_reads_run_length_definition_levels_on_a_string_column(tmp_path):
    """The levels are decoded before the values, so the two encodings combine independently."""
    lance = require_pylance()
    n = 20000
    values = [None if 500 <= i < 900 else _fsst_sentence(i) for i in range(n)]
    table = pa.table({"s": pa.array(values, type=pa.string())})
    path = tmp_path / "rle_levels_fsst.lance"
    lance.write_dataset(table, str(path), mode="overwrite")
    assert pa.table(nanolance.read_table(path)).column(0).to_pylist() == values


# ── LZ4-compressed dictionary blocks ─────────────────────────────────────────────────────────────
# A string column with few distinct values -- a categorical column -- comes back from Lance as a
# dictionary page whose dictionary block is General{LZ4, Variable}. nanolance read that block raw and
# died on its header with "dict block header invalid", which named the symptom rather than the
# missing decoder. It now decompresses it (src/lz4_block.cpp; the LZ4 *block* format, not the framed
# one, and no new dependency: decompression is a token, a literal run and a back-reference).


@pytest.mark.parametrize(
    "values_for, name",
    [
        pytest.param(lambda n: ["alpha", "beta", "gamma"] * (n // 3) + ["alpha"] * (n % 3),
                     "few_values", id="few_values"),
        pytest.param(lambda n: [("h\u00e9llo w\u00f6rld \u00fcn\u00efcode " * (i % 5 + 1)) for i in range(n)],
                     "unicode", id="unicode"),
        pytest.param(lambda n: [("category_%d " % (i % 50)) * 20 for i in range(n)],
                     "long_values", id="long_values"),
    ],
)
def test_reads_lz4_compressed_dictionaries_written_by_stock_lance(values_for, name, tmp_path):
    lance = require_pylance()
    values = values_for(20000)
    table = pa.table({"s": pa.array(values, type=pa.string())})
    path = tmp_path / f"{name}.lance"
    lance.write_dataset(table, str(path), mode="overwrite")
    assert pa.table(nanolance.read_table(path)).column(0).to_pylist() == values


@pytest.mark.parametrize(
    "null_at",
    [
        pytest.param(lambda i: i % 23 == 0, id="scattered"),
        pytest.param(lambda i: 500 <= i < 900, id="one_run"),
    ],
)
def test_reads_nullable_dictionary_columns_written_by_stock_lance(null_at, tmp_path):
    """A categorical column WITH missing values -- the shape the encoding exists for.

    Its index chunks carry definition levels like any other miniblock page, but the dictionary path
    had its own chunk parser that rejected them with "unexpected miniblock payload prefix". It now
    goes through the same splitter as everything else, so the levels (bit-packed here, run-length
    encoded for the one-run case) decode on the way.
    """
    lance = require_pylance()
    n = 20000
    values = [None if null_at(i) else ["alpha", "beta", "gamma", "delta"][i % 4] for i in range(n)]
    table = pa.table({"s": pa.array(values, type=pa.string())})
    path = tmp_path / "nullable_dict.lance"
    lance.write_dataset(table, str(path), mode="overwrite")
    back = pa.table(nanolance.read_table(path))
    assert back.column(0).null_count == table.column(0).null_count
    assert back.column(0).to_pylist() == values


# ── Column projection ────────────────────────────────────────────────────────────────────────────
# `columns=` is the first thing a pyarrow.parquet user reaches for. It is a real projection: the
# columns not asked for are skipped during decode rather than decoded and discarded, which matters
# because column materialization is where a read spends its time.


@pytest.fixture
def projection_table():
    return pa.table(
        {
            "a": pa.array([1, 2, 3], type=pa.int64()),
            "b": pa.array(["x", None, "z"], type=pa.string()),
            "c": pa.array([1.5, 2.5, 3.5], type=pa.float64()),
        }
    )


@pytest.mark.parametrize(
    "columns", [["a"], ["b"], ["a", "c"], ["a", "b", "c"]],
    ids=["one_fixed", "one_variable", "subset", "all_named"],
)
def test_projection_reads_exactly_the_named_columns(columns, projection_table, tmp_path):
    path = tmp_path / "projected.lance"
    nanolance.write_table(projection_table, path)
    back = pa.table(nanolance.read_table(path, columns=columns))
    assert back.column_names == columns
    for name in columns:
        assert back.column(name).to_pylist() == projection_table.column(name).to_pylist()


def test_projection_returns_dataset_order_not_requested_order(projection_table, tmp_path):
    """Documented difference from pyarrow.parquet, pinned so it cannot drift silently."""
    path = tmp_path / "order.lance"
    nanolance.write_table(projection_table, path)
    assert pa.table(nanolance.read_table(path, columns=["c", "a"])).column_names == ["a", "c"]


def test_projection_matches_a_full_read(projection_table, tmp_path):
    """The projected path must not be a second, subtly different decoder."""
    path = tmp_path / "same.lance"
    nanolance.write_table(projection_table, path, compression=True)
    full = pa.table(nanolance.read_table(path))
    for name in projection_table.column_names:
        projected = pa.table(nanolance.read_table(path, columns=[name]))
        assert projected.column(name).equals(full.column(name))


def test_projection_of_a_stock_lance_dataset(tmp_path):
    """Projection is orthogonal to who wrote the file."""
    lance = require_pylance()
    n = 20000
    table = pa.table(
        {
            "keep": pa.array([None if i % 11 == 0 else _fsst_sentence(i) for i in range(n)]),
            "drop": pa.array(list(range(n)), type=pa.int64()),
        }
    )
    path = tmp_path / "stock_projected.lance"
    lance.write_dataset(table, str(path), mode="overwrite")
    back = pa.table(nanolance.read_table(path, columns=["keep"]))
    assert back.column_names == ["keep"]
    assert back.column("keep").to_pylist() == table.column("keep").to_pylist()


@pytest.mark.parametrize(
    "columns, exc, needle",
    [
        pytest.param(["nope"], RuntimeError, "not found", id="unknown_column"),
        pytest.param([], ValueError, "at least one column", id="empty_list"),
        pytest.param("a", TypeError, "not a single string", id="bare_string"),
    ],
)
def test_bad_projections_are_refused_by_name(columns, exc, needle, projection_table, tmp_path):
    """A bare string is the trap worth a test: str IS a Sequence[str], of its own characters."""
    path = tmp_path / "bad.lance"
    nanolance.write_table(projection_table, path)
    with pytest.raises(exc) as excinfo:
        nanolance.read_table(path, columns=columns)
    assert needle in str(excinfo.value)


# ── Streaming reads ──────────────────────────────────────────────────────────────────────────────
# `open_stream` decodes one batch per pull instead of every batch up front. It is a SEPARATE entry
# point from read_table on purpose: streaming moves error reporting from open to consumption, and
# quietly changing read_table's contract would break anyone catching around it.


def _write_fragments(path, table, rows_per_fragment):
    with nanolance.LanceWriter(path, max_rows_per_fragment=rows_per_fragment) as writer:
        for batch in table.to_batches(max_chunksize=rows_per_fragment):
            writer.write_batch(batch)


@pytest.fixture
def fragmented_dataset(tmp_path):
    n, frag = 60_000, 10_000
    table = pa.table(
        {
            "a": pa.array(range(n), type=pa.int64()),
            "s": pa.array([None if i % 7 == 0 else f"v{i}" for i in range(n)], type=pa.string()),
        }
    )
    path = tmp_path / "fragmented.lance"
    _write_fragments(path, table, frag)
    return path, table, n // frag


def test_stream_yields_the_same_rows_as_a_full_read(fragmented_dataset):
    path, table, _ = fragmented_dataset
    streamed = pa.RecordBatchReader.from_stream(nanolance.open_stream(path)).read_all()
    assert streamed.to_pydict() == table.to_pydict()
    assert streamed.to_pydict() == pa.table(nanolance.read_table(path)).to_pydict()


def test_stream_delivers_one_batch_per_fragment(fragmented_dataset):
    """The point of the exercise: batches arrive as they are decoded, not all at the end."""
    path, table, fragments = fragmented_dataset
    sizes = [b.num_rows for b in pa.RecordBatchReader.from_stream(nanolance.open_stream(path))]
    assert len(sizes) == fragments
    assert sum(sizes) == table.num_rows


def test_stream_projects(fragmented_dataset):
    path, table, _ = fragmented_dataset
    streamed = pa.RecordBatchReader.from_stream(nanolance.open_stream(path, columns=["s"])).read_all()
    assert streamed.column_names == ["s"]
    assert streamed.column("s").to_pylist() == table.column("s").to_pylist()


def test_stream_handle_is_single_shot(fragmented_dataset):
    """A stream is consumed, not copied. Exporting twice must say so, not hand out a drained one."""
    path, _, _ = fragmented_dataset
    handle = nanolance.open_stream(path)
    pa.RecordBatchReader.from_stream(handle).read_all()
    with pytest.raises(RuntimeError) as excinfo:
        pa.RecordBatchReader.from_stream(handle)
    assert "already been consumed" in str(excinfo.value)


def test_stream_can_be_abandoned_midway(fragmented_dataset):
    """Dropping a partly-consumed reader must release the dataset, not leak or crash."""
    path, _, _ = fragmented_dataset
    reader = pa.RecordBatchReader.from_stream(nanolance.open_stream(path))
    first = next(iter(reader))
    assert first.num_rows > 0
    del reader
    # Still usable afterwards.
    assert pa.RecordBatchReader.from_stream(nanolance.open_stream(path)).read_all().num_rows > 0


def test_stream_reports_a_corrupt_data_file_at_consumption(tmp_path):
    """The documented difference from read_table, pinned so it cannot drift.

    Opening validates the manifest and schema; a corrupt data file is only found when the batch
    containing it is pulled. read_table reports it at call time because it decodes everything there.
    Either way the process must survive and the message must be accurate.
    """
    path = tmp_path / "victim.lance"
    nanolance.write_table(pa.table({"a": pa.array([1, 2, 3], type=pa.int64())}), path)
    next((path / "data").glob("*.lance")).write_bytes(b"not a lance file")

    handle = nanolance.open_stream(path)  # opening alone does NOT raise
    with pytest.raises(Exception) as excinfo:
        pa.RecordBatchReader.from_stream(handle).read_all()
    assert "data file" in str(excinfo.value)

    # read_table still reports the same corruption eagerly, which is why it stayed a separate call.
    with pytest.raises(RuntimeError):
        nanolance.read_table(path)


def test_stream_of_a_stock_lance_dataset(tmp_path):
    lance = require_pylance()
    n = 20000
    table = pa.table({"s": pa.array([None if i % 11 == 0 else _fsst_sentence(i) for i in range(n)])})
    path = tmp_path / "stock_stream.lance"
    lance.write_dataset(table, str(path), mode="overwrite")
    streamed = pa.RecordBatchReader.from_stream(nanolance.open_stream(path)).read_all()
    assert streamed.column("s").to_pylist() == table.column("s").to_pylist()


# ── Writing nulls ────────────────────────────────────────────────────────────────────────────────
# nanolance now emits Lance's definition-level layer for fixed-width columns: layers = [3], the
# repdef encoding in MiniBlockLayout.f2, and a per-chunk level buffer ahead of the values. Level 1
# means NULL, the inverse of Arrow's validity bit.

NULLABLE_FIXED_WIDTH = [
    pytest.param(pa.int64(), lambda i: i, id="int64"),
    pytest.param(pa.int16(), lambda i: i % 100, id="int16"),
    pytest.param(pa.uint8(), lambda i: i % 200, id="uint8"),
    pytest.param(pa.float64(), lambda i: i * 0.5, id="float64"),
    pytest.param(pa.bool_(), lambda i: i % 2 == 0, id="bool"),
    pytest.param(pa.timestamp("ms"), lambda i: 1_700_000_000_000 + i, id="timestamp"),
    pytest.param(pa.date32(), lambda i: i, id="date32"),
    pytest.param(pa.decimal128(18, 4), lambda i: __import__("decimal").Decimal("1.50"), id="decimal128"),
    pytest.param(pa.binary(4), lambda i: bytes([i % 256]) * 4, id="fixed_size_binary"),
]


@pytest.mark.parametrize("arrow_type, value_for", NULLABLE_FIXED_WIDTH)
@pytest.mark.parametrize("compression", [False, True], ids=["plain", "zstd"])
@pytest.mark.parametrize(
    "null_at",
    [
        pytest.param(lambda i: i % 13 == 0, id="scattered"),
        pytest.param(lambda i: i == 0, id="first_row_only"),
        pytest.param(lambda i: False, id="none"),
    ],
)
def test_nullable_fixed_width_roundtrip(arrow_type, value_for, compression, null_at, tmp_path):
    """Nulls survive nanolance's own round trip AND are read back correctly by stock Lance.

    3000 rows spans several 1024-value chunks including a short final one -- a chunk carrying
    definition levels is capped at one FastLanes block, which is narrower than the byte budget a
    plain chunk uses, so the chunking differs from the no-nulls case.
    """
    lance = require_pylance()
    n = 3000
    values = [None if null_at(i) else value_for(i) for i in range(n)]
    table = pa.table({"v": pa.array(values, type=arrow_type)})
    path = tmp_path / "nullable.lance"
    nanolance.write_table(table, path, compression=compression)

    # Compare against the source COLUMN, not the raw python list: to_pylist() converts a timestamp
    # to datetime and a date32 to date, so the list of ints they were built from never matches.
    back = pa.table(nanolance.read_table(path))
    assert back.column(0).null_count == table.column(0).null_count
    assert back.column(0).equals(table.column(0))

    ref = lance.dataset(str(path)).to_table()
    assert ref.column(0).null_count == table.column(0).null_count
    assert ref.column(0).equals(table.column(0))


def test_nullable_multi_column_table(tmp_path):
    """Several nullable columns with different null patterns in one table."""
    lance = require_pylance()
    n = 4000
    table = pa.table(
        {
            "id": pa.array([None if i % 97 == 0 else i for i in range(n)], type=pa.int64()),
            "score": pa.array([None if i % 13 == 0 else i * 0.5 for i in range(n)], type=pa.float64()),
            "flag": pa.array([None if i % 7 == 0 else (i % 2 == 0) for i in range(n)]),
            "solid": pa.array(list(range(n)), type=pa.int64()),  # no nulls at all
        }
    )
    path = tmp_path / "multi.lance"
    nanolance.write_table(table, path)
    back = pa.table(nanolance.read_table(path))
    ref = lance.dataset(str(path)).to_table()
    for name in table.column_names:
        assert back.column(name).to_pylist() == table.column(name).to_pylist(), name
        assert ref.column(name).to_pylist() == table.column(name).to_pylist(), name


def test_nulls_appearing_in_a_later_chunk(tmp_path):
    """The validity bitmap materializes lazily; a column whose first null is far in must still work."""
    lance = require_pylance()
    n = 5000
    values = [i if i < 4000 else None for i in range(n)]
    table = pa.table({"v": pa.array(values, type=pa.int64())})
    path = tmp_path / "late.lance"
    nanolance.write_table(table, path)
    assert pa.table(nanolance.read_table(path)).column(0).to_pylist() == values
    assert lance.dataset(str(path)).to_table().column(0).to_pylist() == values


# Variable-width columns carry the same definition-level layer. Their chunks are sized by the
# offsets math rather than a value count, so a nullable one is additionally capped at one FastLanes
# block (1024 values) -- which is what makes the boundary cases below worth spelling out.

@pytest.mark.parametrize("arrow_type", [pa.string(), pa.binary()], ids=["utf8", "binary"])
@pytest.mark.parametrize("compression", [False, True], ids=["plain", "zstd"])
@pytest.mark.parametrize(
    "null_at",
    [
        pytest.param(lambda i: i % 13 == 0, id="scattered"),
        pytest.param(lambda i: i == 0, id="first_row_only"),
        pytest.param(lambda i: i in (1023, 1024, 1025), id="chunk_boundary"),
        pytest.param(lambda i: True, id="all_null"),
        pytest.param(lambda i: False, id="none"),
    ],
)
def test_nullable_variable_width_roundtrip(arrow_type, compression, null_at, tmp_path):
    """Nulls in a string/binary column survive nanolance's round trip and stock Lance's."""
    lance = require_pylance()
    n = 3000

    def value_for(i):
        # Deliberately ragged, including empty values: a null and an empty string are distinct rows
        # that both occupy zero data bytes, so an offsets bug that conflates them shows up here.
        raw = ("v%d" % i) * (i % 4)
        return raw if arrow_type == pa.string() else raw.encode()

    values = [None if null_at(i) else value_for(i) for i in range(n)]
    table = pa.table({"s": pa.array(values, type=arrow_type)})
    path = tmp_path / "nullable_var.lance"
    nanolance.write_table(table, path, compression=compression)

    back = pa.table(nanolance.read_table(path))
    assert back.column(0).null_count == table.column(0).null_count
    assert back.column(0).to_pylist() == values

    ref = lance.dataset(str(path)).to_table()
    assert ref.column(0).null_count == table.column(0).null_count
    assert ref.column(0).to_pylist() == values


@pytest.mark.parametrize("compression", [False, True], ids=["plain", "zstd"])
def test_nullable_variable_width_single_row(compression, tmp_path):
    """One row, and that row null: the smallest page that still has to carry a level buffer."""
    lance = require_pylance()
    table = pa.table({"s": pa.array([None], type=pa.string())})
    path = tmp_path / "one_null.lance"
    nanolance.write_table(table, path, compression=compression)
    assert pa.table(nanolance.read_table(path)).column(0).to_pylist() == [None]
    assert lance.dataset(str(path)).to_table().column(0).to_pylist() == [None]

