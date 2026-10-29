# SPDX-License-Identifier: Apache-2.0
"""The unenforced primary key (and clustering key) of a schema, both ways, against pylance 12.

Written by nanolance, read by pylance: the key's position and the merge_insert default. Written by pylance,
read by nanolance: the key the schema reports and the merge_insert default. The spellings and the write checks
are the ones pylance 12 makes; each expectation was measured with pylance 12.0.0 (see
docs/tasks/SPEC_GAPS.md, "Measured for W1e").
"""

import re

import pyarrow as pa
import pytest

from tests.support import require_pylance

import nanolance.lance as nl

KEY = "lance-schema:unenforced-primary-key"
POSITION = "lance-schema:unenforced-primary-key:position"
CLUSTERING = "lance-schema:unenforced-clustering-key:position"


def _table(meta=None, *, nullable=False):
    schema = pa.schema([
        pa.field("id", pa.int64(), nullable=nullable, metadata=meta),
        pa.field("value", pa.int64()),
    ])
    return pa.table({"id": [1, 2, 3], "value": [10, 20, 30]}, schema=schema)


def _merged(lib, path, source):
    """The default merge_insert (no ``on``): update the matched rows, insert the rest."""
    ds = lib.dataset(path)
    ds.merge_insert().when_matched_update_all().when_not_matched_insert_all().execute(source)
    return lib.dataset(path).to_table().sort_by("id").column("value").to_pylist()


def _source():
    return pa.table({"id": [2, 9], "value": [200, 900]})


def _key_positions(lib, path, field="id"):
    ds = lib.dataset(path)
    lf = next(f for f in ds.lance_schema.fields() if f.name() == field)
    return lf.unenforced_primary_key_position(), lf.unenforced_clustering_key_position()


@pytest.mark.parametrize("meta", [
    {KEY: "true"},                  # the flag alone: a key, ordered by field id (no position shown)
    {POSITION: "1"},                # a position: a key at 1
    {POSITION: "2", KEY: "true"},   # both: the position
])
def test_nanolance_key_is_a_pylance_key(tmp_path, meta):
    lance = require_pylance()
    path = str(tmp_path / "n.lance")
    nl.write_dataset(_table({k.encode(): v.encode() for k, v in meta.items()}), path)
    # pylance merges on the key without `on`, and the update lands on the row with the same id.
    assert _merged(lance, path, _source()) == [10, 200, 30, 900]
    position = _key_positions(lance, path)[0]
    assert position == (int(meta[POSITION]) if POSITION in meta else None)


def test_nanolance_clustering_position_is_a_pylance_clustering_key(tmp_path):
    lance = require_pylance()
    path = str(tmp_path / "n.lance")
    nl.write_dataset(_table({CLUSTERING.encode(): b"1"}), path)
    assert _key_positions(lance, path) == (None, 1)
    with pytest.raises(ValueError, match="join keys"):
        _merged(nl, path, _source())  # a clustering key is not a primary key


@pytest.mark.parametrize("meta, key", [
    ({KEY: "true"}, True),
    ({KEY: "TRUE"}, True),
    ({KEY: "yes"}, True),
    ({KEY: "on"}, True),
    ({KEY: "y"}, True),
    ({KEY: "1"}, True),
    ({KEY: "0"}, False),
    ({KEY: "no"}, False),
    ({KEY: "false"}, False),
    ({KEY: ""}, False),
    ({POSITION: "abc", KEY: "true"}, True),   # a position that does not parse falls back to the flag
    ({POSITION: "-1"}, False),                 # and one that does not parse with no flag is no key
])
def test_flag_spellings_match_pylance(tmp_path, meta, key):
    lance = require_pylance()
    path = str(tmp_path / "p.lance")
    lance.write_dataset(_table({k.encode(): v.encode() for k, v in meta.items()}), path)
    # The same schema through nanolance's merge_insert default must agree with pylance's.
    if key:
        assert _merged(nl, path, _source()) == [10, 200, 30, 900]
    else:
        with pytest.raises(ValueError, match="join keys"):
            _merged(nl, path, _source())


@pytest.mark.parametrize("meta, position", [
    ({POSITION: "3", KEY: "false"}, 3),  # a position that parses wins over the flag
    ({POSITION: "0", KEY: "false"}, None),  # position 0 is a key (the flag is written), shown as no position
    ({POSITION: "+007"}, 7),
    ({POSITION: "4294967295"}, 4294967295),
])
def test_positions_match_pylance(tmp_path, meta, position):
    path = str(tmp_path / "p.lance")
    require_pylance().write_dataset(_table({k.encode(): v.encode() for k, v in meta.items()}), path)
    assert nl.dataset(path).lance_schema.fields()[0].unenforced_primary_key_position() == position
    assert _merged(nl, path, _source()) == [10, 200, 30, 900]


def test_nanolance_reads_pylance_key_positions(tmp_path):
    lance = require_pylance()
    path = str(tmp_path / "p.lance")
    lance.write_dataset(_table({POSITION.encode(): b"2", KEY.encode(): b"true"}), path)
    fields = nl.dataset(path).lance_schema.fields()
    assert fields[0].unenforced_primary_key_position() == 2
    assert fields[0].is_unenforced_primary_key()
    assert not fields[1].is_unenforced_primary_key()


def test_arrow_metadata_is_returned_as_written(tmp_path):
    # pylance returns the key metadata exactly as it was written, and adds nothing (measured for both spellings).
    lance = require_pylance()
    for name, meta in (("bool", {KEY: "true"}), ("position", {POSITION: "1"})):
        path = str(tmp_path / f"{name}.lance")
        nl.write_dataset(_table({k.encode(): v.encode() for k, v in meta.items()}), path)
        expected = {k.encode(): v.encode() for k, v in meta.items()}
        assert lance.dataset(path).schema.field("id").metadata == expected
        assert nl.dataset(path).schema.field("id").metadata == expected


def test_nullable_key_is_refused_like_pylance(tmp_path):
    lance = require_pylance()
    message = ("Primary key column and all its ancestors must not be nullable: "
               "Field(id=0, name=id, type=int64)")
    for lib, name in ((nl, "n"), (lance, "p")):
        path = str(tmp_path / f"{name}.lance")
        with pytest.raises(Exception, match=re.escape(message)):
            lib.write_dataset(_table({KEY.encode(): b"true"}, nullable=True), path)


def test_list_key_is_refused_like_pylance(tmp_path):
    lance = require_pylance()
    item = pa.list_(pa.field("item", pa.int32()))
    schema = pa.schema([pa.field("l", item, nullable=False, metadata={KEY.encode(): b"true"})])
    table = pa.table({"l": pa.array([[1], [2], [3]], item)}, schema=schema)
    message = ("Primary key column must be a leaf: "
               "Field(id=0, name=l, type=list, children=[Field(id=1, name=item, type=int32), ])")
    for lib, name in ((nl, "n"), (lance, "p")):
        with pytest.raises(Exception, match=re.escape(message)):
            lib.write_dataset(table, str(tmp_path / f"{name}.lance"))


def test_non_null_leaf_in_non_null_struct_is_a_key(tmp_path):
    lance = require_pylance()
    leaf = pa.struct([pa.field("id", pa.int64(), nullable=False, metadata={KEY.encode(): b"true"})])
    schema = pa.schema([pa.field("s", leaf, nullable=False)])
    table = pa.table({"s": pa.array([{"id": 1}, {"id": 2}, {"id": 3}], leaf)}, schema=schema)
    for lib, name in ((nl, "n"), (lance, "p")):
        path = str(tmp_path / f"{name}.lance")
        lib.write_dataset(table, path)
        assert lance.dataset(path).to_table().column("s").combine_chunks().field("id").to_pylist() == [1, 2, 3]
