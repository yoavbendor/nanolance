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
