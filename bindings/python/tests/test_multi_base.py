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
    indices = sorted({i for i in (0, 17, 101, 199, pl.count_rows() - 1) if i < pl.count_rows()})
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


def base_table(ds):
    """A dataset's base paths as (id, name, path, is_dataset_root), in id order."""
    return sorted((b.id, b.name, b.path, b.is_dataset_root) for b in ds._ds.base_paths().values())


def test_write_all_bases_by_nanolance_pylance_reads(lance, tmp_path, bases):
    """nanolance writes the rotation pylance writes: primary, then each base in turn."""
    p1, p2 = bases
    primary = tmp_path / "primary"
    nl.write_dataset(table(0, 300), str(primary),
                     initial_bases=[nl.DatasetBasePath(str(p1), name="path1"),
                                    nl.DatasetBasePath(str(p2), name="path2", is_dataset_root=True)],
                     target_all_bases=True, max_rows_per_file=100)
    assert base_ids(lance.dataset(str(primary))) == [[None], [1], [2]]
    assert (p1 / "data").exists() is False and list(p1.glob("*.lance"))
    assert list((p2 / "data").glob("*.lance"))
    assert_reads_match(lance, primary)


def test_same_calls_same_layout_on_both_engines(lance, tmp_path, bases):
    """The same write, each engine in its own directory: the same base ids, base paths and rows."""
    p1, p2 = bases
    engines = {"pylance": lance, "nanolance": nl}
    result = {}
    for name, module in engines.items():
        root = tmp_path / name
        root.mkdir()
        ds = module.write_dataset(table(0, 100), str(root / "primary"),
                                  initial_bases=[module.DatasetBasePath(str(root / "p1"), name="path1")],
                                  target_bases=["path1"], max_rows_per_file=50)
        ds = module.write_dataset(table(100, 50), ds, mode="append", target_bases=["path1"], max_rows_per_file=25)
        # The base paths relative to each engine's own root: the layout, not the directory it sits in.
        bases_relative = [(b[0], b[1], b[3]) for b in base_table(ds)]
        result[name] = (base_ids(ds), bases_relative, rows(ds))
    assert result["nanolance"] == result["pylance"]
    assert result["pylance"][0] == [[1], [1], [1], [1]]


def test_append_and_overwrite_with_bases_by_nanolance(lance, tmp_path, bases):
    """An append targets a base by name or by path; an overwrite keeps the bases and writes the primary."""
    p1, p2 = bases
    primary = tmp_path / "primary"
    ds = nl.write_dataset(table(0, 100), str(primary),
                          initial_bases=[nl.DatasetBasePath(str(p1), name="path1"),
                                         nl.DatasetBasePath(str(p2), name="path2")],
                          target_bases=["path1"], max_rows_per_file=50)
    ds = nl.write_dataset(table(100, 50), ds, mode="append", target_bases=[str(p2)], max_rows_per_file=25)
    assert base_ids(lance.dataset(str(primary))) == [[1], [1], [2], [2]]
    ds = nl.write_dataset(table(200, 30), ds, mode="overwrite", max_rows_per_file=10)
    assert base_ids(lance.dataset(str(primary))) == [[None]] * 3
    assert len(base_table(ds)) == 2
    # After an overwrite, fragment ids go on from the old ones, so the addresses assert_reads_match takes
    # are gone: compare the rows and the layout.
    pl = lance.dataset(str(primary))
    assert rows(ds) == rows(pl)
    assert base_ids(ds) == base_ids(pl)


def test_write_validation_refused_like_pylance(lance, tmp_path, bases):
    p1, p2 = bases
    primary = tmp_path / "primary"
    with pytest.raises(OSError, match="not found"):
        nl.write_dataset(table(0, 10), str(primary), initial_bases=[nl.DatasetBasePath(str(p1), name="path1")],
                         target_bases=["path9"])
    assert not primary.exists()
    ds = nl.write_dataset(table(0, 10), str(primary), initial_bases=[nl.DatasetBasePath(str(p1), name="path1")],
                          target_bases=["path1"])
    with pytest.raises(OSError, match="Cannot register new bases in Append mode"):
        nl.write_dataset(table(10, 10), ds, mode="append",
                         initial_bases=[nl.DatasetBasePath(str(p2), name="path2")], target_bases=["path2"])


def test_add_bases_by_nanolance_pylance_reads(lance, tmp_path, bases):
    """add_bases returns the dataset moved to the new version; pylance reads the bases and the rows."""
    p1, p2 = bases
    primary = tmp_path / "primary"
    nl.write_dataset(table(0, 40), str(primary), initial_bases=[nl.DatasetBasePath(str(p1), name="path1")],
                     target_bases=["path1"], max_rows_per_file=20)
    ds = nl.dataset(str(primary))
    assert ds.add_bases([nl.DatasetBasePath(str(p2), name="path2", is_dataset_root=True)],
                        transaction_properties={"team": "data"}) is ds
    assert ds.version == 2
    ds = nl.write_dataset(table(40, 20), ds, mode="append", target_bases=["path2"], max_rows_per_file=20)
    pl = lance.dataset(str(primary))
    assert base_table(pl) == base_table(ds)
    assert [b[1:] for b in base_table(pl)] == [("path1", str(p1), False), ("path2", str(p2), True)]
    assert pl.read_transaction(2).transaction_properties["team"] == "data"
    assert base_ids(pl) == [[1], [1], [2]]
    assert_reads_match(lance, primary)


def test_add_bases_duplicate_refused_both_engines(lance, tmp_path, bases):
    p1, p2 = bases
    messages = []
    for module in (lance, nl):
        primary = tmp_path / module.__name__.replace(".", "_")
        ds = module.write_dataset(table(0, 10), str(primary),
                                  initial_bases=[module.DatasetBasePath(str(p1), name="path1")],
                                  target_bases=["path1"])
        with pytest.raises(Exception) as excinfo:
            ds.add_bases([module.DatasetBasePath(str(p2), name="path1")])
        messages.append("conflict" in str(excinfo.value).lower())
    assert messages == [True, True]


def test_write_fragments_into_target_base_same_on_both_engines(lance, tmp_path, bases):
    """write_fragments(target_bases=) routes its fragments to the named base: the same layout on both
    engines, and pylance reads what nanolance committed."""
    p1, p2 = bases
    result = {}
    for name, module in {"pylance": lance, "nanolance": nl}.items():
        root = tmp_path / name
        root.mkdir()
        primary = str(root / "primary")
        module.write_dataset(table(0, 100), primary,
                             initial_bases=[module.DatasetBasePath(str(root / "p1"), name="path1"),
                                            module.DatasetBasePath(str(root / "p2"), name="path2")],
                             target_bases=["path1"], max_rows_per_file=50)
        ds = module.dataset(primary)
        fragments = module.fragment.write_fragments(table(100, 50), ds, mode="append", target_bases=["path2"],
                                                    max_rows_per_file=25)
        module.LanceDataset.commit(primary, module.LanceOperation.Append(fragments), read_version=ds.version)
        result[name] = (base_ids(module.dataset(primary)), rows(module.dataset(primary)))
    assert result["nanolance"] == result["pylance"]
    assert result["pylance"][0] == [[1], [1], [2], [2]]
    assert_reads_match(lance, tmp_path / "nanolance" / "primary")


def test_merge_insert_into_target_base_same_on_both_engines(lance, tmp_path, bases):
    """merge_insert(target_bases=) writes its matched and new rows into the named base, as pylance does."""
    p1, p2 = bases
    result = {}
    for name, module in {"pylance": lance, "nanolance": nl}.items():
        root = tmp_path / name
        root.mkdir()
        primary = str(root / "primary")
        module.write_dataset(table(0, 100), primary,
                             initial_bases=[module.DatasetBasePath(str(root / "p1"), name="path1"),
                                            module.DatasetBasePath(str(root / "p2"), name="path2")],
                             target_bases=["path1"], max_rows_per_file=50)
        stats = (module.dataset(primary).merge_insert("id").when_matched_update_all()
                 .when_not_matched_insert_all().target_bases(["path2"]).execute(table(50, 100)))
        ds = module.dataset(primary)
        result[name] = (stats["num_updated_rows"], stats["num_inserted_rows"], base_ids(ds), rows(ds))
    assert result["nanolance"] == result["pylance"]
    assert result["pylance"][:2] == (50, 50)
    assert result["pylance"][2][-1] == [2]
    # pylance reads the merge nanolance committed, rows and base ids alike.
    pl = lance.dataset(str(tmp_path / "nanolance" / "primary"))
    assert (base_ids(pl), rows(pl)) == (result["nanolance"][2], result["nanolance"][3])
