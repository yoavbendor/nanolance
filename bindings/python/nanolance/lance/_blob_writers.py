# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Prepared-layout blob writers: ``PackedBlobWriter``, ``DedicatedBlobWriter`` and
``BlobDescriptorArrayBuilder``, with the :class:`BlobDescriptor` values they hand out.

A writer puts blob bytes into a sidecar file next to the data file (``<base>/<data name>/<blob id>.blob``)
and returns descriptors that point at them. The descriptors go into a blob column of a data file that
is written and committed separately, so the bytes can be produced by a distributed job without writing
the data file itself. Mirrors the same classes in ``lance`` (pylance 12).
"""

from __future__ import annotations

import os
import tempfile
from typing import Any, Iterable, List, Optional, Sequence, Tuple

import numpy as np
import pyarrow as pa

__all__ = ["BlobDescriptor", "BlobDescriptorArrayBuilder", "DedicatedBlobWriter", "PackedBlobWriter"]

_BLOB_ENCODING_NAME = b"ARROW:extension:name"
_BLOB_ENCODING_VALUE = b"lance.blob.v2"
_KIND_INLINE, _KIND_PACKED, _KIND_DEDICATED, _KIND_EXTERNAL = 0, 1, 2, 3
_DESCRIPTOR_FIELDS = [
    pa.field("kind", pa.uint8()),
    pa.field("data", pa.large_binary()),
    pa.field("uri", pa.string()),
    pa.field("blob_id", pa.uint32()),
    pa.field("blob_size", pa.uint64()),
    pa.field("position", pa.uint64()),
]
_DESCRIPTOR_TYPE = pa.struct(_DESCRIPTOR_FIELDS)
_OUT_OF_RANGE = "out of range integral type conversion attempted"


class BlobDescriptor:
    """Where one blob lives: inline bytes, a range of a packed or dedicated sidecar, an external URI,
    or null. Obtained from the writers' ``finish`` methods, or built with :class:`BlobDescriptorArrayBuilder`.
    """

    __slots__ = ("_kind", "_data", "_uri", "_blob_id", "_position", "_size")

    def __init__(self, kind: str, *, data: Optional[bytes] = None, uri: Optional[str] = None,
                 blob_id: Optional[int] = None, position: Optional[int] = None, size: Optional[int] = None):
        self._kind = kind
        self._data = data
        self._uri = uri
        self._blob_id = blob_id
        self._position = position
        self._size = size

    @staticmethod
    def _null() -> "BlobDescriptor":
        return BlobDescriptor("null")

    @staticmethod
    def _inline(data: bytes) -> "BlobDescriptor":
        return BlobDescriptor("inline", data=data, size=len(data))

    @staticmethod
    def _packed(blob_id: int, offset: int, size: int) -> "BlobDescriptor":
        return BlobDescriptor("packed", blob_id=blob_id, position=offset, size=size)

    @staticmethod
    def _dedicated(blob_id: int, size: int) -> "BlobDescriptor":
        return BlobDescriptor("dedicated", blob_id=blob_id, size=size)

    @staticmethod
    def _external(uri: str, position: int, size: int) -> "BlobDescriptor":
        return BlobDescriptor("external", uri=uri, position=position, size=size)

    def _row(self) -> Optional[Tuple]:
        """The ``(kind, data, uri, blob_id, blob_size, position)`` struct row, or None for a null."""
        if self._kind == "null":
            return None
        if self._kind == "inline":
            return (_KIND_INLINE, self._data, None, None, None, None)
        if self._kind == "packed":
            return (_KIND_PACKED, None, None, self._blob_id, self._size, self._position)
        if self._kind == "dedicated":
            return (_KIND_DEDICATED, None, None, self._blob_id, self._size, None)
        return (_KIND_EXTERNAL, None, self._uri, None, self._size, self._position)

    def _key(self) -> Tuple:
        return (self._kind, self._data, self._uri, self._blob_id, self._position, self._size)

    def __repr__(self) -> str:
        if self._kind == "null":
            return "Null"
        if self._kind == "inline":
            return f"Inline {{ size: {self._size} }}"
        if self._kind == "packed":
            return f"Packed {{ blob_id: {self._blob_id}, offset: {self._position}, size: {self._size} }}"
        if self._kind == "dedicated":
            return f"Dedicated {{ blob_id: {self._blob_id}, size: {self._size} }}"
        return f'External {{ uri: "{self._uri}", position: {self._position}, size: {self._size} }}'

    __str__ = __repr__

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, BlobDescriptor):
            return NotImplemented
        return self._key() == other._key()

    def __hash__(self) -> int:
        return hash(self._key())


_NULL = BlobDescriptor._null()


def _struct_array(descriptors: Sequence[BlobDescriptor]) -> pa.StructArray:
    rows = [d._row() for d in descriptors]
    columns: List[List[Any]] = [[] for _ in _DESCRIPTOR_FIELDS]
    for row in rows:
        for column, value in zip(columns, row or (None,) * len(_DESCRIPTOR_FIELDS)):
            column.append(value)
    arrays = [pa.array(column, type=field.type) for column, field in zip(columns, _DESCRIPTOR_FIELDS)]
    mask = pa.array([row is None for row in rows], type=pa.bool_())
    return pa.StructArray.from_arrays(arrays, fields=_DESCRIPTOR_FIELDS, mask=mask)


def _descriptor_field(name: str) -> pa.Field:
    return pa.field(name, _DESCRIPTOR_TYPE, nullable=True, metadata={_BLOB_ENCODING_NAME: _BLOB_ENCODING_VALUE})


def _sidecar_path(base: str, data_path: str, blob_id: int) -> str:
    if not 0 <= blob_id < 2**32:
        raise OverflowError(_OUT_OF_RANGE)
    stem = data_path[: -len(".lance")] if data_path.endswith(".lance") else data_path
    name = f"{int(f'{blob_id:032b}'[::-1], 2):032b}.blob"
    return os.path.join(base, stem, name)


def _open_sidecar(path: str):
    """A hidden temporary file in the sidecar's directory; it is renamed to ``path`` when the writer finishes,
    so a reader never sees a partly written sidecar."""
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".tmp")
    return os.fdopen(fd, "wb"), tmp


def _payload_bytes(data: Any) -> bytes:
    """A single blob argument: bytes, bytearray, memoryview, or a list of ints 0..255."""
    if isinstance(data, (bytes, bytearray)):
        return bytes(data)
    if isinstance(data, memoryview):
        return data.tobytes()
    if isinstance(data, str):
        raise TypeError("argument 'data': Can't extract `str` to `Vec`")
    if isinstance(data, list):
        for value in data:
            if not isinstance(value, int) or isinstance(value, bool):
                raise TypeError(f"argument 'data': '{type(value).__name__}' object cannot be interpreted as an integer")
            if not 0 <= value <= 255:
                raise OverflowError(_OUT_OF_RANGE)
        return bytes(data)
    name = "None" if data is None else type(data).__name__
    raise TypeError(f"argument 'data': '{name}' object is not an instance of 'Sequence'")


def _arrow_name(data_type: pa.DataType) -> str:
    """The Arrow type name as the Rust side spells it in error messages (``Int32``, ``Utf8``)."""
    if pa.types.is_integer(data_type):
        return ("Int" if pa.types.is_signed_integer(data_type) else "UInt") + str(data_type.bit_width)
    names = {"string": "Utf8", "large_string": "LargeUtf8", "bool": "Boolean", "float32": "Float32",
             "float64": "Float64", "binary": "Binary", "large_binary": "LargeBinary", "binary_view": "BinaryView"}
    return names.get(str(data_type), str(data_type))


def _is_binary(data_type: pa.DataType) -> bool:
    return (pa.types.is_binary(data_type) or pa.types.is_large_binary(data_type)
            or pa.types.is_binary_view(data_type) or pa.types.is_fixed_size_binary(data_type))


def _validity(array: pa.Array) -> np.ndarray:
    n = len(array)
    if array.null_count == 0:
        return np.ones(n, dtype=bool)
    bits = np.frombuffer(array.buffers()[0], dtype=np.uint8)
    return np.unpackbits(bits, bitorder="little")[array.offset: array.offset + n].astype(bool)


def _array_ranges(array: pa.Array, prefix: str):
    """``(valid, starts, ends, values)`` for the logical slots of a binary-like array.

    ``values`` is a buffer holding the bytes that ``starts``/``ends`` index into. Offsets are checked
    before anything is written, so a malformed array fails the whole call.
    """
    if pa.types.is_binary_view(array.type):
        array = array.cast(pa.large_binary())
    n = len(array)
    valid = _validity(array)
    buffers = array.buffers()
    if pa.types.is_fixed_size_binary(array.type):
        width = array.type.byte_width
        values = memoryview(buffers[1]) if buffers[1] is not None else memoryview(b"")
        starts = (np.arange(array.offset, array.offset + n, dtype=np.int64)) * width
        return valid, starts, starts + width, values
    offset_type = np.int64 if pa.types.is_large_binary(array.type) else np.int32
    values = memoryview(buffers[2]) if buffers[2] is not None else memoryview(b"")
    if n == 0:
        empty = np.zeros(0, dtype=np.int64)
        return valid, empty, empty, values
    itemsize = np.dtype(offset_type).itemsize
    offsets = np.frombuffer(buffers[1], dtype=offset_type, count=n + 1,
                            offset=array.offset * itemsize).astype(np.int64)
    descents = np.nonzero(offsets[1:] < offsets[:-1])[0]
    if len(descents):
        slot = int(descents[0])
        raise ValueError(f"{prefix}Invalid argument error: Offset invariant failure: non-monotonic offset at slot "
                         f"{slot}: {offsets[slot]} > {offsets[slot + 1]}")
    if offsets[0] < 0 or offsets[-1] > len(values):
        bad = int(offsets[-1] if offsets[-1] > len(values) else offsets[0])
        raise ValueError(f"{prefix}Invalid argument error: offset {bad} is out of bounds for values of length "
                         f"{len(values)}")
    return valid, offsets[:-1], offsets[1:], values


class _SidecarWriter:
    """Shared state of the two writers: the open sidecar file and whether it is finished."""

    def __init__(self, base: str, data_path: str, blob_id: int):
        self._blob_id = blob_id
        self._sidecar = os.path.abspath(_sidecar_path(base, data_path, blob_id))
        self._file, self._tmp = _open_sidecar(self._sidecar)
        self._finished = False

    @property
    def blob_id(self) -> int:
        self._check_open()
        return self._blob_id

    @property
    def path(self) -> str:
        self._check_open()
        return self._sidecar.lstrip("/")

    def _check_open(self) -> None:
        if self._finished:
            raise ValueError(f"{type(self).__name__} is already finished")

    def _close(self) -> None:
        self._finished = True
        self._file.close()
        os.replace(self._tmp, self._sidecar)


class PackedBlobWriter(_SidecarWriter):
    """Appends blobs back to back into one sidecar file and returns a descriptor for each row.

    Create it with :meth:`LanceFileSession.open_packed_blob_writer`. Use :meth:`write_blob` for one blob,
    :meth:`write_blobs` for an Arrow binary array (only its valid slots are written), then either
    :meth:`finish` (a list of descriptors) or :meth:`finish_array` (a struct array of the descriptor type,
    with :attr:`field` naming it).
    """

    def __init__(self, base: str, data_path: str, blob_id: int):
        super().__init__(base, data_path, blob_id)
        self._offset = 0
        self._segments: List[Tuple[np.ndarray, np.ndarray, np.ndarray]] = []
        self._field: Optional[pa.Field] = None

    def write_blob(self, data: Any) -> None:
        self._check_open()
        payload = _payload_bytes(data)
        self._file.write(payload)
        self._add(np.ones(1, dtype=bool), np.array([len(payload)], dtype=np.int64))

    def write_blobs(self, payloads: Any) -> None:
        self._check_open()
        parts = self._parts(payloads)  # every chunk is validated before any byte is written
        for valid, starts, ends, values in parts:
            # Adjacent valid slots are one run of the values buffer: one write per run, not per row.
            keep = valid & (ends > starts)
            s, e = starts[keep], ends[keep]
            if len(s):
                breaks = np.nonzero(s[1:] != e[:-1])[0] + 1
                for first, last in zip(np.concatenate(([0], breaks)).tolist(),
                                       np.concatenate((breaks, [len(s)])).tolist()):
                    self._file.write(values[int(s[first]):int(e[last - 1])])
            self._add(valid, np.where(valid, ends - starts, 0))

    @staticmethod
    def _parts(payloads: Any):
        if isinstance(payloads, pa.ChunkedArray):
            if not _is_binary(payloads.type):
                raise ValueError(f"Packed blob payloads must have a Binary Arrow type, got {_arrow_name(payloads.type)}")
            return [_array_ranges(chunk, f"Packed blob payload chunk {k} contains invalid Arrow data: ")
                    for k, chunk in enumerate(payloads.chunks)]
        if isinstance(payloads, pa.Array):
            if not _is_binary(payloads.type):
                raise ValueError(f"Packed blob payload array must have a Binary Arrow type, got {_arrow_name(payloads.type)}")
            return [_array_ranges(payloads, "Packed blob payload array contains invalid Arrow data: ")]
        raise ValueError("payloads must be a pyarrow BinaryArray, LargeBinaryArray, or ChunkedArray, "
                         f"got {type(payloads).__name__}")

    def _add(self, valid: np.ndarray, sizes: np.ndarray) -> None:
        positions = self._offset + np.cumsum(sizes) - sizes
        self._offset += int(sizes.sum())
        self._segments.append((valid, positions, sizes))

    def _descriptors(self) -> List[BlobDescriptor]:
        out: List[BlobDescriptor] = []
        for valid, positions, sizes in self._segments:
            for v, p, s in zip(valid.tolist(), positions.tolist(), sizes.tolist()):
                out.append(BlobDescriptor._packed(self._blob_id, p, s) if v else _NULL)
        return out

    def finish(self) -> List[BlobDescriptor]:
        self._check_open()
        self._close()
        return self._descriptors()

    def finish_array(self, field_name: str) -> pa.StructArray:
        self._check_open()
        self._close()
        self._field = _descriptor_field(field_name)
        return _struct_array(self._descriptors())

    @property
    def field(self) -> pa.Field:
        if self._field is None:
            raise ValueError("PackedBlobWriter field is available after finish_array")
        return self._field


class DedicatedBlobWriter(_SidecarWriter):
    """Writes one blob into a sidecar file of its own. Created with
    :meth:`LanceFileSession.open_dedicated_blob_writer`; :meth:`write` may be called several times and the
    bytes are concatenated; :meth:`finish` returns the descriptor."""

    def __init__(self, base: str, data_path: str, blob_id: int):
        super().__init__(base, data_path, blob_id)
        self._size = 0

    def write(self, data: Any) -> None:
        self._check_open()
        payload = _payload_bytes(data)
        self._file.write(payload)
        self._size += len(payload)

    def finish(self) -> BlobDescriptor:
        self._check_open()
        self._close()
        return BlobDescriptor._dedicated(self._blob_id, self._size)


class BlobDescriptorArrayBuilder:
    """Collects descriptors into a struct array of the ``lance.blob.v2`` descriptor type, for a data file
    whose blob column holds them. :attr:`field` is the field to put in the schema."""

    def __init__(self, field_name: str):
        self._field = _descriptor_field(field_name)
        self._rows: List[BlobDescriptor] = []
        self._finished = False

    @property
    def field(self) -> pa.Field:
        return self._field

    def _check_open(self) -> None:
        if self._finished:
            raise ValueError("BlobDescriptorArrayBuilder is already finished")

    def append(self, descriptor: BlobDescriptor) -> None:
        self._check_open()
        if not isinstance(descriptor, BlobDescriptor):
            if descriptor is None:
                raise TypeError("'None' is not an instance of 'BlobDescriptor'")
            raise TypeError(f"'{type(descriptor).__name__}' object is not an instance of 'BlobDescriptor'")
        self._rows.append(descriptor)

    def extend(self, descriptors: Iterable[BlobDescriptor]) -> None:
        for descriptor in descriptors:
            self.append(descriptor)

    def append_null(self) -> None:
        self.append(_NULL)

    def append_inline(self, data: Any) -> None:
        self.append(BlobDescriptor._inline(_payload_bytes(data)))

    def append_dedicated(self, blob_id: int, size: int) -> None:
        self.append(BlobDescriptor._dedicated(blob_id, size))

    def extend_packed(self, blob_id: int, offsets: Sequence[int], sizes: Sequence[int]) -> None:
        offsets, sizes = list(offsets), list(sizes)
        if len(offsets) != len(sizes):
            raise ValueError(f"offsets and sizes must have the same length, got {len(offsets)} and {len(sizes)}")
        for offset, size in zip(offsets, sizes):
            self.append(BlobDescriptor._packed(blob_id, offset, size))

    def finish(self) -> pa.StructArray:
        self._check_open()
        self._finished = True
        return _struct_array(self._rows)
