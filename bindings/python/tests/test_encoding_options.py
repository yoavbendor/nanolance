"""`lance-encoding:` options that move nanolance's structural choices: rle-threshold, dict-divisor,
dict-size-ratio and dict-values-compression (W1g).

Layouts are pylance 12.0.0's, measured on one 50,000-row column per case with `build/nlance-pagelayout`
(docs/tasks/SPEC_GAPS.md, "W1g measured"). Where nanolance's layout equals pylance's the test asserts it; where
pylance also uses a structure nanolance lacks (an integer dictionary) only the option's own effect is asserted.
Each case also checks that pylance reads nanolance's output and that nanolance reads pylance's.
"""

from __future__ import annotations

import numpy as np
import pyarrow as pa
import pytest

from nanolance.lance.dataset import dataset as nl_dataset
from nanolance.lance.dataset import write_dataset as nl_write_dataset
from tests.support import require_pylance
from tests.test_column_codec import ROWS, _layout

RNG = np.random.default_rng(1)


def _runs(length: int) -> pa.Array:
    values = RNG.integers(0, 1 << 40, ROWS // length + 1, dtype=np.int64)
    return pa.array(np.repeat(values, length)[:ROWS])


def _strings(distinct: int) -> pa.Array:
    words = [f"unique-{i:08d}-" + "x" * (i % 7) for i in range(distinct)]
    return pa.array([words[i] for i in RNG.integers(0, distinct, ROWS)])


def _table(arr: pa.Array, options: dict[str, str]) -> pa.Table:
    metadata = {f"lance-encoding:{k}".encode(): v.encode() for k, v in options.items()}
    return pa.table({"c": arr}, schema=pa.schema([pa.field("c", arr.type, metadata=metadata)]))


def _check_against_pylance(lance, tmp_path, table: pa.Table, *, layout: bool = True) -> None:
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))
    assert lance.dataset(str(ours)).to_table() == table
    assert nl_dataset(str(ours)).to_table() == table
    assert nl_dataset(str(theirs)).to_table() == table  # pylance-written pages read in nanolance
    if layout:
        assert _layout(ours) == _layout(theirs)


def _has_rle(path) -> bool:
    return any("Rle{" in line for line in _layout(path))


# rle-threshold: pylance takes RLE only when the run count is below threshold * rows; 0 turns RLE off.
# Layout parity is not asserted for integer runs: pylance also dictionary-encodes these columns (1,000 to
# 10,000 distinct values) and nanolance has no integer dictionary (SPEC_GAPS.md, W1g). The option's effect on
# nanolance's own RLE choice is asserted instead.
def test_rle_threshold_zero_turns_rle_off(tmp_path):
    lance = require_pylance()
    default = tmp_path / "default.lance"
    off = tmp_path / "off.lance"
    runs = _runs(50)
    nl_write_dataset(_table(runs, {}), str(default))
    nl_write_dataset(_table(runs, {"rle-threshold": "0"}), str(off))
    assert _has_rle(default)
    assert not _has_rle(off)
    assert lance.dataset(str(off)).to_table() == _table(runs, {})


@pytest.mark.parametrize("threshold", ["0.1", "0.5", "1"])
def test_rle_threshold_above_zero_keeps_rle_for_runs_under_the_threshold(tmp_path, threshold):
    lance = require_pylance()
    # Runs of 50 are a run ratio of 0.02: below every positive threshold tried here, so RLE stays.
    table = _table(_runs(50), {"rle-threshold": threshold})
    path = tmp_path / "ours.lance"
    nl_write_dataset(table, str(path))
    assert _has_rle(path)
    assert lance.dataset(str(path)).to_table() == table
    assert nl_dataset(str(path)).to_table() == table


# dict-divisor: a dictionary may hold at most rows / divisor distinct values (divisor 0 reads as 1).
# dict-values-compression=none keeps nanolance's plain dictionary buffer (pylance's default is LZ4; not changed).
@pytest.mark.parametrize("divisor", ["1000", "1", "0"])
def test_dict_divisor_matches_pylance_on_strings(tmp_path, divisor):
    lance = require_pylance()
    _check_against_pylance(
        lance, tmp_path, _table(_strings(5000), {"dict-divisor": divisor, "dict-values-compression": "none"})
    )


def test_dict_divisor_1000_on_ints_matches_pylance(tmp_path):
    lance = require_pylance()
    ints = pa.array(RNG.integers(0, 1000, ROWS, dtype=np.int64))
    # 1,000 distinct values exceed rows / 1000, so pylance writes plain InlineBitpacking(64).
    _check_against_pylance(lance, tmp_path, _table(ints, {"dict-divisor": "1000"}))


def test_dict_size_ratio_0_1_takes_fsst_as_pylance_does(tmp_path):
    lance = require_pylance()
    _check_against_pylance(lance, tmp_path, _table(_strings(5000), {"dict-divisor": "1", "dict-size-ratio": "0.1"}))


def test_dict_size_ratio_that_does_not_parse_keeps_the_default_values(tmp_path):
    lance = require_pylance()
    # pylance keeps its default for an unparsable ratio. The default layout differs from pylance's on this column
    # (its sample projection rejects the dictionary, nanolance's full count keeps it), a default that is not
    # changed here, so only the values and readability are asserted.
    _check_against_pylance(lance, tmp_path, _table(_strings(5000), {"dict-size-ratio": "abc"}), layout=False)


@pytest.mark.parametrize("ratio", ["0.0", "-0.5", "1.5", "2"])
def test_dict_size_ratio_outside_zero_one_is_refused_by_name(tmp_path, ratio):
    # pylance panics ("not in the range (0, 1]"); nanolance refuses with an error that names the field.
    with pytest.raises(Exception, match=r"dict-size-ratio"):
        nl_write_dataset(_table(_strings(5000), {"dict-size-ratio": ratio}), str(tmp_path / "bad.lance"))


@pytest.mark.parametrize("codec", ["lz4", "zstd", "none"])
def test_dict_values_compression_matches_pylance(tmp_path, codec):
    lance = require_pylance()
    # divisor 1 makes this column dictionary-encoded, so the option has a dictionary buffer to wrap.
    _check_against_pylance(
        lance, tmp_path, _table(_strings(5000), {"dict-divisor": "1", "dict-values-compression": codec})
    )


def test_dict_values_compression_level_is_accepted(tmp_path):
    lance = require_pylance()
    table = _table(
        _strings(5000),
        {"dict-divisor": "1", "dict-values-compression": "zstd", "dict-values-compression-level": "9"},
    )
    _check_against_pylance(lance, tmp_path, table)


def test_unknown_dict_values_compression_is_refused_by_name(tmp_path):
    # pylance refuses brotli on a dictionary column: "Unknown compression scheme: brotli".
    table = _table(_strings(5000), {"dict-divisor": "1", "dict-values-compression": "brotli"})
    with pytest.raises(Exception, match=r"dict-values-compression"):
        nl_write_dataset(table, str(tmp_path / "bad.lance"))


def test_unknown_dict_values_compression_is_ignored_off_dictionaries(tmp_path):
    # pylance accepts any dict-values-compression on a column it does not dictionary-encode, and ignores it.
    lance = require_pylance()
    rng = np.random.default_rng(3)
    for arr in (pa.array(rng.integers(0, 2**40, ROWS)), pa.array([f"u{i}" for i in range(ROWS)])):
        table = _table(arr, {"dict-values-compression": "brotli"})
        ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
        nl_write_dataset(table, str(ours), mode="overwrite")
        lance.write_dataset(table, str(theirs), mode="overwrite")
        assert lance.dataset(str(ours)).to_table() == lance.dataset(str(theirs)).to_table() == table


def test_dictionary_buffer_is_plain_without_the_option(tmp_path):
    # Without the option nanolance keeps its plain dictionary buffer (pylance's LZ4 default is not adopted).
    table = _table(_strings(5000), {"dict-divisor": "1"})
    ours = tmp_path / "ours.lance"
    nl_write_dataset(table, str(ours))
    assert any("dict=Variable" in line for line in _layout(ours))
    assert require_pylance().dataset(str(ours)).to_table() == table


# W1r: dictionary indices under lance-encoding:compression. pylance writes them as
# General{codec, ByteStreamSplit{Flat(32)}} for lz4 / zstd and Flat(32) for none (SPEC_GAPS.md, "W1h measured again");
# the dictionary buffer follows dict-values-compression, as before. Integer dictionaries are W1p; the int64 and float64
# cases here read pylance's dictionaries and assert the indices' encoding only.
INDEX_LAYOUT = {
    "lz4": "General{LZ4,ByteStreamSplit{Flat(32)}}",
    "zstd": "General{ZSTD,ByteStreamSplit{Flat(32)}}",
    "none": "Flat(32)",
}


def _index_layouts(path) -> str:
    return " ".join(_layout(path))


@pytest.mark.parametrize("codec", ["lz4", "zstd", "none"])
def test_pylance_string_dictionary_indices_read_and_take(tmp_path, codec):
    lance = require_pylance()
    table = _table(_strings(1000), {"compression": codec, "dict-divisor": "1"})
    path = tmp_path / "theirs.lance"
    lance.write_dataset(table, str(path))
    assert INDEX_LAYOUT[codec] in _index_layouts(path)
    ours = nl_dataset(str(path))
    assert ours.to_table() == table
    rows = [0, 49_999, 12_345, 7, 7]
    assert ours.take(rows).to_pylist() == table.take(rows).to_pylist()


@pytest.mark.parametrize("codec", ["lz4", "zstd", "none"])
def test_pylance_int64_dictionary_indices_read_and_take(tmp_path, codec):
    lance = require_pylance()
    values = RNG.integers(0, 1 << 40, 1000, dtype=np.int64)
    table = _table(pa.array(values[RNG.integers(0, 1000, ROWS)]), {"compression": codec})
    path = tmp_path / "theirs.lance"
    lance.write_dataset(table, str(path))
    assert INDEX_LAYOUT[codec] in _index_layouts(path)
    ours = nl_dataset(str(path))
    assert ours.to_table() == table
    rows = [0, 49_999, 12_345]
    assert ours.take(rows).to_pylist() == table.take(rows).to_pylist()


@pytest.mark.parametrize("codec", ["lz4", "zstd", "none"])
def test_pylance_float64_dictionary_indices_read_and_take(tmp_path, codec):
    lance = require_pylance()
    values = RNG.random(1000)
    table = _table(pa.array(values[RNG.integers(0, 1000, ROWS)]), {"compression": codec})
    path = tmp_path / "theirs.lance"
    lance.write_dataset(table, str(path))
    assert INDEX_LAYOUT[codec] in _index_layouts(path)
    ours = nl_dataset(str(path))
    assert ours.to_table() == table
    rows = [0, 49_999, 12_345]
    assert ours.take(rows).to_pylist() == table.take(rows).to_pylist()


@pytest.mark.parametrize("codec", ["lz4", "zstd", "none"])
def test_nanolance_string_dictionary_indices_match_pylance_layout(tmp_path, codec):
    lance = require_pylance()
    # dict-values-compression=none: pylance's default wraps the dictionary in LZ4, which nanolance does not adopt
    # (W1g); with the plain buffer on both sides the layouts are equal.
    table = _table(_strings(1000), {"compression": codec, "dict-divisor": "1", "dict-values-compression": "none"})
    ours = tmp_path / "ours.lance"
    theirs = tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))
    assert INDEX_LAYOUT[codec] in _index_layouts(ours)
    assert _layout(ours) == _layout(theirs)
    assert lance.dataset(str(ours)).to_table() == table
    assert nl_dataset(str(ours)).to_table() == table
    assert nl_dataset(str(theirs)).to_table() == table


@pytest.mark.parametrize("codec", ["lz4", "zstd", "none"])
def test_column_option_survives_the_writer_wide_compression_flag(tmp_path, codec):
    # The writer-wide compression flag tags every string column zstd. On a dictionary column the caller's own
    # option must win, so the indices are the option's layout.
    import nanolance

    lance = require_pylance()
    table = _table(_strings(1000), {"compression": codec, "dict-divisor": "1", "dict-values-compression": "none"})
    ours = tmp_path / "ours.lance"
    nanolance.write_table(table, str(ours), compression=True)
    assert INDEX_LAYOUT[codec] in _index_layouts(ours)
    assert lance.dataset(str(ours)).to_table() == table
    assert nl_dataset(str(ours)).to_table() == table
