"""Feature flags: nanolance refuses what Lance 12 refuses, with pylance's exception type and message.

A manifest's reader word must hold only bits this build supports (0-8) for a read to work, and its writer
word for a write. Bit 8 (mixed data-file versions) must be set in both words or neither. Each case builds
the dataset with each engine, patches its latest manifest with the flags (``patch_flags``), and compares the
exception type and message. pylance's message has a source suffix (``, /home/runner/...rs:NN:MM``) that is
dropped before the comparison.
"""

from __future__ import annotations

import os
import re
import struct
from datetime import timedelta

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

READ_REFUSED = ("Not supported: This dataset cannot be read by this version of Lance. Please upgrade Lance to "
                "read this dataset. Flags: {}")
WRITE_REFUSED = ("Not supported: This dataset cannot be written by this version of Lance. Please upgrade Lance "
                 "to write this dataset. Flags: {}")
APPEND_REFUSED = ("Not supported: This dataset cannot be written by this version of Lance. Please upgrade Lance "
                  "to write to this dataset.\n Flags: {}")
CORRUPT = ("Encountered corrupt file manifest: Manifest has only one of the mixed data-file-version reader and "
           "writer feature bits set, so its semantics are undefined")


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def table(n=3, start=1):
    return pa.table({"id": pa.array(range(start, start + n), pa.int64()),
                     "v": pa.array([i * 10 for i in range(start, start + n)], pa.int64())})


def strip_source(message: str) -> str:
    """Drop pylance's ``, /home/runner/...rs:NN:MM`` suffix."""
    return re.sub(r", /[^\s]*\.rs:\d+:\d+$", "", message)


def refusal(call):
    """The (type, message) of the exception ``call`` raises."""
    with pytest.raises(Exception) as info:
        call()
    return type(info.value), strip_source(str(info.value))


def latest_manifest(dataset: str) -> str:
    """The manifest of the newest version. Lance names a version by its inverted number (u64 max - version),
    so the newest has the smallest name; a plain number is a version as written."""
    versions = os.path.join(dataset, "_versions")

    def version(name: str) -> int:
        number = int(name[:-len(".manifest")])
        return (2**64 - 1 - number) if number > 2**63 else number

    names = [n for n in os.listdir(versions) if n.endswith(".manifest")]
    return os.path.join(versions, max(names, key=version))


def _varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def patch_flags(dataset: str, reader: int | None = None, writer: int | None = None) -> None:
    """Set a dataset's reader and writer flags in its latest manifest.

    The file ends with a 16-byte footer ``<Q pos><H major><H minor>LANC``; at ``pos`` is a u32 length and the
    protobuf body. The reader word is field 9 (tag 0x48), the writer word field 10 (tag 0x50). Appending a
    field again wins (a proto3 scalar takes its last value), so the body is rewritten with the new fields
    after the old ones.
    """
    path = latest_manifest(dataset)
    with open(path, "rb") as f:
        data = f.read()
    pos = struct.unpack("<Q", data[-16:-8])[0]
    (length,) = struct.unpack("<I", data[pos:pos + 4])
    body = data[pos + 4:pos + 4 + length]
    if reader is not None:
        body += b"\x48" + _varint(reader)
    if writer is not None:
        body += b"\x50" + _varint(writer)
    rewritten = data[:pos] + struct.pack("<I", len(body)) + body + data[pos + 4 + length:]
    with open(path, "wb") as f:
        f.write(rewritten)


def flagged(module, path: str, reader: int = 0, writer: int = 0, fragments: int = 1) -> str:
    """Write ``fragments`` fragments with ``module``, then patch the flags in."""
    module.write_dataset(table(), path)
    for _ in range(fragments - 1):
        module.write_dataset(table(3, 100), path, mode="append")
    patch_flags(path, reader=reader, writer=writer)
    return path


def both_engines(lance, tmp_path, reader: int = 0, writer: int = 0, fragments: int = 1):
    """(engine, path) for nanolance and pylance, each built and patched the same way."""
    out = []
    for name, module in (("nano", nl), ("py", lance)):
        out.append((module, flagged(module, str(tmp_path / f"{name}.lance"), reader, writer, fragments)))
    return out


# ── reader word ─────────────────────────────────────────────────────────────────────────────────────────

@pytest.mark.parametrize("bit", [9, 10, 12, 16])
def test_unknown_reader_bit_refuses_open_and_read(lance, tmp_path, bit):
    value = 1 << bit
    expected = READ_REFUSED.format(value)
    (nano, npath), (py, ppath) = both_engines(lance, tmp_path, reader=value, writer=value)
    assert refusal(lambda: nano.dataset(npath)) == refusal(lambda: py.dataset(ppath)) == (ValueError, expected)
    assert refusal(lambda: nano.dataset(npath).to_table()) == (ValueError, expected)
    assert refusal(lambda: py.dataset(ppath).to_table()) == (ValueError, expected)


# ── bit 8, paired ───────────────────────────────────────────────────────────────────────────────────────

@pytest.mark.parametrize(("reader", "writer"), [(256, 0), (0, 256)], ids=["reader-only", "writer-only"])
def test_mixed_bit_in_one_word_is_corrupt(lance, tmp_path, reader, writer):
    (nano, npath), (py, ppath) = both_engines(lance, tmp_path, reader=reader, writer=writer)
    assert refusal(lambda: nano.dataset(npath)) == refusal(lambda: py.dataset(ppath)) == (ValueError, CORRUPT)
    assert refusal(lambda: nano.dataset(npath).to_table()) == (ValueError, CORRUPT)
    assert refusal(lambda: py.dataset(ppath).to_table()) == (ValueError, CORRUPT)


# ── writer word ─────────────────────────────────────────────────────────────────────────────────────────

def _append(m, path):
    m.write_dataset(table(), path, mode="append")


def _delete(m, path):
    m.dataset(path).delete("id = 1")


def _update(m, path):
    m.dataset(path).update({"v": "v + 1"}, where="id = 2")


def _merge_insert(m, path):
    m.dataset(path).merge_insert("id").when_matched_update_all().execute(table(2, 1))


def _compact(m, path):
    m.dataset(path).optimize.compact_files()


def _scalar_index(m, path):
    m.dataset(path).create_scalar_index("id", "BTREE")


def _add_columns(m, path):
    m.dataset(path).add_columns({"w": "id * 2"})


def _drop_columns(m, path):
    m.dataset(path).drop_columns(["v"])


WRITE_OPS = {
    "append": (_append, APPEND_REFUSED),
    "delete": (_delete, WRITE_REFUSED),
    "update": (_update, WRITE_REFUSED),
    "merge_insert": (_merge_insert, WRITE_REFUSED),
    "compact": (_compact, WRITE_REFUSED),
    "scalar_index": (_scalar_index, WRITE_REFUSED),
    "add_columns": (_add_columns, WRITE_REFUSED),
    "drop_columns": (_drop_columns, WRITE_REFUSED),
}


@pytest.mark.parametrize("op", list(WRITE_OPS))
def test_unknown_writer_bit_reads_but_refuses_writes(lance, tmp_path, op):
    """Reads work and match; the write is refused with the same message, and the version does not move."""
    run, template = WRITE_OPS[op]
    expected = template.format(2048)
    (nano, npath), (py, ppath) = both_engines(lance, tmp_path, writer=2048, fragments=2)
    assert nano.dataset(npath).count_rows() == py.dataset(ppath).count_rows() == 6
    assert sorted(nano.dataset(npath).to_table().to_pylist(), key=lambda r: r["id"]) == \
        sorted(py.dataset(ppath).to_table().to_pylist(), key=lambda r: r["id"])
    nano_version, py_version = nano.dataset(npath).version, py.dataset(ppath).version
    nano_refusal = refusal(lambda: run(nano, npath))
    py_refusal = refusal(lambda: run(py, ppath))
    assert nano_refusal[1] == py_refusal[1] == expected
    assert nano.dataset(npath).version == nano_version
    assert py.dataset(ppath).version == py_version


@pytest.mark.parametrize("op", list(WRITE_OPS))
def test_unknown_writer_bit_exception_type(lance, tmp_path, op):
    run, _ = WRITE_OPS[op]
    (nano, npath), (py, ppath) = both_engines(lance, tmp_path, writer=2048, fragments=2)
    expected = NotImplementedError if op == "scalar_index" else OSError  # measured on pylance 12.0.0
    assert refusal(lambda: run(nano, npath))[0] is refusal(lambda: run(py, ppath))[0] is expected


def test_unknown_writer_bit_refuses_vector_index_build(lance, tmp_path):
    """A vector index build is refused as pylance refuses it: NotImplementedError, the commit wording."""
    vectors = pa.FixedSizeListArray.from_arrays(pa.array([float(i % 7) for i in range(64 * 8)], pa.float32()), 8)
    refusals = []
    for name, module in (("nano", nl), ("py", lance)):
        path = str(tmp_path / f"{name}.lance")
        module.write_dataset(pa.table({"vec": vectors}), path)
        patch_flags(path, writer=2048)
        refusals.append(refusal(lambda: module.dataset(path).create_index("vec", "IVF_FLAT", num_partitions=1)))
    assert refusals[0] == refusals[1] == (NotImplementedError, WRITE_REFUSED.format(2048))


# ── known bits that pylance writes still open, read and write ──────────────────────────────────────────

def _pylance_plain(lance, path):
    lance.write_dataset(table(), path)
    return path


def _pylance_stable(lance, path):
    lance.write_dataset(table(), path, enable_stable_row_ids=True)
    return path


def _pylance_deletions(lance, path):
    lance.write_dataset(table(), path)
    lance.dataset(path).delete("id = 1")
    return path


def _pylance_table_config(lance, path):
    lance.write_dataset(table(), path)
    lance.dataset(path).update_config({"owner": "t1"})
    return path


def _pylance_base_paths(lance, path):
    base = path + "_base"
    os.makedirs(base)
    lance.write_dataset(table(6), path, initial_bases=[lance.DatasetBasePath(base, name="path1")],
                        target_all_bases=True, max_rows_per_file=2)
    return path


def _pylance_overlays(lance, path, monkeypatch):
    # pylance 12 opens a dataset with overlays (feature flag 64) only with this set.
    monkeypatch.setenv("LANCE_ENABLE_UNSTABLE_DATA_OVERLAY_FILES", "1")
    lance.write_dataset(table(), path)
    ds = lance.dataset(path)
    with lance.file.LanceFileWriter(os.path.join(path, "data", "ov.lance")) as w:
        w.write_batch(pa.table({"v": pa.array([111, 444], pa.int64())}))
    base = ds.get_fragments()[0].metadata.files[0]
    df = lance.fragment.DataFile(path="ov.lance", fields=[1], column_indices=[0],
                                 file_major_version=base.file_major_version,
                                 file_minor_version=base.file_minor_version,
                                 file_size_bytes=os.path.getsize(os.path.join(path, "data", "ov.lance")))
    op = lance.LanceOperation.DataOverlay([lance.LanceOperation.DataOverlayGroup(
        ds.get_fragments()[0].fragment_id, [lance.LanceOperation.DataOverlayFile(df, offsets=[1, 2])])])
    lance.LanceDataset.commit(path, op, read_version=ds.version)
    return path


@pytest.mark.parametrize("known", ["plain", "stable_row_ids", "deletions", "table_config", "base_paths", "overlays"])
def test_known_bits_pylance_writes_still_read_and_write(lance, tmp_path, monkeypatch, known):
    path = str(tmp_path / "ds.lance")
    if known == "overlays":
        _pylance_overlays(lance, path, monkeypatch)
    else:
        build = {"plain": _pylance_plain, "stable_row_ids": _pylance_stable, "deletions": _pylance_deletions,
                 "table_config": _pylance_table_config, "base_paths": _pylance_base_paths}[known]
        build(lance, path)
    expected = lance.dataset(path).to_table().sort_by("id")
    assert nl.dataset(path).to_table().sort_by("id").to_pylist() == expected.to_pylist()
    nl.write_dataset(table(2, 50), path, mode="append")
    assert lance.dataset(path).count_rows() == expected.num_rows + 2
    assert nl.dataset(path).count_rows() == expected.num_rows + 2


# ── drop and cleanup ───────────────────────────────────────────────────────────────────────────────────

def test_drop_accepts_a_refused_manifest_as_pylance_does(lance, tmp_path):
    """Drop finds the dataset root from a manifest that parses, not from one it would read: pylance drops it."""
    for name, module in (("nano", nl), ("py", lance)):
        path = flagged(module, str(tmp_path / f"{name}.lance"), reader=512, writer=512)
        module.LanceDataset.drop(path)
        assert not os.path.exists(path), name


def test_cleanup_is_refused_as_open_is(lance, tmp_path):
    """Cleanup needs the dataset open, and an open refuses the reader word: pylance refuses it too."""
    (nano, npath), (py, ppath) = both_engines(lance, tmp_path, reader=512, writer=512)
    expected = (ValueError, READ_REFUSED.format(512))
    assert refusal(lambda: nano.dataset(npath).cleanup_old_versions(older_than=timedelta(0))) == expected
    assert refusal(lambda: py.dataset(ppath).cleanup_old_versions(older_than=timedelta(0))) == expected
