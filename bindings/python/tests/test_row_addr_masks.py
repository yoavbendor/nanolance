"""Row-address masks: ``serialize_row_addrs`` bytes, and ``row_addr_allowlist`` / ``row_addr_blocklist``
on scans, filters, KNN and full-text search, each checked against pylance on the same data.

The pylance oracle for the scan behaviour is tests/pylance_suite (test_row_addr_prefilter.py).
"""

from __future__ import annotations

import numpy as np
import pyarrow as pa
import pytest

from nanolance.lance.dataset import ScannerBuilder, serialize_row_addrs
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


N = 64
DIM = 4


@pytest.fixture(params=["nanolance", "pylance"])
def engine(request):
    if request.param == "pylance":
        return require_pylance()
    import nanolance.lance as nanolance_lance

    return nanolance_lance


def _write(engine, path, with_index=False, text_index=False):
    rng = np.random.default_rng(1234)
    vectors = rng.standard_normal((N, DIM)).astype(np.float32)
    tbl = pa.table(
        {
            "id": pa.array(range(N), pa.int64()),
            "vector": pa.FixedSizeListArray.from_arrays(pa.array(vectors.reshape(-1), pa.float32()), DIM),
            "text": pa.array([f"row {i} lorem ipsum" for i in range(N)]),
        }
    )
    ds = engine.write_dataset(tbl, str(path), mode="overwrite")
    if with_index:
        ds.create_index("vector", index_type="IVF_FLAT", num_partitions=4, metric="l2")
    if text_index:
        ds.create_scalar_index("text", index_type="INVERTED")
    return ds, vectors


def _addrs(ds):
    return ds.to_table(with_row_id=True)["_rowid"].to_pylist()


def _mask(engine, addrs):
    """serialize_row_addrs of the engine under test (its dataset module; `lance.dataset` is a function on pylance)."""
    import importlib

    return importlib.import_module(engine.__name__ + ".dataset").serialize_row_addrs(addrs)


def test_mask_scan_keeps_only_allowed_rows(engine, tmp_path) -> None:
    ds, _ = _write(engine, tmp_path / "t.lance")
    got = ds.scanner(columns=["id"], row_addr_allowlist=_mask(engine, _addrs(ds)[3:9])).to_table()
    assert got["id"].to_pylist() == list(range(3, 9))


def test_mask_allow_and_block_combine(engine, tmp_path) -> None:
    ds, _ = _write(engine, tmp_path / "t.lance")
    addrs = _addrs(ds)
    got = ds.scanner(
        columns=["id"],
        row_addr_allowlist=_mask(engine, addrs[:10]),
        row_addr_blocklist=_mask(engine, addrs[5:15]),
    ).to_table()
    assert got["id"].to_pylist() == list(range(5))
    got = ds.scanner(columns=["id"], row_addr_blocklist=_mask(engine, addrs[:5])).to_table()
    assert got["id"].to_pylist() == list(range(5, N))


def test_mask_empty_allowlist_selects_nothing(engine, tmp_path) -> None:
    ds, _ = _write(engine, tmp_path / "t.lance")
    assert ds.scanner(row_addr_allowlist=_mask(engine, [])).to_table().num_rows == 0


def test_mask_composes_with_filter_offset_and_limit(engine, tmp_path) -> None:
    ds, _ = _write(engine, tmp_path / "t.lance")
    addrs = _addrs(ds)
    got = ds.scanner(columns=["id"], filter="id % 2 == 0", row_addr_allowlist=_mask(engine, addrs[:20])).to_table()
    assert got["id"].to_pylist() == [i for i in range(20) if i % 2 == 0]
    # Offset and limit count the rows the mask lets through (ids 10..29 here).
    got = ds.scanner(columns=["id"], offset=5, limit=4, row_addr_allowlist=_mask(engine, addrs[10:30])).to_table()
    assert got["id"].to_pylist() == [15, 16, 17, 18]


def test_mask_builder_setter_matches_kwarg(engine, tmp_path) -> None:
    ds, _ = _write(engine, tmp_path / "t.lance")
    blob = _mask(engine, _addrs(ds)[2:7])
    from_kwarg = ds.scanner(columns=["id"], row_addr_allowlist=blob).to_table()
    assert from_kwarg["id"].to_pylist() == list(range(2, 7))
    if engine.__name__ == "nanolance.lance":
        from_builder = ScannerBuilder(ds).columns(["id"]).row_addr_prefilter(allowlist=blob).to_scanner().to_table()
        assert from_builder["id"].to_pylist() == from_kwarg["id"].to_pylist()


def test_mask_rejects_malformed_bytes(engine, tmp_path) -> None:
    ds, _ = _write(engine, tmp_path / "t.lance")
    with pytest.raises(Exception, match="(?i)row address mask|invalid"):
        ds.scanner(row_addr_allowlist=b"not a treemap").to_table()


@pytest.mark.parametrize("with_index", [False, True], ids=["flat", "ivf"])
def test_mask_knn_topk_is_over_allowed_rows(engine, tmp_path, with_index) -> None:
    ds, vectors = _write(engine, tmp_path / "t.lance", with_index=with_index)
    addrs = _addrs(ds)
    allowed = addrs[10:20]
    query = np.zeros(DIM, dtype=np.float32)
    got = ds.scanner(
        columns=["id"], nearest={"column": "vector", "q": query, "k": 3, "nprobes": 4},
        row_addr_allowlist=_mask(engine, allowed),
    ).to_table()
    expect = sorted(allowed, key=lambda a: np.linalg.norm(vectors[a] - query))[:3]
    assert sorted(got["id"].to_pylist()) == sorted(expect)


def test_mask_knn_blocklist_refills(engine, tmp_path) -> None:
    ds, vectors = _write(engine, tmp_path / "t.lance")
    addrs = _addrs(ds)
    query = np.zeros(DIM, dtype=np.float32)
    order = sorted(addrs, key=lambda a: np.linalg.norm(vectors[a] - query))
    got = ds.scanner(
        columns=["id"], nearest={"column": "vector", "q": query, "k": 3},
        row_addr_blocklist=_mask(engine, order[:1]),
    ).to_table()
    assert got.num_rows == 3
    assert order[0] not in got["id"].to_pylist()


def test_mask_full_text_search_honors_allowlist(engine, tmp_path) -> None:
    ds, _ = _write(engine, tmp_path / "t.lance", text_index=True)
    allowed = _addrs(ds)[50:60]
    got = ds.scanner(
        columns=["id"], full_text_query="lorem", limit=5, row_addr_allowlist=_mask(engine, allowed),
    ).to_table()
    assert got.num_rows == 5
    assert set(got["id"].to_pylist()) <= set(range(50, 60))
