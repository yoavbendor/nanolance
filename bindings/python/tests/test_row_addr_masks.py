"""Row-address masks: ``serialize_row_addrs`` bytes, checked against pylance.

The scan, KNN and full-text behaviour of ``row_addr_allowlist`` / ``row_addr_blocklist`` is not
implemented yet (see docs/tasks/SMALL_ITEMS_1.md, item 1); this file covers the serialization that
the masks are passed in.
"""

from __future__ import annotations

import pytest

from nanolance.lance.dataset import serialize_row_addrs
from tests.support import require_pylance

CASES = [
    [],
    [0],
    [1, 2, (1 << 32) | 5],
    [5, 3, 1, 2],
    [(7 << 32) | 0xFFFFFFFF, (7 << 32) | 1, (2 << 32) | 70000],
    list(range(0, 10000, 3)),
    [(f << 32) | o for f in (0, 1, 3) for o in range(0, 9000, 1500)],
]


@pytest.fixture(scope="module")
def pylance_serialize():
    require_pylance()
    from lance.dataset import serialize_row_addrs as official

    return official


@pytest.mark.parametrize("addrs", CASES, ids=lambda a: f"n={len(a)}")
def test_serialize_row_addrs_matches_pylance(pylance_serialize, addrs) -> None:
    assert serialize_row_addrs(addrs) == bytes(pylance_serialize(addrs))


def test_serialize_row_addrs_known_bytes() -> None:
    # pylance's output for [1, 2, (1 << 32) | 5]: 2 fragments; fragment 0 (id 0) with a 20-byte
    # roaring bitmap whose cookie 12346 (0x303a) comes first.
    blob = serialize_row_addrs([1, 2, (1 << 32) | 5])
    assert len(blob) == 58
    assert blob[:12] == bytes.fromhex("02000000" "00000000" "14000000")
    assert blob[12:14] == bytes.fromhex("3a30")


def test_serialize_row_addrs_rejects_out_of_range() -> None:
    with pytest.raises(ValueError):
        serialize_row_addrs([-1])
    with pytest.raises(ValueError):
        serialize_row_addrs([1 << 64])
