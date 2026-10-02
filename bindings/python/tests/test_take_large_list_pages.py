"""take() on list columns stored as a few very large MiniBlock pages.

pylance writes a list column as one MiniBlock page for as long as it can: Speech Commands' 4,890
one-second clips (list<int16>, 156 MB) are ONE page of ~76,000 chunks. take() used to decode the
whole page for every mini-batch -- 118 s for a shuffled epoch. It now reads the page's repetition
index (which rows finish in which chunk) and decodes only the chunks holding the requested rows,
turning a leading part-row into a row of its own that is then dropped. What has to hold: the rows
come back exactly as pylance's take returns them, for rows at page and chunk edges, rows spanning
many chunks, empty and null lists, lists of lists, of strings, of structs, and maps.

Full reads of such a page are windowed the same way (bounded memory: the 156 MB page was held
three to four times over), and checked here with windows forced small.
"""

from __future__ import annotations

import glob
import json
import os
import subprocess
import sys

import numpy as np
import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _shapes():
    rng = np.random.default_rng(11)
    n = 6_000
    lengths = rng.integers(0, 700, n)
    return {
        "long_int16": pa.array([rng.integers(-3_000, 3_000, 1_500, dtype=np.int16) for _ in range(2_000)],
                               pa.list_(pa.int16())),
        "nullable_int64": pa.array([None if i % 13 == 5 else rng.integers(0, 1 << 40, int(lengths[i])).tolist()
                                    for i in range(n)], pa.list_(pa.int64())),
        "item_nulls": pa.array([[None if (i + j) % 7 == 0 else float(j) for j in range(int(lengths[i]) // 3)]
                                for i in range(n)], pa.list_(pa.float64())),
        "list_of_lists": pa.array([[list(range(j % 9)) for j in range(int(lengths[i]) // 20)] if i % 11 else None
                                   for i in range(n)], pa.list_(pa.list_(pa.int32()))),
        "strings": pa.array([[f"w{(i * 31 + j) % 5_000}" for j in range(int(lengths[i]) // 10)] for i in range(n)],
                            pa.list_(pa.utf8())),
        "structs": pa.array([[{"a": j, "b": float(i)} for j in range(int(lengths[i]) // 8)] for i in range(n)],
                            pa.list_(pa.struct([("a", pa.int32()), ("b", pa.float32())]))),
        "map": pa.array([[(f"k{j}", i * j) for j in range(int(lengths[i]) // 12)] for i in range(n)],
                        pa.map_(pa.utf8(), pa.int64())),
    }


SHAPES = _shapes()


def _index_sets(n, seed):
    rng = np.random.default_rng(seed)
    return [
        [0],
        [n - 1],
        [0, n - 1],
        list(range(1_000, 1_040)),                    # a dense run: one window
        list(range(0, n, max(1, n // 50))),           # spread: many windows
        rng.choice(n, 64, replace=False).tolist(),    # a shuffled mini-batch
        [5, 5, 3, n - 2, 3],                          # repeats, out of order
    ]


# Columns this small are otherwise decoded once and kept for later takes (NANOLANCE_TAKE_CACHE_MB);
# the page windows are checked in a child process with that cache off, the cache in this one.
_CHILD = """
import sys, json, lance, nanolance, pyarrow as pa
path, sets, columns = sys.argv[1], json.loads(sys.argv[2]), json.loads(sys.argv[3])
ds = lance.dataset(path)
for idx in sets:
    got = pa.table(nanolance.take(path, idx, columns=columns))
    got.validate(full=True)
    assert got.to_pydict() == ds.take(idx, columns=columns).to_pydict(), idx[:6]
# A full read and a row range, with the page decoded in windows (NANOLANCE_LIST_WINDOW_KB).
whole = ds.to_table(columns=columns)
got = pa.table(nanolance.read_table(path, columns=columns))
got.validate(full=True)
assert got.to_pydict() == whole.to_pydict(), "full read"
n = whole.num_rows
part = pa.table(nanolance.read_table(path, columns=columns, offset=n // 3, length=n // 3))
assert part.to_pydict() == whole.slice(n // 3, n // 3).to_pydict(), "row range"
"""

# Every page windowed: 4 KiB windows, so pages over 16 KiB are split on scans too.
_WINDOWED = {"NANOLANCE_TAKE_CACHE_MB": "0", "NANOLANCE_LIST_WINDOW_KB": "4"}


@pytest.mark.parametrize("name", list(SHAPES))
def test_take_from_rusts_large_list_pages(lance_mod, tmp_path, name):
    table = pa.table({"id": pa.array(range(len(SHAPES[name])), pa.int64()), "c": SHAPES[name]})
    path = tmp_path / f"{name}.lance"
    lance_mod.write_dataset(table, str(path), data_storage_version="2.2")
    ds = lance_mod.dataset(str(path))
    sets = _index_sets(table.num_rows, seed=len(name))
    for idx in sets:
        got = pa.table(nanolance.take(path, idx, columns=["c"]))
        got.validate(full=True)
        expected = table.select(["c"]).take(pa.array(idx, pa.int64())).to_pydict()
        assert got.to_pydict() == expected, (name, idx[:6])
        assert ds.take(idx, columns=["c"]).to_pydict() == expected
    for knobs in ({"NANOLANCE_TAKE_CACHE_MB": "0"}, _WINDOWED):
        subprocess.run([sys.executable, "-c", _CHILD, str(path), json.dumps(sets), '["c"]'],
                       env={**os.environ, **knobs}, check=True)


def test_audio_clips_are_one_page_of_many_chunks(lance_mod, tmp_path):
    """The case this is for: check pylance still writes it that way, so the test above exercises the
    windowed path rather than whole-page decoding."""
    from lance.file import LanceFileReader

    table = pa.table({"c": SHAPES["long_int16"]})
    path = tmp_path / "clips.lance"
    lance_mod.write_dataset(table, str(path), data_storage_version="2.2")
    (data_file,) = glob.glob(f"{path}/data/*.lance")
    pages = LanceFileReader(data_file).metadata().columns[0].pages
    assert len(pages) <= 2, len(pages)
    encoding = str(pages[0].encoding)
    assert "MiniBlockLayout" in encoding and "repetition_index_depth: 1" in encoding


def test_take_from_nanolance_list_pages(lance_mod, tmp_path):
    """nanolance's own list pages carry the same repetition index, and are windowed the same way."""
    table = pa.table({name: col for name, col in SHAPES.items() if len(col) == 6_000})
    path = tmp_path / "ours.lance"
    nanolance.write_table(table, path)
    ds = lance_mod.dataset(str(path))
    for idx in _index_sets(table.num_rows, seed=3):
        got = pa.table(nanolance.take(path, idx))
        assert got.to_pydict() == table.take(pa.array(idx, pa.int64())).to_pydict()
        assert got.to_pydict() == ds.take(idx).to_pydict()
    sets = _index_sets(table.num_rows, seed=3)
    for knobs in ({"NANOLANCE_TAKE_CACHE_MB": "0"}, _WINDOWED):
        subprocess.run([sys.executable, "-c", _CHILD, str(path), json.dumps(sets), json.dumps(table.column_names)],
                       env={**os.environ, **knobs}, check=True)
