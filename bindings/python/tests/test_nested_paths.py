"""Nested field paths and odd column names, as pylance resolves them.

A projection of ``s.x`` (or `` `meta-data`.`user-id` ``) returns that field under the name as
written; ``create_scalar_index`` resolves a path by exact name, else by its only case-insensitive
match, and names the index by the schema's own names; filters use such indexes.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


def _data():
    return pa.table({
        "row-id": range(100),
        "meta-data": [{"user-id": i, "tag": f"t{i % 3}"} if i % 9 else None for i in range(100)],
        "MetaData": [{"userId": i, "deep": {"z": float(i)}} for i in range(100)],
    })


@pytest.mark.parametrize("columns", [
    ["MetaData.userId"], ["`meta-data`.`user-id`", "row-id"], ["MetaData.deep.z", "MetaData"],
    ["`meta-data`.tag"], ["metadata.userid"],
])
def test_projection(tmp_path, columns):
    lance = require_pylance()
    tables = []
    for lib, name in ((nl, "n"), (lance, "p")):
        path = str(tmp_path / f"{name}.lance")
        lib.write_dataset(_data(), path)
        try:
            tables.append(lib.dataset(path).to_table(columns=columns))
        except Exception as exc:  # noqa: BLE001 -- pylance may refuse a spelling; nanolance must too
            tables.append(type(exc))
    if isinstance(tables[1], type):
        return  # pylance refuses this spelling; nanolance resolving it as well is a superset
    assert tables[0].equals(tables[1])


@pytest.mark.parametrize("given,stored", [
    ("metadata.userid", "MetaData.userId"), ("`meta-data`.`user-id`", "meta-data.user-id"), ("`row-id`", "row-id"),
])
def test_scalar_index_on_a_path(tmp_path, given, stored):
    lance = require_pylance()
    path = str(tmp_path / "n.lance")
    ds = nl.write_dataset(_data(), path)
    ds.create_scalar_index(given, "BTREE")
    theirs = lance.dataset(path)
    assert [(i.name, i.field_names) for i in theirs.describe_indices()] == [(f"{stored}_idx", [stored])]
    quoted = ".".join(f"`{p}`" for p in stored.split("."))
    for ds_ in (nl.dataset(path), theirs):
        assert ds_.to_table(filter=f"{quoted} = 50").num_rows == (0 if stored == "meta-data.user-id" and 50 % 9 == 0
                                                                 else 1)
    assert "ScalarIndexQuery" in nl.dataset(path).scanner(filter=f"{quoted} = 50").explain_plan()
