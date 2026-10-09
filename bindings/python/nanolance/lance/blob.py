# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.blob``: Blob v2 columns -- writing them, and file-like access to their values.

A blob column is written from :func:`blob_array` values under a :func:`blob_field`: bytes, or a URI
(with an optional range) of an object kept outside the dataset. Lance stores each blob by its size --
in the data file (inline), in a sidecar file shared with other blobs (packed), or in a sidecar file
of its own (dedicated) -- or keeps the reference (external).

``LanceDataset.take_blobs`` returns one :class:`BlobFile` per row. A blob is read where it is stored,
and only the bytes asked for are read.
"""

from __future__ import annotations

import ctypes
import io
from dataclasses import dataclass
from typing import IO, Any, Iterator, List, Optional, Tuple, Union

import pyarrow as pa

from .. import _nanolance
from ._errors import native

__all__ = ["Blob", "BlobArray", "BlobColumn", "BlobFile", "BlobIterator", "BlobType", "blob_array", "blob_field"]

_BLOB_INLINE_SIZE_THRESHOLD_META_KEY = b"lance-encoding:blob-inline-size-threshold"
_BLOB_DEDICATED_SIZE_THRESHOLD_META_KEY = b"lance-encoding:blob-dedicated-size-threshold"
_BLOB_PACK_FILE_SIZE_THRESHOLD_META_KEY = b"lance-encoding:blob-pack-file-size-threshold"
_MAX_RUST_USIZE = ctypes.c_size_t(-1).value


@dataclass(frozen=True)
class Blob:
    """A logical blob value for writing Lance blob columns: inline bytes, or an external URI
    (optionally with a non-empty range). Use ``None`` for a null blob and :meth:`empty` for a valid
    empty one. Mirrors ``lance.Blob``."""

    data: Optional[bytes] = None
    uri: Optional[str] = None
    position: Optional[int] = None
    size: Optional[int] = None

    def __post_init__(self) -> None:
        if self.data is not None and self.uri is not None:
            raise ValueError("Blob cannot have both data and uri")
        if self.uri == "":
            raise ValueError("Blob uri cannot be empty")
        if (self.position is not None or self.size is not None) and self.uri is None:
            raise ValueError("External packed blob must have a uri")
        if (self.position is None) != (self.size is None):
            raise ValueError("External blob must set both position and size, or neither")
        if self.data is not None and self.position is not None:
            raise ValueError("Blob cannot have both inline data and external slice metadata")
        if self.data is None and self.uri is None:
            raise ValueError("Blob must set `data` or `uri`; use None for a null blob")
        if self.size == 0:
            raise ValueError("External blob range size must be greater than zero")

    @staticmethod
    def from_bytes(data: Union[bytes, bytearray, memoryview]) -> "Blob":
        return Blob(data=bytes(data))

    @staticmethod
    def from_uri(uri: str, position: Optional[int] = None, size: Optional[int] = None) -> "Blob":
        if uri == "":
            raise ValueError("Blob uri cannot be empty")
        if (position is not None and position < 0) or (size is not None and size < 0):
            raise ValueError("External blob position and size must be non-negative")
        return Blob(uri=uri, position=position, size=size)

    @staticmethod
    def empty() -> "Blob":
        return Blob(data=b"")


_STORAGE_FIELDS = [("data", pa.large_binary()), ("uri", pa.utf8()), ("position", pa.uint64()),
                   ("size", pa.uint64())]


class BlobType(pa.ExtensionType):
    """The Arrow extension type of a Lance blob column (``lance.blob.v2``): storage
    ``struct<data: large_binary, uri: utf8, position: uint64, size: uint64>`` (or the minimal
    ``struct<data, uri>``). Mirrors ``lance.blob.BlobType``."""

    def __init__(self) -> None:
        storage = pa.struct([pa.field(name, typ, nullable=True) for name, typ in _STORAGE_FIELDS])
        pa.ExtensionType.__init__(self, storage, "lance.blob.v2")

    def __arrow_ext_serialize__(self) -> bytes:
        return b""

    @staticmethod
    def _validate_storage_type(storage_type: pa.DataType) -> None:
        if not pa.types.is_struct(storage_type):
            raise TypeError("BlobType storage type must be a struct")
        fields = list(storage_type)
        if len(fields) not in (2, 4):
            raise TypeError("BlobType storage struct must contain either data/uri or data/uri/position/size")
        for index, field in enumerate(fields):
            expected_name, expected_type = _STORAGE_FIELDS[index]
            if field.name != expected_name or field.type != expected_type:
                raise TypeError(f"BlobType storage field {index} must be {expected_name}: {expected_type}, got "
                                f"{field.name}: {field.type}")
            if index < 2 and not field.nullable:
                raise TypeError(f"BlobType storage field {field.name} must be nullable")

    @classmethod
    def _from_storage_type(cls, storage_type: pa.DataType) -> "BlobType":
        cls._validate_storage_type(storage_type)
        instance = cls.__new__(cls)
        pa.ExtensionType.__init__(instance, storage_type, "lance.blob.v2")
        return instance

    @classmethod
    def __arrow_ext_deserialize__(cls, storage_type: pa.DataType, serialized: bytes) -> "BlobType":
        return cls._from_storage_type(storage_type)

    def __arrow_ext_class__(self):
        return BlobArray

    def __reduce__(self):
        return type(self).__arrow_ext_deserialize__, (self.storage_type, self.__arrow_ext_serialize__())


try:
    pa.register_extension_type(BlobType())
except pa.ArrowKeyError:
    pass  # registered already (by pylance, in the same interpreter): the same type


class BlobArray(pa.ExtensionArray):
    """An Arrow array of a blob column. Build one with :meth:`from_pylist` or :func:`blob_array`."""

    @classmethod
    def from_pylist(cls, values: List[Any]) -> "BlobArray":
        data, uris, positions, sizes, nulls = [], [], [], [], []
        for v in values:
            if v is None:
                d, u, p, s, null = None, None, None, None, True
            elif type(v).__name__ == "Blob" and hasattr(v, "uri") and hasattr(v, "data"):
                d, u, p, s, null = v.data, v.uri, v.position, v.size, False
            elif isinstance(v, str):
                if v == "":
                    raise ValueError("Blob uri cannot be empty")
                d, u, p, s, null = None, v, None, None, False
            elif isinstance(v, (bytes, bytearray, memoryview)):
                d, u, p, s, null = bytes(v), None, None, None, False
            else:
                raise TypeError(f"BlobArray values must be bytes-like, str (URI), Blob, or None; got {type(v)}")
            data.append(d)
            uris.append(u)
            positions.append(p)
            sizes.append(s)
            nulls.append(null)
        storage = pa.StructArray.from_arrays(
            [pa.array(data, pa.large_binary()), pa.array(uris, pa.utf8()), pa.array(positions, pa.uint64()),
             pa.array(sizes, pa.uint64())],
            names=["data", "uri", "position", "size"], mask=pa.array(nulls, pa.bool_()))
        return pa.ExtensionArray.from_storage(BlobType(), storage)  # type: ignore[return-value]


def blob_array(values: List[Any]) -> BlobArray:
    """A blob array from Python values: bytes-like (inline bytes), str (an external URI), :class:`Blob`,
    or None (null). Mirrors ``lance.blob_array``."""
    return BlobArray.from_pylist(values)


def _validate_threshold(name: str, value: Optional[int], *, allow_zero: bool) -> None:
    if value is None:
        return
    if isinstance(value, bool) or not isinstance(value, int):
        raise TypeError(f"{name} must be an int, got {type(value).__name__}")
    if allow_zero:
        if value < 0:
            raise ValueError(f"{name} must be non-negative")
    elif value <= 0:
        raise ValueError(f"{name} must be positive")
    if value > _MAX_RUST_USIZE:
        raise OverflowError(f"{name} must fit in a Rust usize")


def blob_field(name: str, *, nullable: bool = True, inline_size_threshold: Optional[int] = None,
               dedicated_size_threshold: Optional[int] = None,
               pack_file_size_threshold: Optional[int] = None) -> pa.Field:
    """An Arrow field for a Lance blob column. Blobs of more than ``inline_size_threshold`` bytes
    (default 64 KiB) go to a packed sidecar file, of more than ``dedicated_size_threshold`` (default
    4 MiB) to a sidecar file of their own; a packed sidecar holds at most ``pack_file_size_threshold``
    bytes (default 1 GiB). Mirrors ``lance.blob_field``."""
    _validate_threshold("inline_size_threshold", inline_size_threshold, allow_zero=True)
    _validate_threshold("dedicated_size_threshold", dedicated_size_threshold, allow_zero=False)
    _validate_threshold("pack_file_size_threshold", pack_file_size_threshold, allow_zero=False)
    field = pa.field(name, BlobType(), nullable=nullable)
    if inline_size_threshold is None and dedicated_size_threshold is None and pack_file_size_threshold is None:
        return field
    metadata = dict(field.metadata or {})
    if inline_size_threshold is not None:
        metadata[_BLOB_INLINE_SIZE_THRESHOLD_META_KEY] = str(inline_size_threshold).encode()
    if dedicated_size_threshold is not None:
        metadata[_BLOB_DEDICATED_SIZE_THRESHOLD_META_KEY] = str(dedicated_size_threshold).encode()
    if pack_file_size_threshold is not None:
        metadata[_BLOB_PACK_FILE_SIZE_THRESHOLD_META_KEY] = str(pack_file_size_threshold).encode()
    return field.with_metadata(metadata)


class BlobIterator:
    def __init__(self, binary_iter: Iterator[pa.BinaryScalar]):
        self.binary_iter = binary_iter

    def __next__(self) -> Optional[IO[bytes]]:
        value = next(self.binary_iter)
        if value is None:
            return None
        return io.BytesIO(value.as_py())


class BlobColumn:
    """A binary column's rows as file-like objects. Mirrors ``lance.BlobColumn``."""

    def __init__(self, blob_column: Union[pa.Array, pa.ChunkedArray]):
        if not isinstance(blob_column, (pa.Array, pa.ChunkedArray)):
            raise ValueError(f"Expected a pyarrow.Array or pyarrow.ChunkedArray, got {type(blob_column)}")
        if not pa.types.is_large_binary(blob_column.type) and not pa.types.is_binary(blob_column.type):
            raise ValueError(f"Expected a binary array, got {blob_column.type}")
        self.blob_column = blob_column

    def __iter__(self) -> Iterator[IO[bytes]]:
        return BlobIterator(iter(self.blob_column))


class BlobFile(io.RawIOBase):
    """One blob as a read-only, seekable file. Mirrors ``lance.BlobFile``."""

    def __init__(self, file: str, external: bool, position: int, size: int):
        super().__init__()
        self._file = file
        self._external = bool(external)
        self._position = int(position)
        self._size = int(size)
        self._cursor = 0

    def _read(self, offset: int, length: int) -> bytes:
        if length <= 0:
            return b""
        with native():
            return _nanolance._blob_read(self._file, self._external, self._position, self._size, offset, length)

    def readable(self) -> bool:
        return True

    def seekable(self) -> bool:
        return True

    def seek(self, offset: int, whence: int = io.SEEK_SET) -> int:
        if whence == io.SEEK_SET:
            target = offset
        elif whence == io.SEEK_CUR:
            target = self._cursor + offset
        elif whence == io.SEEK_END:
            target = self._size + offset
        else:
            raise ValueError(f"Invalid whence: {whence}")
        if target < 0:
            raise ValueError("negative seek position")
        self._cursor = target
        return self._cursor

    def tell(self) -> int:
        return self._cursor

    def size(self) -> int:
        """The size of the blob in bytes."""
        return self._size

    def readall(self) -> bytes:
        data = self._read(self._cursor, max(self._size - self._cursor, 0))
        self._cursor = max(self._cursor, self._size)
        return data

    def readinto(self, b) -> int:
        n = min(len(b), max(self._size - self._cursor, 0))
        data = self._read(self._cursor, n)
        b[:n] = data
        self._cursor += n
        return n

    def read_range(self, offset: int, length: int) -> bytes:
        """A blob-relative byte range, without moving the cursor."""
        if offset < 0 or length < 0 or offset + length > self._size:
            raise ValueError("the range is outside the blob")
        return self._read(offset, length)

    def read_ranges(self, ranges: List[Tuple[int, int]]) -> List[bytes]:
        return [self.read_range(offset, length) for offset, length in ranges]

    def __repr__(self) -> str:
        return f"<BlobFile size={self.size()}>"
