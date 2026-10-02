# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.blob``: file-like access to the values of a Blob v2 column.

``LanceDataset.take_blobs`` returns one :class:`BlobFile` per row. A blob is read where Lance stored
it -- in the data file (inline), in a sidecar file shared with other blobs (packed), in a sidecar
file of its own (dedicated), or at an external URI -- and only the bytes asked for are read.
"""

from __future__ import annotations

import io
from typing import List, Tuple

from .. import _nanolance
from ._errors import native, unsupported

__all__ = ["BlobFile"]


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


def blob_field(*args, **kwargs):
    raise unsupported("writing Lance's Blob v2 layouts (lance.blob_field); nanolance writes external blobs")


def blob_array(*args, **kwargs):
    raise unsupported("writing Lance's Blob v2 layouts (lance.blob_array); nanolance writes external blobs")
