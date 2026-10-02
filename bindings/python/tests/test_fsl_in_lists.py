"""fixed_size_list values under a list: bounding boxes per object, embeddings per token.

COCO's objects carry bbox as fixed_size_list<float32, 4> inside list<struct> -- the shape Hugging Face
datasets give `Sequence(float32, length=4)`. nanolance refused it on write and on read. Lance stores
the items as FixedSizeList{items_per_value, values = Flat} in an ordinary list page; a null item's
elements are marked null too, which carries nothing the item's own validity does not.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _table(n=3_000):
    box = pa.list_(pa.float32(), 4)
    return pa.table({
        "boxes": pa.array([[[1.0, 2, 3, float(i)], [5, 6, 7, 8]] if i % 3 else ([] if i % 2 else None)
                           for i in range(n)], pa.list_(box)),
        "objects": pa.array([[{"bbox": [float(i), 2.0, 3.0, 4.0], "label": "x"}] if i % 4
                             else [{"bbox": None, "label": "y"}] for i in range(n)],
                            pa.list_(pa.struct([("bbox", box), ("label", pa.utf8())]))),
        "embeddings": pa.array([[[float(i + j)] * 8 for j in range(i % 3)] for i in range(n)],
                               pa.list_(pa.list_(pa.float64(), 8))),
    })


@pytest.mark.parametrize("writer", ["nanolance", "rust"])
def test_round_trip_both_ways(lance_mod, tmp_path, writer):
    table = _table()
    path = tmp_path / "fsl.lance"
    if writer == "nanolance":
        nanolance.write_table(table, path)
    else:
        lance_mod.write_dataset(table, str(path), data_storage_version="2.2")
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == table.to_pydict()
    assert lance_mod.dataset(str(path)).to_table().to_pydict() == table.to_pydict()
    idx = [2_999, 4, 1_000, 4]
    assert pa.table(nanolance.take(path, idx)).to_pydict() == table.take(pa.array(idx)).to_pydict()


def test_a_null_element_inside_a_valid_item_is_refused(tmp_path):
    values = pa.array([1.0, None, 3.0, 4.0], pa.float32())
    boxes = pa.FixedSizeListArray.from_arrays(values, 4)
    table = pa.table({"boxes": pa.ListArray.from_arrays(pa.array([0, 1], pa.int32()), boxes)})
    with pytest.raises(Exception, match="null element"):
        nanolance.write_table(table, tmp_path / "bad.lance")
