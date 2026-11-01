"""128-bit dictionaries (W1p): pylance 12 dictionary-encodes decimal128 and fixed_size_binary(16) columns as it does
64-bit ones (Lance's dictionary_encode takes both widths) -- the same sample, threshold and budget at 16 bytes an entry,
u32 indices (bit-packed, RLE'd with runs, or the column codec's layout), and the dictionary stored by the general codec
(never bitpacked). nanolance writes them the same; both readers read them back. Also pins Lance's exact sample (block b
starts at b * rows / 32), which decides `i % 25,000` over 50,000 rows for 64-bit columns too."""

from __future__ import annotations

import decimal
import re
import subprocess
from pathlib import Path

import pyarrow as pa
import pytest

from nanolance.lance.dataset import dataset as nl_dataset
from nanolance.lance.dataset import write_dataset as nl_write_dataset
from tests.support import require_pylance

PAGELAYOUT = Path(__file__).resolve().parents[3] / "build" / "nlance-pagelayout"
N = 50_000


def _layout(path: Path) -> list[str]:
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    return sorted(re.sub(r",rows=\d+", "", line.split("  ", 1)[1]) for line in out.splitlines() if "  " in line)


def _dec(values, precision=20, scale=2):
    return pa.array([None if v is None else decimal.Decimal(v) for v in values], pa.decimal128(precision, scale))


CASES = {
    "dec_scattered": (lambda: _dec(i * 7919 % 20 for i in range(N)), {}),
    "dec_runs": (lambda: _dec(i // 500 % 20 for i in range(N)), {}),
    "dec_nulls": (lambda: _dec(None if i % 7 == 0 else i * 7919 % 20 for i in range(N)), {}),
    "dec38_2000": (lambda: _dec((i * 7919 % 2000 for i in range(N)), 38, 4), {}),
    "dec_unique": (lambda: _dec(range(N)), {}),
    "dec_15000": (lambda: _dec(i % 15_000 for i in range(N)), {}),
    "dec_24000": (lambda: _dec(i % 24_000 for i in range(N)), {}),
    "dec_25000": (lambda: _dec(i % 25_000 for i in range(N)), {}),
    "dec_26000": (lambda: _dec(i % 26_000 for i in range(N)), {}),
    "dec_small": (lambda: _dec(i % 3 for i in range(500)), {}),
    "fsb16": (lambda: pa.array([bytes([i * 7919 % 20]) * 16 for i in range(N)], pa.binary(16)), {}),
    "dec_values_none": (lambda: _dec(i % 20 for i in range(N)), {"lance-encoding:dict-values-compression": "none"}),
    "dec_values_zstd": (lambda: _dec(i % 20 for i in range(N)), {"lance-encoding:dict-values-compression": "zstd"}),
    "dec_codec_lz4": (lambda: _dec(i % 20 for i in range(N)), {"lance-encoding:compression": "lz4"}),
    "dec_codec_zstd": (lambda: _dec(i % 20 for i in range(N)), {"lance-encoding:compression": "zstd"}),
    "dec_codec_none": (lambda: _dec(i % 20 for i in range(N)), {"lance-encoding:compression": "none"}),
    "i64_values_zstd": (lambda: pa.array([(i % 20) * 1_000_003 for i in range(N)], pa.int64()),
                        {"lance-encoding:dict-values-compression": "zstd"}),
    "i64_25000": (lambda: pa.array([(i % 25_000) * 1_000_003 for i in range(N)], pa.int64()), {}),
    "i64_12500": (lambda: pa.array([(i % 12_500) * 1_000_003 for i in range(N)], pa.int64()), {}),
}


@pytest.mark.parametrize("name", sorted(CASES))
def test_dictionary_as_pylance(tmp_path, name):
    lance = require_pylance()
    make, metadata = CASES[name]
    column = make()
    table = pa.table([column], schema=pa.schema([pa.field("v", column.type, metadata=metadata)]))
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))
    assert _layout(ours) == _layout(theirs)
    assert nl_dataset(str(ours)).to_table() == table
    assert lance.dataset(str(ours)).to_table() == table
    picks = [0, 1, 7, len(column) // 2, len(column) - 1]
    assert lance.dataset(str(ours)).take(picks) == table.take(picks)
    assert nl_dataset(str(ours)).take(picks) == table.take(picks)
    assert nl_dataset(str(ours)).to_table(filter="v IS NULL").num_rows == column.null_count
