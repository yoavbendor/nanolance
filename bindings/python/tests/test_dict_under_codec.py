"""W1r: a dictionary column under lance-encoding:compression, data with runs included (`dict-rle` by default). pylance
12 keeps the dictionary and lays its indices out by the codec -- General{codec, ByteStreamSplit{Flat(32)}} for lz4 and
zstd, Flat(32) for none, never RLE -- with a General{LZ4, Flat(64)} dictionary for 64-bit values. nanolance writes the
same layout; both readers read it back."""

from __future__ import annotations

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


COLUMNS = {
    # Runs of 500: the default layout is a dictionary with RLE'd indices.
    "string_runs": lambda: pa.array([f"category-{i // 500 % 20}" for i in range(N)]),
    "int64_runs": lambda: pa.array([(i // 500 % 20) * 1_000_003 for i in range(N)], pa.int64()),
    "float64_runs": lambda: pa.array([(i // 500 % 20) * 1.5 for i in range(N)], pa.float64()),
    # Scattered: a dictionary with bit-packed indices by default.
    "int64_scattered": lambda: pa.array([(i * 7919 % 20) * 1_000_003 for i in range(N)], pa.int64()),
}


@pytest.mark.parametrize("codec", ["lz4", "zstd", "none"])
@pytest.mark.parametrize("name", sorted(COLUMNS))
def test_dictionary_under_codec_as_pylance(tmp_path, name, codec):
    lance = require_pylance()
    metadata = {"lance-encoding:compression": codec}
    if name.startswith("string"):
        # pylance's default wraps a string dictionary in LZ4, nanolance keeps it plain (W1g): name it on both sides.
        metadata["lance-encoding:dict-values-compression"] = "lz4"
    column = COLUMNS[name]()
    table = pa.table([column], schema=pa.schema([pa.field("v", column.type, metadata=metadata)]))
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))
    layout = _layout(ours)
    assert layout == _layout(theirs)
    assert "dict=" in layout[0] and "Rle" not in layout[0]
    assert nl_dataset(str(ours)).to_table().column("v") == table.column("v")
    assert lance.dataset(str(ours)).to_table().column("v") == table.column("v")
    picks = [0, 1, 499, 500, N // 2, N - 1]
    assert lance.dataset(str(ours)).take(picks).column("v") == table.take(picks).column("v")
    assert nl_dataset(str(ours)).take(picks).column("v") == table.take(picks).column("v")


@pytest.mark.parametrize("codec", ["lz4", "zstd", "none"])
def test_nullable_int64_under_codec_reads_back(tmp_path, codec):
    """Nulls keep the codec's own layout (no dictionary yet, see SPEC_GAPS W1r); the data is intact both ways."""
    lance = require_pylance()
    column = pa.array([None if i % 11 == 0 else (i // 500 % 20) * 1_000_003 for i in range(N)], pa.int64())
    table = pa.table([column], schema=pa.schema([pa.field("v", pa.int64(),
                                                           metadata={"lance-encoding:compression": codec})]))
    ours = tmp_path / "ours.lance"
    nl_write_dataset(table, str(ours))
    assert nl_dataset(str(ours)).to_table().column("v") == table.column("v")
    assert lance.dataset(str(ours)).to_table().column("v") == table.column("v")
