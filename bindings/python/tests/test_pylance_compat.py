"""nanolance.lance, the pylance-compatible API, checked against pylance itself.

pylance's own test suite runs against nanolance.lance too (tools/pylance_suite.py, with the list of
tests it passes in tests/pylance_suite/expected_pass.txt). This file keeps what that suite cannot:
every result here is compared with what pylance returns for the same files, in both directions --
datasets nanolance writes read by pylance, and pylance's read by nanolance -- plus the bugs the
suite found in nanolance's core, pinned where they were fixed.
"""

from __future__ import annotations

import sys
import uuid

import numpy as np
import pyarrow as pa
import pytest

import nanolance
import nanolance.lance as nl
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def table(n=20, start=0):
    return pa.table({
        "id": pa.array(range(start, start + n), pa.int64()),
        "name": pa.array([f"row {i}" for i in range(start, start + n)]),
        "score": pa.array(np.linspace(0, 1, n), pa.float64()),
    })


# ── what nanolance writes, pylance reads, and back ─────────────────────────────────────────────────


@pytest.mark.parametrize("writer", ["nanolance", "pylance"])
def test_versions_and_modes_agree(lance, tmp_path, writer):
    mod = nl if writer == "nanolance" else lance
    uri = str(tmp_path / "ds")
    mod.write_dataset(table(10), uri)
    mod.write_dataset(table(5, 10), uri, mode="append")
    mod.write_dataset(table(3, 100), uri, mode="overwrite")
    with pytest.raises(OSError, match="already exists"):
        mod.write_dataset(table(1), uri, mode="create")
    for reader in (nl, lance):
        ds = reader.dataset(uri)
        assert ds.version == 3
        assert [v["version"] for v in ds.versions()] == [1, 2, 3]
        assert ds.to_table() == table(3, 100)
        assert reader.dataset(uri, version=2).to_table() == pa.concat_tables([table(10), table(5, 10)])
        assert ds.checkout_version(1).count_rows() == 10
        assert [f.fragment_id for f in reader.dataset(uri, version=2).get_fragments()] == [0, 1]


def test_one_version_per_write(lance, tmp_path):
    uri = str(tmp_path / "ds")
    ds = nl.write_dataset(table(100), uri, max_rows_per_file=30)
    assert ds.version == 1
    assert [f.count_rows() for f in ds.get_fragments()] == [30, 30, 30, 10]
    assert lance.dataset(uri).to_table() == table(100)
    assert lance.dataset(uri).versions()[0]["metadata"] == ds.versions()[0]["metadata"]


@pytest.mark.parametrize("writer", ["nanolance", "pylance"])
def test_scans_agree(lance, tmp_path, writer):
    mod = nl if writer == "nanolance" else lance
    uri = str(tmp_path / "ds")
    mod.write_dataset(table(50), uri, max_rows_per_file=20)
    ours, theirs = nl.dataset(uri), lance.dataset(uri)
    for kwargs in [
        {},
        {"columns": ["score", "id"]},
        {"limit": 7, "offset": 13},
        {"offset": 45},
        {"with_row_id": True},
        {"with_row_address": True, "columns": ["name"]},
        {"columns": ["_rowaddr", "id", "_rowid"]},
        {"columns": ["_rowoffset"], "offset": 3, "limit": 4},
    ]:
        assert ours.to_table(**kwargs) == theirs.to_table(**kwargs), kwargs
    frag = ours.get_fragments()[1]
    assert frag.to_table() == theirs.get_fragments()[1].to_table()
    assert (ours.scanner(fragments=[frag]).to_table()
            == theirs.scanner(fragments=[theirs.get_fragments()[1]]).to_table())
    assert ours.take([49, 0, 21, 0]) == theirs.take([49, 0, 21, 0])
    assert ours.head(3) == theirs.head(3)
    assert ours.count_rows() == theirs.count_rows() == 50
    assert ours.schema == theirs.schema


def test_row_ids_after_deletes_and_appends(lance, tmp_path):
    """Deleted rows stay deleted when nanolance appends -- a fragment's deletion file used to be
    dropped when nanolance rewrote the manifest, bringing the rows back -- and row ids (fragment id
    << 32 | offset) are pylance's."""
    uri = str(tmp_path / "ds")
    lance.write_dataset(pa.table({"a": list(range(10))}), uri).delete("a < 3")
    nanolance.write_table(pa.table({"a": [100]}), uri, append=True)
    nl.write_dataset(pa.table({"a": [200]}), uri, mode="append")
    want = [3, 4, 5, 6, 7, 8, 9, 100, 200]
    assert lance.dataset(uri).to_table()["a"].to_pylist() == want
    assert pa.table(nanolance.read_table(uri))["a"].to_pylist() == want
    assert nl.dataset(uri).to_table(with_row_id=True) == lance.dataset(uri).to_table(with_row_id=True)
    rows = [3, 2**32, 2 * 2**32]
    assert nl.dataset(uri)._take_rows(rows) == lance.dataset(uri)._take_rows(rows)


def test_table_config_and_metadata_survive(lance, tmp_path):
    uri = str(tmp_path / "ds")
    ds = nl.write_dataset(table(4), uri)
    ds.update_config({"team": "vision"})
    ds.update_metadata({"owner": "me"})
    nl.write_dataset(table(2, 4), uri, mode="append")
    for reader in (nl, lance):
        got = reader.dataset(uri)
        assert got.config()["team"] == "vision"
        assert got.metadata["owner"] == "me" if reader is nl else True
    lance.dataset(uri).update_config({"more": "yes"})
    nl.write_dataset(table(1, 6), uri, mode="append")
    assert nl.dataset(uri).config() == lance.dataset(uri).config()


def test_restore(lance, tmp_path):
    uri = str(tmp_path / "ds")
    nl.write_dataset(table(3), uri)
    nl.write_dataset(table(9), uri, mode="overwrite")
    nl.dataset(uri, version=1).restore()
    assert lance.dataset(uri).version == 3
    assert lance.dataset(uri).to_table() == table(3)


def test_memory_uri():
    ds = nl.write_dataset(table(3), "memory://compat-test")
    assert nl.dataset("memory://compat-test").to_table() == table(3)
    assert ds.version == 1


def test_unsupported_is_loud(tmp_path):
    ds = nl.write_dataset(table(3), str(tmp_path / "ds"))
    with pytest.raises(NotImplementedError):
        ds.create_index("score", "IVF_PQ")
    with pytest.raises(NotImplementedError):
        nl.write_dataset(table(3), str(tmp_path / "old"), data_storage_version="2.0")


def test_install_as_lance(tmp_path):
    """install_as_lance() makes `import lance` nanolance's for this process, `lance.dataset` stays the
    function (not the submodule of the same name), and uninstall gives the real one back."""
    saved = {k: v for k, v in sys.modules.items() if k == "lance" or k.startswith("lance.")}
    try:
        nl.install_as_lance()
        import lance as alias
        import lance.dataset  # noqa: F401 -- must not rebind alias.dataset to the module
        from lance.file import LanceFileReader

        assert alias is nl
        assert callable(alias.dataset) and not isinstance(alias.dataset, type(sys))
        assert LanceFileReader is nl.file.LanceFileReader
        alias.write_dataset(table(2), str(tmp_path / "ds"))
        assert alias.dataset(str(tmp_path / "ds")).count_rows() == 2
    finally:
        nl.uninstall_as_lance()
        sys.modules.update(saved)


# ── files ──────────────────────────────────────────────────────────────────────────────────────────


@pytest.mark.parametrize("writer", ["nanolance", "pylance"])
def test_files_agree(lance, tmp_path, writer):
    from lance.file import LanceFileReader as TheirReader, LanceFileWriter as TheirWriter

    Writer = nl.file.LanceFileWriter if writer == "nanolance" else TheirWriter
    path = str(tmp_path / "f.lance")
    data = table(40)
    with Writer(path) as w:
        for batch in data.to_batches(max_chunksize=16):
            w.write_batch(batch)
    for Reader in (nl.file.LanceFileReader, TheirReader):
        r = Reader(path)
        assert r.num_rows() == 40
        assert r.read_all().to_table() == data
        assert r.read_range(5, 10).to_table() == data.slice(5, 10)
        assert r.take_rows([2, 7, 39]).to_table() == data.take([2, 7, 39])
        with pytest.raises(ValueError, match="ascending"):
            r.take_rows([39, 2])
        assert r.metadata().num_rows == 40
        assert r.metadata().schema == data.schema
        assert Reader(path, columns=["name"]).read_all().to_table() == data.select(["name"])


def test_schema_only_file(tmp_path):
    path = str(tmp_path / "f.lance")
    schema = pa.schema([("a", pa.int64())])
    with nl.file.LanceFileWriter(path, schema):
        pass
    assert nl.file.LanceFileReader(path).metadata().schema == schema
    with pytest.raises(ValueError, match="Schema is unknown"):
        with nl.file.LanceFileWriter(str(tmp_path / "g.lance")):
            pass


# ── core bugs the pylance suite found ──────────────────────────────────────────────────────────────


def test_schema_metadata_is_written_and_read(lance, tmp_path):
    """Any schema-level metadata -- pandas adds some to every table -- made the writer fail with
    "struct array for '' is missing child": a missing metadata key was read as present."""
    t = table(3).replace_schema_metadata({"source": "unit test", "pandas": '{"x": 1}'})
    uri = str(tmp_path / "ds")
    nanolance.write_table(t, uri)
    assert pa.table(nanolance.read_table(uri)).schema.metadata == t.schema.metadata
    assert lance.dataset(uri).schema.metadata == t.schema.metadata
    assert nl.dataset(uri).schema.metadata == t.schema.metadata


def test_extension_columns_keep_their_rows(lance, tmp_path):
    """A column of an Arrow extension type (other than Lance's own blob) was mapped as having no data:
    the file held no column for it and a read returned no rows."""
    tensor = pa.ExtensionArray.from_storage(
        pa.fixed_shape_tensor(pa.float32(), [2, 3]),
        pa.FixedSizeListArray.from_arrays(pa.array(np.arange(18, dtype=np.float32)), 6),
    )
    uuids = pa.array([uuid.uuid4().bytes for _ in range(3)], pa.uuid())
    t = pa.table({"tensor": tensor, "uuid": uuids, "id": [1, 2, 3]})
    uri = str(tmp_path / "ds")
    nanolance.write_table(t, uri)
    assert pa.table(nanolance.read_table(uri)) == t
    assert lance.dataset(uri).to_table() == t
    assert nl.dataset(uri).take([2, 0]) == t.take([2, 0])


def test_encoding_hints_stay_out_of_the_schema(tmp_path):
    """nanolance's per-file encoding notes (constant values, packing) are not part of the dataset's
    schema: a constant column's field used to come back with them as field metadata."""
    uri = str(tmp_path / "ds")
    nl.write_dataset(pa.table({"z": [1.5] * 10}), uri)
    assert nl.dataset(uri).schema.field("z").metadata in (None, {})
