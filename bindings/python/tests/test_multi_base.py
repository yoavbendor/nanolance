"""Multi-base datasets: nanolance reads what pylance writes with base paths.

pylance 12 keeps data files outside the dataset's own directory when given ``initial_bases`` /
``target_bases``. Every case writes such a dataset with pylance, then reads it with nanolance and
compares what each reads back: rows, filters, takes, counts, blobs, and each data file's ``base_id``.
"""

from __future__ import annotations

import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


@pytest.fixture
def bases(tmp_path):
    p1, p2 = tmp_path / "p1", tmp_path / "p2"
    p1.mkdir()
    p2.mkdir()
    return p1, p2


def table(start, n):
    return pa.table({"id": pa.array(range(start, start + n), pa.int64()),
                     "v": pa.array([i * 10 for i in range(start, start + n)], pa.int64())})


def rows(ds, **kwargs):
    t = ds.to_table(**kwargs)
    return sorted(zip(*(t[c].to_pylist() for c in t.column_names)))


def base_ids(ds):
    return [[f.base_id for f in frag.data_files()] for frag in ds.get_fragments()]


def assert_reads_match(lance, path, *, keys=(0, 1, 2)):
    """nanolance reads the pylance-written dataset exactly as pylance does."""
    pl = lance.dataset(str(path))
    nn = nl.dataset(str(path))
    assert nn.count_rows() == pl.count_rows()
    assert rows(nn) == rows(pl)
    assert rows(nn, filter="id > 150 AND id < 260") == rows(pl, filter="id > 150 AND id < 260")
    assert rows(nn, columns=["v"], filter="id % 7 = 3") == rows(pl, columns=["v"], filter="id % 7 = 3")
    indices = [0, 17, 101, 199, pl.count_rows() - 1]
    assert nn.take(indices).to_pydict() == pl.take(indices).to_pydict()
    # fragment << 32 | row, in fragments 1 and 2. Live rows only: _take_rows of a deleted row's address
    # returns it (on plain datasets too), which is not what this file is about.
    addresses = [(1 << 32) | 6, (2 << 32) | 7]
    assert nn._take_rows(addresses, columns=["id"]).to_pydict() == pl._take_rows(addresses, columns=["id"]).to_pydict()
    assert base_ids(nn) == base_ids(pl)
    assert [[f.path for f in frag.data_files()] for frag in nn.get_fragments()] == \
        [[f.path for f in frag.data_files()] for frag in pl.get_fragments()]


def test_initial_bases_all_targets(lance, tmp_path, bases):
    """The primary, a non-root base and a root base in use at once."""
    p1, p2 = bases
    primary = tmp_path / "primary"
    lance.write_dataset(table(0, 300), primary,
                        initial_bases=[lance.DatasetBasePath(str(p1), name="path1"),
                                       lance.DatasetBasePath(str(p2), name="path2", is_dataset_root=True)],
                        target_all_bases=True, max_rows_per_file=100)
    assert {b for ids in base_ids(lance.dataset(str(primary))) for b in ids} == {None, 1, 2}
    assert_reads_match(lance, primary)


def test_non_root_base_only(lance, tmp_path, bases):
    p1, _ = bases
    primary = tmp_path / "primary"
    lance.write_dataset(table(0, 250), primary, initial_bases=[lance.DatasetBasePath(str(p1), name="path1")],
                        target_bases=["path1"], max_rows_per_file=100)
    assert_reads_match(lance, primary)


def test_root_base_only(lance, tmp_path, bases):
    _, p2 = bases
    primary = tmp_path / "primary"
    lance.write_dataset(table(0, 250), primary,
                        initial_bases=[lance.DatasetBasePath(str(p2), name="path2", is_dataset_root=True)],
                        target_bases=["path2"], max_rows_per_file=100)
    assert_reads_match(lance, primary)


def test_append_to_a_base(lance, tmp_path, bases):
    p1, p2 = bases
    primary = tmp_path / "primary"
    lance.write_dataset(table(0, 150), primary,
                        initial_bases=[lance.DatasetBasePath(str(p1), name="path1"),
                                       lance.DatasetBasePath(str(p2), name="path2", is_dataset_root=True)],
                        target_all_bases=True, max_rows_per_file=100)
    lance.write_dataset(table(150, 120), primary, mode="append", target_bases=["path2"], max_rows_per_file=100)
    assert_reads_match(lance, primary)


def test_deletions_of_rows_in_base_fragments(lance, tmp_path, bases):
    p1, p2 = bases
    primary = tmp_path / "primary"
    ds = lance.write_dataset(table(0, 300), primary,
                             initial_bases=[lance.DatasetBasePath(str(p1), name="path1"),
                                            lance.DatasetBasePath(str(p2), name="path2", is_dataset_root=True)],
                             target_all_bases=True, max_rows_per_file=100)
    ds.delete("id % 4 = 1 OR id > 270")
    pl = lance.dataset(str(primary))
    assert pl.count_rows() < 300
    assert_reads_match(lance, primary)


def test_scalar_index_over_base_fragments(lance, tmp_path, bases):
    p1, _ = bases
    primary = tmp_path / "primary"
    ds = lance.write_dataset(table(0, 300), primary, initial_bases=[lance.DatasetBasePath(str(p1), name="path1")],
                             target_all_bases=True, max_rows_per_file=100)
    ds.create_scalar_index("id", "BTREE")
    assert_reads_match(lance, primary)
    assert rows(nl.dataset(str(primary)), filter="id = 205") == rows(lance.dataset(str(primary)), filter="id = 205")


def test_blobs_in_base_packed_and_dedicated(lance, tmp_path, bases):
    p1, _ = bases
    primary = tmp_path / "primary"
    small = [bytes([i]) * 64 for i in range(20)]
    big = [bytes([i]) * (5 * 1024 * 1024) for i in range(2)]
    blobs = small + big
    schema = pa.schema([pa.field("id", pa.int64()), lance.blob_field("blob")])
    t = pa.table({"id": pa.array(range(len(blobs)), pa.int64()), "blob": lance.blob_array(blobs)}, schema=schema)
    lance.write_dataset(t, primary, initial_bases=[lance.DatasetBasePath(str(p1), name="path1")],
                        target_all_bases=True, max_rows_per_file=100)
    nn = nl.dataset(str(primary))
    pl = lance.dataset(str(primary))
    assert rows(nn, columns=["id"]) == rows(pl, columns=["id"])
    wanted = [0, 5, 19, 20, 21]
    got = [b.read() for b in nn.take_blobs("blob", indices=wanted)]
    assert got == [blobs[i] for i in wanted]
    assert [f.base_id for f in nn.get_fragments()[0].data_files()] == \
        [f.base_id for f in pl.get_fragments()[0].data_files()]
