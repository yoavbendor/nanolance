"""Which Arrow types nanolance supports, in each direction, as an executable statement.

Three things this pins that prose cannot:

1. **A refused type must refuse, not corrupt.** The worst failure this project has had was writing a
   type it could not represent and producing a file that read back wrong (see the nullability work).
   Every entry in ``REFUSED_ON_WRITE`` asserts a clean, named refusal -- so a future change that makes
   one of them "work" fails here instead of silently shipping bad files.
2. **The read and write surfaces are not the same, on purpose.** `large_string` and `large_binary`
   READ correctly from a pylance dataset and are REFUSED on write. That asymmetry is deliberate and
   its reason is recorded where it is raised; pinning it stops someone "tidying up" one side.
   (`decimal256`, `float16`, `duration` and Arrow's `null` type used to be on the refused side too;
   all four now round-trip.)
3. **Lists and maps round-trip.** `list`, `large_list`, lists of structs, structs of lists and maps
   are written as leaf columns with repetition and definition levels (roadmap Phase D) and read
   back by both readers, as are pylance's own (Phase C).

Types are exercised at 200 rows: enough for Lance to make real encoding choices, small enough that
the whole matrix runs in a couple of seconds. Row-count sensitivity is `test_lance_read_matrix.py`'s
job, not this file's.
"""

from __future__ import annotations

import datetime
import decimal

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance

N = 200


def _t(arrow_type, values):
    return pa.table({"c": pa.array(values, arrow_type)})


def _ints():
    out = {}
    for width in (8, 16, 32, 64):
        out[f"int{width}"] = _t(getattr(pa, f"int{width}")(), [i % 100 for i in range(N)])
        out[f"uint{width}"] = _t(getattr(pa, f"uint{width}")(), [i % 100 for i in range(N)])
    return out


ROUNDTRIPS = {
    **_ints(),
    "float32": _t(pa.float32(), [i * 0.5 for i in range(N)]),
    "float64": _t(pa.float64(), [i * 0.125 for i in range(N)]),
    "bool": _t(pa.bool_(), [i % 3 == 0 for i in range(N)]),
    "string": _t(pa.utf8(), [f"s{i}" for i in range(N)]),
    "binary": _t(pa.binary(), [b"b%d" % i for i in range(N)]),
    "fixed_size_binary": _t(pa.binary(4), [b"%04d" % i for i in range(N)]),
    "date32": _t(pa.date32(), [datetime.date(2026, 1, 1) + datetime.timedelta(days=i) for i in range(N)]),
    "date64": _t(pa.date64(), [datetime.date(2026, 1, 1) + datetime.timedelta(days=i) for i in range(N)]),
    "time32_s": _t(pa.time32("s"), [datetime.time(i % 24, i % 60) for i in range(N)]),
    "time64_us": _t(pa.time64("us"), [datetime.time(i % 24, i % 60) for i in range(N)]),
    "timestamp_us": _t(
        pa.timestamp("us"),
        [datetime.datetime(2026, 1, 1) + datetime.timedelta(seconds=i) for i in range(N)],
    ),
    "timestamp_utc": _t(
        pa.timestamp("us", tz="UTC"),
        [datetime.datetime(2026, 1, 1) + datetime.timedelta(seconds=i) for i in range(N)],
    ),
    "decimal128": _t(pa.decimal128(12, 2), [decimal.Decimal(f"{i}.{i % 100:02d}") for i in range(N)]),
    "decimal256": _t(pa.decimal256(40, 2), [decimal.Decimal(f"{i}.{i % 100:02d}") for i in range(N)]),
    "struct": pa.table({"c": pa.array([{"a": i, "b": f"s{i}"} for i in range(N)])}),
    "struct_nested": pa.table({"c": pa.array([{"a": {"x": i}} for i in range(N)])}),
    # Lance names these "halffloat" and "duration:<unit>" on disk. Both are existing fixed-width
    # paths; they were refused only because nothing mapped the names.
    "float16": _t(pa.float16(), [float(i % 10) for i in range(N)]),
    "float16_nullable": _t(pa.float16(), [None if i % 7 == 0 else float(i % 10) for i in range(N)]),
    "duration_us": _t(pa.duration("us"), [datetime.timedelta(seconds=i) for i in range(N)]),
    "duration_s_nullable": _t(pa.duration("s"), [None if i % 5 == 0 else datetime.timedelta(seconds=i) for i in range(N)]),
    # Arrow's null type: no buffers at all. Written as Lance's all-null spelling -- a ConstantLayout
    # with a NULLABLE_ITEM layer and no value -- byte-identical to pylance's.
    "null": _t(pa.null(), [None] * N),
    # fixed_size_list is NOT a repetition-level list in Lance: one physical column of N-element rows,
    # with a FixedSizeList wrapper in the page descriptor and no child field in the schema. Vectors.
    "fixed_size_list": _t(pa.list_(pa.int64(), 2), [[i, i + 1] for i in range(N)]),
    "fixed_size_list_f32_768": pa.table(
        {"c": pa.FixedSizeListArray.from_arrays(pa.array([float(i % 97) for i in range(N * 768)], pa.float32()), 768)}
    ),
    "fixed_size_list_nullable": _t(pa.list_(pa.float32(), 4), [None if i % 7 == 0 else [i, i, i, i] for i in range(N)]),
    # Lists and maps (roadmap phases C and D): written as leaf columns with repetition and definition
    # levels, read back by both readers.
    "list": _t(pa.list_(pa.int64()), [None if i % 9 == 0 else [i, i + 1][: i % 3] for i in range(N)]),
    "large_list": _t(pa.large_list(pa.int64()), [[i, i + 1] for i in range(N)]),
    "list_of_struct": _t(pa.list_(pa.struct([("a", pa.int64())])), [[{"a": i}] for i in range(N)]),
    "struct_of_list": _t(pa.struct([("a", pa.list_(pa.int64()))]), [{"a": [i]} for i in range(N)]),
    "map": _t(pa.map_(pa.utf8(), pa.int64()), [[("k%d" % i, i)] for i in range(N)]),
    "list_of_strings": _t(pa.list_(pa.utf8()), [[f"s{i}", None][: i % 3] for i in range(N)]),
    "null_struct": _t(pa.struct([("a", pa.int64()), ("s", pa.utf8())]), [None if i % 4 == 0 else {"a": i, "s": f"s{i}"} for i in range(N)]),
}

# Refused at write_batch, each with a message naming the column. The fragment is what the refusal has
# to keep saying; a change that makes any of these WRITE must come here and justify itself.
REFUSED_ON_WRITE = {
    "large_string": (_t(pa.large_utf8(), [f"s{i}" for i in range(N)]), "large_utf8"),
    "large_binary": (_t(pa.large_binary(), [b"b%d" % i for i in range(N)]), "large_binary"),
    "dictionary": (
        pa.table({"c": pa.array([f"d{i % 5}" for i in range(N)]).dictionary_encode()}),
        "dictionary-encoded column",
    ),
    # A null element inside a VALID vector needs FixedSizeList.has_validity on write, which this
    # writer does not emit yet. It reads correctly (pylance writes it; see test_lance_read_matrix).
    "fixed_size_list_item_nulls": (
        _t(pa.list_(pa.float32(), 2), [[None, 1.0] if i % 5 == 0 else [1.0, 2.0] for i in range(N)]),
        "null element inside a row",
    ),
}

# Written by pylance, read correctly by nanolance, but refused on OUR write side. Each asymmetry is
# deliberate; see the refusal sites for why.
READ_ONLY = ("large_string", "large_binary")

# Written by pylance and NOT readable. Pinned by message so each fails loudly when implemented.
UNREADABLE_FROM_PYLANCE = {
    "dictionary": "unsupported on-disk logical type",
}


@pytest.mark.parametrize("name", sorted(ROUNDTRIPS))
def test_type_roundtrips_through_nanolance(tmp_path, name):
    table = ROUNDTRIPS[name]
    path = tmp_path / f"{name}.lance"
    nanolance.write_table(table, path)
    assert pa.table(nanolance.read_table(path)).to_pydict() == table.to_pydict()


@pytest.mark.parametrize("name", sorted(ROUNDTRIPS))
def test_stock_lance_reads_what_nanolance_wrote(tmp_path, name):
    """The other half of "supported": a file only counts if Lance itself can read it."""
    lance_mod = require_pylance()
    table = ROUNDTRIPS[name]
    path = tmp_path / f"{name}.lance"
    nanolance.write_table(table, path)
    assert lance_mod.dataset(str(path)).to_table().to_pydict() == table.to_pydict()


@pytest.mark.parametrize("name", sorted(REFUSED_ON_WRITE))
def test_unsupported_type_is_refused_not_corrupted(tmp_path, name):
    table, fragment = REFUSED_ON_WRITE[name]
    path = tmp_path / f"{name}.lance"
    with pytest.raises(Exception) as excinfo:
        nanolance.write_table(table, path)
    message = str(excinfo.value)
    assert fragment in message, f"{name} refused with an unexpected message: {message}"
    assert "'c'" in message or "column" in message, f"{name}'s refusal does not name the column: {message}"


@pytest.mark.parametrize("name", READ_ONLY)
def test_read_only_types_read_back_from_stock_lance(tmp_path, name):
    """Refused on write, correct on read. Pinning this stops the asymmetry being 'tidied up'."""
    lance_mod = require_pylance()
    table = REFUSED_ON_WRITE[name][0]
    path = str(tmp_path / f"{name}.lance")
    lance_mod.write_dataset(table, path)
    expected = lance_mod.dataset(path).to_table()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()


@pytest.mark.parametrize("name", sorted(ROUNDTRIPS))
def test_stock_lance_written_type_reads_back(tmp_path, name):
    """The third direction: pylance writes it, nanolance reads it.

    The two tests above cover files nanolance WROTE, and they agree with each other because they
    share a writer. This one starts from Lance's own encoder, which picks layouts ours never emits.

    Its absence hid a real bug for as long as this file existed. `struct` round-tripped here and read
    back from nanolance's own files, yet a pylance struct in the FIRST column position was
    unreadable: Lance leaves a zero `parent_id` off the wire (proto3), our decoder read the absence
    as "root", and every child of field id 0 detached from its parent. Only a file Lance wrote could
    show it, and only when the struct was field 0 -- put any column ahead of it and the ids shift,
    the parent link is non-zero, and it is serialized again.
    """
    lance_mod = require_pylance()
    table = ROUNDTRIPS[name]
    path = str(tmp_path / f"{name}.lance")
    lance_mod.write_dataset(table, path)
    expected = lance_mod.dataset(path).to_table()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()


@pytest.mark.parametrize("name", sorted(UNREADABLE_FROM_PYLANCE))
def test_unreadable_types_fail_by_name(tmp_path, name):
    lance_mod = require_pylance()
    table = REFUSED_ON_WRITE[name][0] if name in REFUSED_ON_WRITE else ROUNDTRIPS[name]
    path = str(tmp_path / f"{name}.lance")
    lance_mod.write_dataset(table, path)
    with pytest.raises(Exception) as excinfo:
        pa.table(nanolance.read_table(path))
    assert UNREADABLE_FROM_PYLANCE[name] in str(excinfo.value), (
        f"{name} now fails differently: {excinfo.value}. If it was implemented, move it out of "
        f"UNREADABLE_FROM_PYLANCE."
    )
