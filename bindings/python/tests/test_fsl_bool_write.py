"""fixed_size_list<bool, N> written as pylance 12 writes it: FixedSizeList{N x Flat(1)}, N bits a row packed across
the chunk; with element validity (FixedSizeList{..., validity}, two buffers a chunk) whenever the Arrow child holds a
null, which pyarrow also puts under a null row. Both readers read it back; the layout text equals pylance's (page row
counts aside under a list, where nanolance cuts pages as it does for every nested leaf)."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import numpy as np
import pyarrow as pa
import pytest

from nanolance.lance.dataset import dataset as nl_dataset
from nanolance.lance.dataset import write_dataset as nl_write_dataset
from tests.support import require_pylance

PAGELAYOUT = Path(__file__).resolve().parents[3] / "build" / "nlance-pagelayout"


def _layout(path: Path) -> list[str]:
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    return sorted({re.sub(r",rows=\d+", "", line.split("  ", 1)[1]) for line in out.splitlines() if "  " in line})


def _rows(n, rows, null_row=0, null_el=0, seed=0):
    rng = np.random.default_rng(seed)
    out = []
    for i in range(rows):
        if null_row and i % null_row == 0:
            out.append(None)
            continue
        out.append([None if null_el and (i * n + j) % null_el == 0 else bool(x)
                    for j, x in enumerate(rng.integers(0, 2, n))])
    return out


CASES = {
    "n4": lambda: pa.table({"v": pa.array(_rows(4, 3000), pa.list_(pa.bool_(), 4))}),
    "n4_null_rows": lambda: pa.table({"v": pa.array(_rows(4, 3000, 7), pa.list_(pa.bool_(), 4))}),
    "n4_null_elements": lambda: pa.table({"v": pa.array(_rows(4, 3000, 0, 5), pa.list_(pa.bool_(), 4))}),
    "n200": lambda: pa.table({"v": pa.array(_rows(200, 3000), pa.list_(pa.bool_(), 200))}),
    "n3000": lambda: pa.table({"v": pa.array(_rows(3000, 300), pa.list_(pa.bool_(), 3000))}),
    "many_chunks": lambda: pa.table({"v": pa.array(_rows(3, 300_000, 9), pa.list_(pa.bool_(), 3))}),
    "in_list": lambda: pa.table({"v": pa.array([[[True, False]] * (i % 3) for i in range(3000)],
                                               pa.list_(pa.list_(pa.bool_(), 2)))}),
    "in_struct": lambda: pa.table({"s": pa.array([{"f": [i % 2 == 0, True, False]} for i in range(3000)],
                                                 pa.struct([("f", pa.list_(pa.bool_(), 3))]))}),
}


@pytest.mark.parametrize("name", sorted(CASES))
def test_fsl_bool_written_as_pylance(tmp_path, name):
    lance = require_pylance()
    table = CASES[name]()
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl_write_dataset(table, str(ours))
    lance.write_dataset(table, str(theirs))
    assert _layout(ours) == _layout(theirs)
    assert nl_dataset(str(ours)).to_table() == table
    assert lance.dataset(str(ours)).to_table() == table
    picks = [0, 1, table.num_rows // 2, table.num_rows - 1]
    assert lance.dataset(str(ours)).take(picks) == table.take(picks)


def test_element_validity_follows_the_child_nulls(tmp_path):
    """A row mask alone (the child has no nulls) writes no element validity, as pylance; pyarrow's nulls under a
    null row do (both measured on pylance 12.0.0, float32 and bool alike)."""
    lance = require_pylance()
    masked = pa.FixedSizeListArray.from_arrays(pa.array(np.arange(12000, dtype=np.float32)), 4,
                                               mask=pa.array([i % 7 == 0 for i in range(3000)]))
    implied = pa.array([None if i % 7 == 0 else [1.0, 2.0, 3.0, 4.0] for i in range(3000)], pa.list_(pa.float32(), 4))
    for name, column in (("masked", masked), ("implied", implied)):
        table = pa.table({"v": column})
        ours, theirs = tmp_path / f"{name}_ours.lance", tmp_path / f"{name}_theirs.lance"
        nl_write_dataset(table, str(ours))
        lance.write_dataset(table, str(theirs))
        assert _layout(ours) == _layout(theirs), name
        assert ("validity" in _layout(ours)[0]) == (name == "implied")
        assert lance.dataset(str(ours)).to_table() == table
