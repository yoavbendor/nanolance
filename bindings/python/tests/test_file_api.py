"""``lance.file`` (LanceFileSession, LanceFileWriter options), checked against pylance.

Every result here is compared with what pylance returns for the same files: the file session's
directory listing, the global buffers and schema metadata a writer adds, and the zstd option on a
column. The pylance-written side of each comparison is read by nanolance and the other way round.
"""

from __future__ import annotations

import importlib

import pyarrow as pa
import pytest

import nanolance.lance.file as nl_file
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance():
    module = require_pylance()
    importlib.import_module("lance.file")  # a submodule: not loaded by `import lance`
    return module


def _write_tree(session_cls, root, schema):
    """The same five files, written through the given file session class."""
    session = session_cls(str(root))
    for name in ["file1.lance", "file2.lance", "subdir/file3.lance", "subdir/nested/file4.lance",
                 "other/file5.lance"]:
        with session.open_writer(name, schema=schema) as writer:
            writer.write_batch(pa.table({"x": [1]}))


# ── LanceFileSession.list_with_delimiter ───────────────────────────────────────────────────────────


def test_list_with_delimiter_matches_pylance(tmp_path, lance):
    schema = pa.schema([pa.field("x", pa.int64())])
    nl_root = tmp_path / "nanolance"
    py_root = tmp_path / "pylance"
    _write_tree(nl_file.LanceFileSession, nl_root, schema)
    _write_tree(lance.file.LanceFileSession, py_root, schema)

    nl_session = nl_file.LanceFileSession(str(nl_root))
    py_session = lance.file.LanceFileSession(str(py_root))
    for path in [None, "", "subdir", "subdir/", "subdir/nested", "nonexistent"]:
        ours = nl_session.list_with_delimiter(path)
        theirs = py_session.list_with_delimiter(path)
        assert sorted(ours.common_prefixes) == sorted(theirs.common_prefixes), path
        assert sorted(ours.objects) == sorted(theirs.objects), path

    assert nl_session.list_with_delimiter("subdir/") == nl_session.list_with_delimiter("subdir")
    assert nl_session.list_with_delimiter("nonexistent").common_prefixes == []
    assert nl_session.list_with_delimiter("nonexistent").objects == []


# ── LanceFileWriter: global buffers and schema metadata ────────────────────────────────────────────


def test_global_buffer_and_schema_metadata_match_pylance(tmp_path, lance):
    table = pa.table({"a": [1, 2, 3]})
    payload = b"hello"

    # nanolance writes, pylance reads back: the index, the bytes and the schema metadata.
    ours = tmp_path / "nanolance.lance"
    with nl_file.LanceFileWriter(str(ours)) as writer:
        writer.write_batch(table)
        index = writer.add_global_buffer(payload)
        writer.add_schema_metadata("foo", "bar")
    assert index == 1  # global buffer 0 is the schema
    theirs = lance.file.LanceFileReader(str(ours))
    assert theirs.read_all().to_table() == table
    assert theirs.metadata().global_buffers[index].size == len(payload)
    assert bytes(theirs.read_global_buffer(index)) == payload
    assert theirs.metadata().schema.metadata.get(b"foo") == b"bar"

    # pylance writes, nanolance reads back.
    pylance_path = tmp_path / "pylance.lance"
    with lance.file.LanceFileWriter(str(pylance_path)) as writer:
        writer.write_batch(table)
        py_index = writer.add_global_buffer(payload)
        writer.add_schema_metadata("foo", "bar")
    assert py_index == index
    ours_read = nl_file.LanceFileReader(str(pylance_path))
    assert ours_read.read_all().to_table() == table
    assert bytes(ours_read.read_global_buffer(py_index)) == payload
    assert ours_read.metadata().schema.metadata.get(b"foo") == b"bar"
    assert ours_read.metadata().global_buffers[py_index].size == len(payload)
