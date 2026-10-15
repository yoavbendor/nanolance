"""Blob writers: nanolance.lance's PackedBlobWriter, DedicatedBlobWriter and BlobDescriptorArrayBuilder
against pylance on the same calls.

Every case runs the same script on a nanolance session and on a pylance session (each in its own
directory) and compares what each returns: the descriptor reprs, the struct arrays, the descriptor
fields, the sidecar files byte for byte, the sidecar path relative to the base, and the exception
type and message of every rejected call.
"""

from __future__ import annotations

import os

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _sidecars(base) -> dict:
    """Every file under ``base``, keyed by its path relative to ``base``."""
    out = {}
    for root, _, files in os.walk(base):
        for name in files:
            full = os.path.join(root, name)
            with open(full, "rb") as f:
                out[os.path.relpath(full, base)] = f.read()
    return out


def _rel_path(path: str, base) -> str:
    """``writer.path`` is the absolute sidecar path without its leading slash; strip the base from it."""
    prefix = str(base).lstrip("/") + "/"
    assert path.startswith(prefix), path
    return path[len(prefix):]


def _published(sidecars: dict) -> dict:
    return {name: data for name, data in sidecars.items() if name.endswith(".blob")}


def _packed_scenario(module, base, finisher: str) -> dict:
    session = module.file.LanceFileSession(str(base))
    writer = session.open_packed_blob_writer("data.lance", 7)
    rel = _rel_path(writer.path, base)
    with pytest.raises(ValueError, match="available after finish_array"):
        writer.field
    writer.write_blob(b"s")
    writer.write_blobs(pa.array([b"a", None, b""], type=pa.binary()))
    writer.write_blobs(pa.array([None, b"bc"], type=pa.large_binary()))
    writer.write_blobs(pa.array([b"prefix", b"0123456789abcdefXYZ", None, b"v", b""],
                                type=pa.binary_view()).slice(1, 4))
    writer.write_blobs(pa.array([b"AB", None, b"CD"], type=pa.binary(2)))
    writer.write_blobs(pa.chunked_array([pa.array([b"c1", None]), pa.array([b"", b"c3"])]))
    writer.write_blob(bytearray(b"ba"))
    writer.write_blob(memoryview(b"mv"))
    writer.write_blob([1, 2, 255])
    out = {"rel_path": rel, "blob_id": writer.blob_id}
    if finisher == "finish":
        out["descriptors"] = [repr(d) for d in writer.finish()]
    else:
        out["rows"] = writer.finish_array("image_bytes").to_pylist()
        out["field"] = writer.field
    out["sidecars"] = _sidecars(base)
    return out


@pytest.mark.parametrize("finisher", ["finish", "finish_array"])
def test_packed_writer_mixed_calls_match_pylance(tmp_path, lance, finisher):
    nano = _packed_scenario(nl, tmp_path / "nano", finisher)
    real = _packed_scenario(lance, tmp_path / "lance", finisher)
    assert nano == real


def test_packed_writer_without_blobs_publishes_an_empty_sidecar(tmp_path, lance):
    def scenario(module, base):
        session = module.file.LanceFileSession(str(base))
        writer = session.open_packed_blob_writer("data.lance", 3)
        before = _published(_sidecars(base))
        descriptors = writer.finish()
        return before, descriptors, _sidecars(base)

    nano_before, nano_descriptors, nano_after = scenario(nl, tmp_path / "nano")
    real_before, real_descriptors, real_after = scenario(lance, tmp_path / "lance")
    # the sidecar name appears only at finish, as in pylance
    assert nano_before == real_before == {}
    assert nano_descriptors == real_descriptors == []
    assert nano_after == real_after
    assert list(nano_after.values()) == [b""]


def _dedicated_scenario(module, base) -> dict:
    session = module.file.LanceFileSession(str(base))
    writer = session.open_dedicated_blob_writer("data.lance", 5)
    writer.write(b"abc")
    writer.write(bytearray(b"def"))
    rel = _rel_path(writer.path, base)
    blob_id = writer.blob_id
    descriptor = writer.finish()
    return {"rel_path": rel, "blob_id": blob_id,
            "descriptor": repr(descriptor), "sidecars": _sidecars(base)}


def test_dedicated_writer_matches_pylance(tmp_path, lance):
    nano = _dedicated_scenario(nl, tmp_path / "nano")
    real = _dedicated_scenario(lance, tmp_path / "lance")
    assert nano == real
    assert nano["descriptor"] == "Dedicated { blob_id: 5, size: 6 }"


def _builder_scenario(module, base) -> dict:
    session = module.file.LanceFileSession(str(base))
    packed = session.open_packed_blob_writer("x.lance", 2)
    packed.write_blob(b"zz")
    from_writer = packed.finish()[0]

    builder = module.BlobDescriptorArrayBuilder("blob")
    builder.append_inline(b"inl")
    builder.append_null()
    builder.append_dedicated(9, 100)
    builder.extend_packed(3, [0, 10], [10, 5])
    builder.append(from_writer)
    builder.extend([from_writer])
    array = builder.finish()

    empty = module.BlobDescriptorArrayBuilder("e").finish()
    return {"rows": array.to_pylist(), "type": str(array.type), "field": builder.field,
            "empty_len": len(empty), "empty_type": str(empty.type)}


def test_descriptor_array_builder_matches_pylance(tmp_path, lance):
    nano = _builder_scenario(nl, tmp_path / "nano")
    real = _builder_scenario(lance, tmp_path / "lance")
    assert nano == real
    assert nano["rows"][0] == {"kind": 0, "data": b"inl", "uri": None, "blob_id": None,
                               "blob_size": None, "position": None}
    assert nano["rows"][1] is None


def test_descriptor_field_is_the_blob_v2_struct(lance):
    for module in (nl, lance):
        field = module.BlobDescriptorArrayBuilder("blob").field
        assert field.name == "blob"
        assert field.metadata == {b"ARROW:extension:name": b"lance.blob.v2"}
        assert [f.name for f in field.type] == ["kind", "data", "uri", "blob_id", "blob_size", "position"]


def _rejected_calls():
    """(name, action(module, session), exception type, message fragment) for the rejected calls of the spec."""
    def packed(session):
        return session.open_packed_blob_writer("d.lance", 1)

    def dedicated(session):
        return session.open_dedicated_blob_writer("d.lance", 1)

    def int32_array(m, s):
        packed(s).write_blobs(pa.array([1, 2], type=pa.int32()))

    def int32_chunked(m, s):
        packed(s).write_blobs(pa.chunked_array([[1], [2]], type=pa.int32()))

    def python_list(m, s):
        packed(s).write_blobs([b"not an Arrow array"])

    def bytes_payload(m, s):
        packed(s).write_blobs(b"raw")

    def non_monotonic(m, s):
        offsets = pa.array([0, 2, 1, 2], type=pa.int32()).buffers()[1]
        packed(s).write_blobs(pa.Array.from_buffers(pa.binary(), 3, [None, offsets, pa.py_buffer(b"ab")]))

    def packed_after_finish(m, s):
        writer = packed(s)
        writer.finish()
        writer.write_blob(b"x")

    def packed_finish_twice(m, s):
        writer = packed(s)
        writer.finish()
        writer.finish_array("blob")

    def packed_field_before_finish(m, s):
        packed(s).field

    def dedicated_after_finish(m, s):
        writer = dedicated(s)
        writer.finish()
        writer.write(b"x")

    def dedicated_finish_twice(m, s):
        writer = dedicated(s)
        writer.finish()
        writer.finish()

    def builder_append_bytes(m, s):
        m.BlobDescriptorArrayBuilder("blob").append(b"raw")

    def builder_append_none(m, s):
        m.BlobDescriptorArrayBuilder("blob").append(None)

    def builder_extend_bytes(m, s):
        m.BlobDescriptorArrayBuilder("blob").extend([b"r"])

    def builder_length_mismatch(m, s):
        m.BlobDescriptorArrayBuilder("blob").extend_packed(1, [0], [1, 2])

    def builder_after_finish(m, s):
        builder = m.BlobDescriptorArrayBuilder("blob")
        builder.finish()
        builder.append_null()

    return [
        ("packed str", lambda m, s: packed(s).write_blob("str"), TypeError, "Can't extract `str` to `Vec`"),
        ("packed int", lambda m, s: packed(s).write_blob(3), TypeError, "Sequence"),
        ("packed None", lambda m, s: packed(s).write_blob(None), TypeError, "Sequence"),
        ("packed list 300", lambda m, s: packed(s).write_blob([300]), OverflowError,
         "out of range integral type conversion attempted"),
        ("packed int32 array", int32_array, ValueError, "must have a Binary Arrow type, got Int32"),
        ("packed int32 chunked", int32_chunked, ValueError, "payloads must have a Binary Arrow type, got Int32"),
        ("packed python list", python_list, ValueError, "payloads must be a pyarrow BinaryArray"),
        ("packed bytes", bytes_payload, ValueError, "payloads must be a pyarrow BinaryArray"),
        ("packed non-monotonic", non_monotonic, ValueError, "non-monotonic offset at slot 1: 2 > 1"),
        ("packed after finish", packed_after_finish, ValueError, "PackedBlobWriter is already finished"),
        ("packed finish twice", packed_finish_twice, ValueError, "PackedBlobWriter is already finished"),
        ("packed field before finish", packed_field_before_finish, ValueError,
         "PackedBlobWriter field is available after finish_array"),
        ("dedicated str", lambda m, s: dedicated(s).write("str"), TypeError, "Can't extract `str` to `Vec`"),
        ("dedicated list 300", lambda m, s: dedicated(s).write([300]), OverflowError,
         "out of range integral type conversion attempted"),
        ("dedicated after finish", dedicated_after_finish, ValueError,
         "DedicatedBlobWriter is already finished"),
        ("dedicated finish twice", dedicated_finish_twice, ValueError,
         "DedicatedBlobWriter is already finished"),
        ("builder bytes", builder_append_bytes, TypeError,
         "'bytes' object is not an instance of 'BlobDescriptor'"),
        ("builder None", builder_append_none, TypeError, "'None' is not an instance of 'BlobDescriptor'"),
        ("builder extend bytes", builder_extend_bytes, TypeError,
         "'bytes' object is not an instance of 'BlobDescriptor'"),
        ("builder length mismatch", builder_length_mismatch, ValueError,
         "offsets and sizes must have the same length, got 1 and 2"),
        ("builder after finish", builder_after_finish, ValueError,
         "BlobDescriptorArrayBuilder is already finished"),
    ]


@pytest.mark.parametrize("case", _rejected_calls(), ids=lambda c: c[0])
def test_rejected_calls_raise_the_same_error_as_pylance(tmp_path, lance, case):
    _, action, expected_type, fragment = case
    results = {}
    for name, module in (("nanolance", nl), ("pylance", lance)):
        session = module.file.LanceFileSession(str(tmp_path / name))
        try:
            action(module, session)
        except Exception as exc:  # noqa: BLE001 -- the type and message are what is being compared
            results[name] = (type(exc), str(exc))
        else:
            pytest.fail(f"{name} did not raise")
        assert results[name][0] is expected_type, f"{name}: {results[name]}"
        assert fragment in results[name][1], f"{name}: {results[name][1]}"

# Section 6 item 5: end to end. The sidecars come from one engine's writers, the data file and the
# DataReplacement commit always come from nanolance, and the blobs are read back by both engines.
_REPLACEMENT_FILE = "replacement.lance"

# Each case is a list of row operations, in row order.
_REPLACEMENT_CASES = {
    "packed": [("packed", [b"p0", b"p1", b"p2"])],
    "empty_packed": [("packed", []), ("inline", b"after-empty")],
    "inline": [("inline", b"i0"), ("inline", b"i1")],
    "dedicated": [("dedicated", [b"ded", b"icated"])],
    "mixed": [("packed", [b"packed-one", b""]), ("inline", b"inl"), ("dedicated", [b"dedic", b"ated"])],
    # kept apart from "mixed": take_blobs on a null row in a mixed replacement raises in pylance too
    "packed_null": [("packed", [b"packed-one"]), ("null", None)],
}


def _expected_blobs(ops) -> list:
    out = []
    for kind, value in ops:
        if kind == "packed":
            out.extend(value)
        elif kind == "inline":
            out.append(value)
        elif kind == "dedicated":
            out.append(b"".join(value))
        else:
            out.append(None)
    return out


def _replacement_column(module, files, ops):
    """The blob descriptor column for ``ops``, built with ``module``'s writers and builder."""
    builder = module.BlobDescriptorArrayBuilder("blob")
    for kind, value in ops:
        if kind == "packed":
            writer = files.open_packed_blob_writer(_REPLACEMENT_FILE, 1)
            for payload in value:
                writer.write_blob(payload)
            builder.extend(writer.finish())
        elif kind == "inline":
            builder.append_inline(value)
        elif kind == "dedicated":
            writer = files.open_dedicated_blob_writer(_REPLACEMENT_FILE, 2)
            for chunk in value:
                writer.write(chunk)
            builder.append(writer.finish())
        else:
            builder.append_null()
    return builder


def _blob_dataset(lance, uri, n_rows: int) -> None:
    """One fragment of ``n_rows`` rows. DataReplacement keeps the replaced fragment's row count, so the
    replacement must have the same number of rows (pylance does the same)."""
    logical = pa.schema([pa.field("id", pa.uint32(), nullable=False), lance.blob_field("blob")])
    table = pa.table([pa.array([0] * n_rows, type=pa.uint32()),
                      lance.blob_array([b"old-%d" % i for i in range(n_rows)])], schema=logical)
    lance.write_dataset(table, str(uri), data_storage_version="2.2")


def _commit_replacement(uri, schema, batch) -> None:
    """nanolance's data file writer, DataFile.create and DataReplacement commit, for fragment 0."""
    with nl.file.LanceFileSession(os.path.join(uri, "data")).open_writer(
            _REPLACEMENT_FILE, schema=schema, version="2.2") as writer:
        writer.write_batch(batch)
    ds = nl.dataset(str(uri))
    data_file = nl.fragment.DataFile.create(ds, _REPLACEMENT_FILE)
    op = nl.LanceOperation.DataReplacement([nl.LanceOperation.DataReplacementGroup(0, data_file)])
    nl.LanceDataset.commit(str(uri), op, read_version=ds.version)


def _read_back(module, uri, n_rows) -> tuple:
    ds = module.dataset(str(uri))
    ids = ds.to_table(columns=["id"]).column("id").to_pylist()
    blobs = []
    for i in range(n_rows):
        (blob,) = ds.take_blobs("blob", indices=[i])
        blobs.append(None if blob is None else blob.readall())
    return ids, blobs


@pytest.mark.parametrize("sidecar_writer", ["nanolance", "pylance"])
@pytest.mark.parametrize("case", sorted(_REPLACEMENT_CASES))
def test_replacement_commit_reads_back_the_same_bytes_in_both_engines(tmp_path, lance, case, sidecar_writer):
    ops = _REPLACEMENT_CASES[case]
    n_rows = len(_expected_blobs(ops))
    uri = tmp_path / "ds"
    _blob_dataset(lance, uri, n_rows)

    writers = nl if sidecar_writer == "nanolance" else lance
    files = writers.file.LanceFileSession(os.path.join(uri, "data"))
    builder = _replacement_column(writers, files, ops)
    schema = pa.schema([pa.field("id", pa.uint32(), nullable=False), builder.field])
    batch = pa.record_batch([pa.array(range(1, n_rows + 1), type=pa.uint32()), builder.finish()],
                            schema=schema)
    _commit_replacement(uri, schema, batch)

    expected = (list(range(1, n_rows + 1)), _expected_blobs(ops))
    assert _read_back(nl, uri, n_rows) == expected
    assert _read_back(lance, uri, n_rows) == expected
