# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor

"""``lance.bitmap``: the set of fragment ids an index segment covers (pylance's Roaring-backed
``Bitmap``, here over a Python set). A ``MutableSet``: it compares equal to a set of the same ids."""

from __future__ import annotations

from collections.abc import Iterable, MutableSet

__all__ = ["Bitmap"]


class Bitmap(MutableSet):
    __slots__ = ("_ids",)

    def __init__(self, values: Iterable[int] = ()):
        self._ids = set()
        for v in values:
            self.add(v)

    def __contains__(self, value) -> bool:
        return value in self._ids

    def __iter__(self):
        return iter(sorted(self._ids))

    def __len__(self) -> int:
        return len(self._ids)

    def add(self, value: int) -> None:
        value = int(value)
        if not 0 <= value < 2**32:
            raise OverflowError(f"bitmap values are u32, got {value}")
        self._ids.add(value)

    def discard(self, value: int) -> None:
        self._ids.discard(value)

    def remove(self, value: int) -> None:
        self._ids.remove(value)

    def copy(self) -> "Bitmap":
        return Bitmap(self._ids)

    def update(self, *others: Iterable[int]) -> None:
        for other in others:
            for v in other:
                self.add(v)

    def union(self, *others: Iterable[int]) -> "Bitmap":
        out = self.copy()
        out.update(*others)
        return out

    def intersection(self, *others: Iterable[int]) -> "Bitmap":
        return Bitmap(self._ids.intersection(*[set(o) for o in others]))

    def difference(self, *others: Iterable[int]) -> "Bitmap":
        return Bitmap(self._ids.difference(*[set(o) for o in others]))

    def symmetric_difference(self, other: Iterable[int]) -> "Bitmap":
        return Bitmap(self._ids.symmetric_difference(set(other)))

    def issubset(self, other: Iterable[int]) -> bool:
        return self._ids.issubset(set(other))

    def issuperset(self, other: Iterable[int]) -> bool:
        return self._ids.issuperset(set(other))

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
