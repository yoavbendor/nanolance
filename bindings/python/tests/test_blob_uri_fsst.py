"""External blob v2 descriptors with long, repeating uris (millions of rows naming a few recordings).

pylance 12 stores a descriptor as a VariablePackedStruct whose uri field is FSST-compressed per value (the table in
the page descriptor). nanolance read those files wrong -- it returned the compressed bytes as the uri -- and wrote
its own uris plain, ~2.8x larger. Both now follow pylance: the layout text equals pylance's, either library reads
the other's files, and fetching a blob through the descriptor returns the source file's bytes."""

from __future__ import annotations

import os
import re
import subprocess
import uuid
from pathlib import Path

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance

PAGELAYOUT = Path(__file__).resolve().parents[3] / "build" / "nlance-pagelayout"
RECORDING = 1 << 16


def _layout(path) -> list[str]:
    if not PAGELAYOUT.exists():
        pytest.skip(f"{PAGELAYOUT} not built")
    out = subprocess.run([str(PAGELAYOUT), str(path)], capture_output=True, text=True, check=True).stdout
    return sorted(re.sub(r",rows=\d+", "", line.split("  ", 1)[1]) for line in out.splitlines() if "  " in line)


def _data_bytes(path) -> int:
    return sum(f.stat().st_size for f in Path(path, "data").glob("*.lance"))


@pytest.fixture(scope="module")
def recordings(tmp_path_factory):
    """Four source files with long paths, and their bytes."""
    root = tmp_path_factory.mktemp("recordings") / ("session-" + "x" * 40) / "mf4"
    root.mkdir(parents=True)
    files = {}
    for k in range(4):
        path = root / f"recording_{k:03d}_vehicle_front_camera.mf4"
        data = os.urandom(RECORDING)
        path.write_bytes(data)
        files["file://" + str(path)] = data
    return files


def _ref(mod, uris, i):
    return mod.Blob.from_uri(uris[(i // 500) % len(uris)], position=(i * 64) % (RECORDING - 64), size=64)


def _table(mod, uris, rows, null_every=0):
    values = [None if null_every and i % null_every == 0 else _ref(mod, uris, i) for i in range(rows)]
    return pa.table([pa.array(range(rows), pa.int64()), mod.blob_array(values)],
                    schema=pa.schema([pa.field("id", pa.int64()), mod.blob_field("b")]))


def _descriptors(mod, path, column="b"):
    return mod.dataset(str(path)).to_table(columns=[column]).column(column).to_pylist()


SHAPES = {"many_rows": (20_000, 0), "few_rows_plain": (100, 0), "with_nulls": (20_000, 7)}


@pytest.mark.parametrize("name", sorted(SHAPES))
def test_written_as_pylance_and_read_both_ways(tmp_path, recordings, name):
    lance = require_pylance()
    uris = list(recordings)
    rows, null_every = SHAPES[name]
    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl.write_dataset(_table(nl, uris, rows, null_every), str(ours), allow_external_blob_outside_bases=True)
    lance.write_dataset(_table(lance, uris, rows, null_every), str(theirs), allow_external_blob_outside_bases=True)
    layout = _layout(ours)
    assert layout == _layout(theirs)
    assert ("Fsst" in " ".join(layout)) == (rows >= 1000), layout  # below Lance's 32 KiB threshold: plain
    # Each library reads each file the same, and the uris are the real ones.
    for path in (ours, theirs):
        mine, pylance = _descriptors(nl, path), _descriptors(lance, path)
        assert mine == pylance
        assert {d["blob_uri"] for d in mine if d is not None} <= set(uris)
    # Within 10% of pylance's size (nanolance's uris are FSST-compressed as pylance's are).
    assert _data_bytes(ours) <= 1.1 * _data_bytes(theirs)


def test_fetching_through_the_descriptors_returns_the_source_bytes(tmp_path, recordings):
    """The golden check: a blob read through a descriptor nanolance wrote, or one pylance wrote and nanolance reads,
    is the source file's bytes at (uri, position, size)."""
    lance = require_pylance()
    uris = list(recordings)
    rows = 20_000
    picks = [0, 1, 499, 500, 7_777, rows - 1]
    for writer in (nl, lance):
        path = tmp_path / f"{writer.__name__}.lance"
        writer.write_dataset(_table(writer, uris, rows), str(path), allow_external_blob_outside_bases=True)
        for reader in (nl, lance):
            blobs = reader.dataset(str(path)).take_blobs("b", indices=picks)
            for i, blob in zip(picks, blobs):
                ref = _ref(nl, uris, i)
                assert blob.read() == recordings[ref.uri][ref.position:ref.position + ref.size], (writer, reader, i)


def test_uris_shrink(tmp_path, recordings):
    """250-byte-class uris repeated over many rows: the descriptors are a small fraction of the plain size."""
    uris = list(recordings)
    path = tmp_path / "ds.lance"
    rows = 50_000
    nl.write_dataset(_table(nl, uris, rows), str(path), allow_external_blob_outside_bases=True)
    plain = rows * (29 + len(uris[0]) + 4)  # descriptor row, its uri, its offset
    assert _data_bytes(path) < plain / 2.5, (_data_bytes(path), plain)


def test_nested_blobs_with_long_uris(tmp_path, recordings):
    """Blobs inside a struct and a list take the same uri compression; both libraries read them."""
    lance = require_pylance()
    uris = list(recordings)
    rows = 8_000

    def storage_struct():
        return pa.struct([pa.field("data", pa.large_binary()), pa.field("uri", pa.utf8()),
                          pa.field("position", pa.uint64()), pa.field("size", pa.uint64())])

    def make(mod):
        blob_type = mod.blob_field("x").type
        refs = [_ref(nl, uris, i) for i in range(rows)]
        flat = pa.array([{"data": None, "uri": r.uri, "position": r.position, "size": r.size} for r in refs],
                        storage_struct())
        blobs = pa.ExtensionArray.from_storage(blob_type, flat)
        struct = pa.StructArray.from_arrays([blobs, pa.array(range(rows), pa.int64())],
                                            fields=[mod.blob_field("x"), pa.field("y", pa.int64())])
        offsets = pa.array(list(range(0, rows + 1, 2)), pa.int32())
        listed = pa.ListArray.from_arrays(offsets, blobs, type=pa.list_(mod.blob_field("item")))
        return pa.table({"s": struct.slice(0, rows // 2), "l": listed})

    ours, theirs = tmp_path / "ours.lance", tmp_path / "theirs.lance"
    nl.write_dataset(make(nl), str(ours), allow_external_blob_outside_bases=True)
    lance.write_dataset(make(lance), str(theirs), allow_external_blob_outside_bases=True)
    assert _layout(ours) == _layout(theirs)
    for path in (ours, theirs):
        for column in ("s", "l"):
            assert _descriptors(nl, path, column) == _descriptors(lance, path, column), (path, column)


ENDPOINT = os.environ.get("NANOLANCE_S3_TEST_ENDPOINT", "")


@pytest.mark.skipif(not ENDPOINT, reason="NANOLANCE_S3_TEST_ENDPOINT is not set (no S3 endpoint)")
def test_on_s3(recordings):
    """The same on an s3:// dataset (staged write, as a cloud worker does), read by both libraries."""
    lance = require_pylance()
    options = {"aws_access_key_id": "testing", "aws_secret_access_key": "testing", "aws_region": "us-east-1",
               "aws_endpoint": ENDPOINT, "allow_http": "true"}
    uri = f"s3://{os.environ.get('NANOLANCE_S3_TEST_BUCKET', 'nl-test')}/blob-uris-{uuid.uuid4().hex}/ds.lance"
    uris = list(recordings)
    nl.write_dataset(_table(nl, uris, 20_000), uri, storage_options=options, allow_external_blob_outside_bases=True)
    mine = nl.dataset(uri, storage_options=options).to_table(columns=["b"]).column("b").to_pylist()
    theirs = lance.dataset(uri, storage_options=options).to_table(columns=["b"]).column("b").to_pylist()
    assert mine == theirs and {d["blob_uri"] for d in mine} <= set(uris)


def test_compaction_repacks_plain_descriptors(tmp_path, recordings):
    """The offline repack: fragments whose descriptors are plain (written before this, or too small for FSST) are
    rewritten FSST-compressed by compact_files, the descriptors unchanged and the blobs fetching the same bytes."""
    lance = require_pylance()
    uris = list(recordings)
    path = tmp_path / "ds.lance"
    for k in range(30):  # 100 rows a commit: under Lance's 32 KiB threshold, so each fragment is plain
        rows = [_ref(nl, uris, k * 100 + i) for i in range(100)]
        table = pa.table([pa.array(range(k * 100, k * 100 + 100), pa.int64()), nl.blob_array(rows)],
                         schema=pa.schema([pa.field("id", pa.int64()), nl.blob_field("b")]))
        nl.write_dataset(table, str(path), mode="append" if k else "create", allow_external_blob_outside_bases=True)
    assert "Fsst" not in " ".join(_layout(path))
    before = _descriptors(nl, path)

    def referenced_bytes():
        ds = nl.dataset(str(path))
        return sum((path / "data" / f.path).stat().st_size for fr in ds.get_fragments() for f in fr.data_files())

    plain_bytes = referenced_bytes()
    nl.dataset(str(path)).optimize.compact_files(target_rows_per_fragment=1_000_000)
    assert len(nl.dataset(str(path)).get_fragments()) == 1
    assert "Fsst" in " ".join(_layout(path))
    assert referenced_bytes() < plain_bytes / 2
    assert _descriptors(nl, path) == before == _descriptors(lance, path)
    picks = [0, 99, 100, 2999]
    for i, blob in zip(picks, nl.dataset(str(path)).take_blobs("b", indices=picks)):
        ref = _ref(nl, uris, i)
        assert blob.read() == recordings[ref.uri][ref.position:ref.position + ref.size]
