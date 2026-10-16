# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor

"""``lance.bitmap``: the set of fragment ids an index segment covers (pylance's Roaring-backed
``Bitmap``, here over a Python set). A ``MutableSet``: it compares equal to a set of the same ids."""

from __future__ import annotations

import operator
import sys
from collections.abc import Iterable, MutableSet

__all__ = ["Bitmap"]

_U32 = 2**32
# Arrow's own names for the types pylance reports in its errors (str(pa.float64()) is "double").
_ARROW_NAMES = {"double": "Float64", "float": "Float32", "halffloat": "Float16", "string": "Utf8",
                "large_string": "LargeUtf8", "bool": "Boolean"}


def _arrow_module():
    """pyarrow when the caller has imported it (an Arrow input implies it is loaded), else None."""
    return sys.modules.get("pyarrow")


def _from_arrow(arr, pa) -> set:
    """The values of an integer Arrow array (or chunked array), with pylance's checks."""
    chunks = arr.chunks if isinstance(arr, pa.ChunkedArray) else [arr]
    if any(c.null_count for c in chunks):
        raise ValueError("Bitmap cannot be constructed from an array containing nulls")
    if not pa.types.is_integer(arr.type):
        name = str(arr.type)
        name = _ARROW_NAMES.get(name, name)
        raise ValueError(f"Bitmap can only be constructed from an integer pyarrow array, got {name}")
    out = set()
    for chunk in chunks:
        for v in chunk.to_pylist():
            if not 0 <= v < _U32:
                raise ValueError("Bitmap values must fit in an unsigned 32-bit integer: "
                                 f"Cast error: Can't cast value {v} to type UInt32")
            out.add(v)
    return out


def _u32_values(values: Iterable[int]) -> set:
    """Bulk form (constructor, update, set operators): ints in u32 range, else ValueError / TypeError."""
    pa = _arrow_module()
    if pa is not None and isinstance(values, (pa.Array, pa.ChunkedArray)):
        return _from_arrow(values, pa)
    out = set()
    for v in values:
        n = operator.index(v)  # TypeError for non-integers, as pylance
        if not 0 <= n < _U32:
            raise ValueError(f"Bitmap values must fit in an unsigned 32-bit integer, got {n}")
        out.add(n)
    return out


def _u32_value(value) -> int:
    """Single-value form (add, discard, remove): pyo3's OverflowError for values outside u32."""
    try:
        n = operator.index(value)
    except TypeError as exc:  # pyo3's argument message, as pylance raises it
        raise TypeError(f"argument 'value': {exc}") from None
    if not 0 <= n < _U32:
        raise OverflowError("out of range integral type conversion attempted")
    return n


class Bitmap(MutableSet):
    __slots__ = ("_ids",)

    def __init__(self, values: Iterable[int] = ()):
        self._ids = _u32_values(values)

    def __contains__(self, value) -> bool:
        return value in self._ids

    def __iter__(self):
        return iter(sorted(self._ids))

    def __len__(self) -> int:
        return len(self._ids)

    def __eq__(self, other) -> bool:
        if isinstance(other, (list, tuple)):  # pylance's Bitmap equals the ascending list of its values
            return sorted(self._ids) == list(other)
        return super().__eq__(other)

    def add(self, value: int) -> None:
        self._ids.add(_u32_value(value))

    def discard(self, value: int) -> None:
        self._ids.discard(_u32_value(value))

    def remove(self, value: int) -> None:
        self._ids.remove(_u32_value(value))

    def copy(self) -> "Bitmap":
        return Bitmap(self._ids)

    def update(self, *others: Iterable[int]) -> None:
        for other in others:
            self._ids.update(_u32_values(other))

    def union(self, *others: Iterable[int]) -> "Bitmap":
        out = self.copy()
        out.update(*others)
        return out

    def intersection(self, *others: Iterable[int]) -> "Bitmap":
        return Bitmap(self._ids.intersection(*[_u32_values(o) for o in others]))

    def difference(self, *others: Iterable[int]) -> "Bitmap":
        return Bitmap(self._ids.difference(*[_u32_values(o) for o in others]))

    def symmetric_difference(self, other: Iterable[int]) -> "Bitmap":
        return Bitmap(self._ids.symmetric_difference(_u32_values(other)))

    def issubset(self, other: Iterable[int]) -> bool:
        return self._ids.issubset(_u32_values(other))

    def issuperset(self, other: Iterable[int]) -> bool:
        return self._ids.issuperset(_u32_values(other))

    def __hash__(self):
        return None.__hash__()  # unhashable, as a set

    def __repr__(self) -> str:
        return f"Bitmap({sorted(self._ids)!r})" if self._ids else "Bitmap()"


def _serialize_roaring(values: Iterable[int]) -> bytes:
    """`values` (u32) in the portable RoaringBitmap format, as roaring-rs writes it without run
    containers: array containers up to 4096 values, bitmap containers above."""
    import struct

    by_key = {}
    for v in sorted(set(int(x) for x in values)):
        by_key.setdefault(v >> 16, []).append(v & 0xFFFF)
    keys = sorted(by_key)
    out = bytearray(struct.pack("<II", 12346, len(keys)))
    for k in keys:
        out += struct.pack("<HH", k, len(by_key[k]) - 1)
    offset = len(out) + 4 * len(keys)
    bodies = []
    for k in keys:
        low = by_key[k]
        if len(low) <= 4096:
            body = struct.pack(f"<{len(low)}H", *low)
        else:
            words = [0] * 1024
            for v in low:
                words[v >> 6] |= 1 << (v & 63)
            body = struct.pack("<1024Q", *words)
        out += struct.pack("<I", offset)
        offset += len(body)
        bodies.append(body)
    for body in bodies:
        out += body
    return bytes(out)


def _bitmap_serialize(self) -> bytes:
    return _serialize_roaring(self._ids)


Bitmap.serialize = _bitmap_serialize
