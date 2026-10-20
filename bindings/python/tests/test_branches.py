"""Branches written by pylance (B1: reading them, and appending to them, from nanolance).

Each test builds its branches with pylance, the engine that creates them (nanolance does not create
branches yet), and checks that nanolance opens them, lists them and resolves their references as
pylance does, and that nanolance's appends and overwrites land on the branch and read back in pylance.
"""

from __future__ import annotations

import os

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lance as nl
from tests.support import require_pylance


@pytest.fixture
def lance():
    return require_pylance()


def _rows(ds) -> list:
    return ds.to_table().to_pylist()


def _main_and_branch(lance, root):
    """main at v2 (4 rows); branch ``feat`` forked from main's v2 and appended to once (its v3, 5 rows)."""
    main = lance.write_dataset(pa.table({"a": [1, 2, 3], "b": [4, 5, 6]}), root)
    main = lance.write_dataset(pa.table({"a": [7], "b": [8]}), root, mode="append")
    branch = main.create_branch("feat", main.version)
    branch = lance.write_dataset(pa.table({"a": [11], "b": [12]}), branch, mode="append")
    assert branch.version == 3
    return main, branch


def test_branch_dir_opens_as_its_own_dataset(lance, tmp_path):
    root = tmp_path / "ds"
    _main_and_branch(lance, root)
    branch_dir = str(root / "tree" / "feat")

    ours = nl.dataset(branch_dir)
    theirs = lance.dataset(branch_dir)
    assert ours.version == theirs.version == 3
    assert ours.to_table().to_pylist() == theirs.to_table().to_pylist()
    assert ours.checkout_version(2).to_table().num_rows == 4
    assert nl.dataset(branch_dir, version=2).to_table().to_pylist() == theirs.checkout_version(2).to_table().to_pylist()


def test_checkout_version_by_branch_reference(lance, tmp_path):
    root = tmp_path / "ds"
    main, branch = _main_and_branch(lance, root)
    ours = nl.dataset(str(root))

    latest = ours.checkout_version(("feat", None))
    assert latest.version == 3
    assert _rows(latest) == _rows(branch)

    at_fork = ours.checkout_version(("feat", 2))
    assert at_fork.version == 2
    assert _rows(at_fork) == _rows(branch.checkout_version(2))

    # main is unchanged by the branch: its own latest, and version 2 of it, read as before.
    assert _rows(ours.checkout_version(("main", None))) == _rows(main)
    assert ours.checkout_version(("main", None)).version == 2
    assert _rows(ours.checkout_version((None, 1))) == [{"a": 1, "b": 4}, {"a": 2, "b": 5}, {"a": 3, "b": 6}]

    # Within a branch, an integer version stays on that branch.
    assert latest.checkout_version(2).version == 2
    assert _rows(latest.checkout_version(2)) == _rows(at_fork)


def test_missing_branch_is_a_value_error(lance, tmp_path):
    root = tmp_path / "ds"
    _main_and_branch(lance, root)
    with pytest.raises(ValueError, match="Not found"):
        nl.dataset(str(root)).checkout_version(("nope", None))


def test_branches_list_matches_pylance(lance, tmp_path):
    root = tmp_path / "ds"
    main, branch = _main_and_branch(lance, root)
    main.create_branch("other", 1)  # forked from v1: a second branch, to order
    main.create_branch("team/x", 2)  # a name with a slash: its file is percent-encoded
    ours = nl.dataset(str(root))
    assert main.branches.list() == ours.branches.list()
    for order in (None, "asc", "desc"):
        assert main.branches.list_ordered(order) == ours.branches.list_ordered(order)
    assert [n for n, _ in ours.branches.list_ordered()] == ["feat", "team/x", "other"]
    assert nl.dataset(str(root / "tree" / "team" / "x")).version == 2

    listed = nl.dataset(str(root)).branches.list()
    assert set(listed) == {"feat", "other", "team/x"}
    feat = listed["feat"]
    assert feat["parent_branch"] is None
    assert feat["parent_version"] == 2
    assert feat["branch_identifier"][0][0] == 2 and isinstance(feat["branch_identifier"][0][1], str)
    assert feat["manifest_size"] > 0 and feat["metadata"] == {}


def test_branch_of_branch_reads_through_both_forks(lance, tmp_path):
    root = tmp_path / "ds"
    main, branch = _main_and_branch(lance, root)
    grandchild = branch.create_branch("sub", branch.version)  # forked from feat's v3
    grandchild = lance.write_dataset(pa.table({"a": [21], "b": [22]}), grandchild, mode="append")
    expected = _rows(grandchild)
    assert len(expected) == 6

    ours = nl.dataset(str(root)).checkout_version(("sub", None))
    assert ours.version == grandchild.version
    assert _rows(ours) == expected
    assert nl.dataset(str(root)).branches.list()["sub"]["parent_branch"] == "feat"
    assert _rows(nl.dataset(str(root / "tree" / "sub"))) == expected


def test_tags_on_branches_read(lance, tmp_path):
    root = tmp_path / "ds"
    main, branch = _main_and_branch(lance, root)
    branch.tags.create("feat_latest", ("feat", None))
    main.tags.create("main_latest", (None, None))

    ours = nl.dataset(str(root))
    tags = ours.tags.list()
    assert tags == lance.dataset(str(root)).tags.list()
    assert tags["feat_latest"]["branch"] == "feat"
    assert tags["main_latest"]["branch"] is None
    assert ours.tags.get_version("main_latest") == 2
    assert ours.tags.get_version("feat_latest") == lance.dataset(str(root)).tags.get_version("feat_latest") == 3
    assert _rows(ours.checkout_version("feat_latest")) == _rows(branch)
    assert ours.checkout_version("feat_latest").version == 3
    assert nl.dataset(str(root), version="feat_latest").version == 3
    assert _rows(nl.dataset(str(root), version="feat_latest")) == _rows(branch)
    # Opened on a branch's own directory, a tag still names its own branch or main.
    assert _rows(nl.dataset(str(root / "tree" / "feat"), version="main_latest")) == _rows(main)
    assert nl.dataset(str(root / "tree" / "feat"), version="feat_latest").version == 3


def test_append_on_pylance_branch_lands_on_branch(lance, tmp_path):
    root = tmp_path / "ds"
    main, branch = _main_and_branch(lance, root)
    main_before = _rows(lance.dataset(str(root)))
    branch_dir = str(root / "tree" / "feat")
    before_files = set(os.listdir(os.path.join(branch_dir, "data")))

    nd = nl.write_dataset(pa.table({"a": [31, 32], "b": [33, 34]}), nl.dataset(branch_dir), mode="append")
    assert nd.version == 4
    assert nd.uri == branch_dir

    # The new rows are a file of the branch's own data directory; the inherited ones are not copied.
    new_files = set(os.listdir(os.path.join(branch_dir, "data"))) - before_files
    assert len(new_files) == 1

    expected = [{"a": 1, "b": 4}, {"a": 2, "b": 5}, {"a": 3, "b": 6}, {"a": 7, "b": 8}, {"a": 11, "b": 12},
                {"a": 31, "b": 33}, {"a": 32, "b": 34}]
    assert _rows(nl.dataset(branch_dir)) == expected
    theirs = lance.dataset(branch_dir)
    assert theirs.version == 4
    assert _rows(theirs) == expected
    assert theirs.checkout_version(3).to_table().num_rows == 5

    # main: untouched on disk and to both readers.
    assert _rows(lance.dataset(str(root))) == main_before
    assert nl.dataset(str(root)).version == 2
    assert nl.dataset(str(root)).version_refs() == [{"version": 1}, {"version": 2}]


def test_overwrite_on_pylance_branch(lance, tmp_path):
    root = tmp_path / "ds"
    main, branch = _main_and_branch(lance, root)
    branch_dir = str(root / "tree" / "feat")
    nl.write_dataset(pa.table({"a": [90], "b": [91]}), branch_dir, mode="overwrite")

    theirs = lance.dataset(branch_dir)
    assert theirs.version == 4
    assert _rows(theirs) == [{"a": 90, "b": 91}]
    # Earlier versions of the branch still read as they did, inherited rows included.
    assert _rows(theirs.checkout_version(3)) == _rows(branch)
    assert _rows(nl.dataset(branch_dir, version=3)) == _rows(branch)
    assert _rows(lance.dataset(str(root))) == _rows(main)


def test_pylance_branch_of_nanolance_written_main_reads_back(lance, tmp_path):
    """A branch forked from a version nanolance wrote on main, appended to by nanolance, read by pylance."""
    root = tmp_path / "ds"
    lance.write_dataset(pa.table({"a": [1, 2]}), root)
    nl.write_dataset(pa.table({"a": [3]}), str(root), mode="append")
    main = lance.dataset(str(root))
    branch = main.create_branch("from_nano", main.version)
    nl.write_dataset(pa.table({"a": [4]}), str(root / "tree" / "from_nano"), mode="append")
    assert _rows(lance.dataset(str(root / "tree" / "from_nano"))) == [{"a": 1}, {"a": 2}, {"a": 3}, {"a": 4}]
    assert _rows(nl.dataset(str(root / "tree" / "from_nano"))) == [{"a": 1}, {"a": 2}, {"a": 3}, {"a": 4}]
    assert _rows(branch) == [{"a": 1}, {"a": 2}, {"a": 3}]
    assert lance.dataset(str(root)).version == 2


def test_inherited_indexes_answer_as_pylance(lance, tmp_path):
    root = tmp_path / "ds"
    rng = np.random.default_rng(0)
    n = 300
    vectors = rng.normal(size=(n, 8)).astype("float32")
    table = pa.table({
        "id": pa.array(range(n), pa.int64()),
        "k": pa.array([f"k{i % 7}" for i in range(n)]),
        "vec": pa.FixedSizeListArray.from_arrays(pa.array(vectors.ravel()), 8),
    })
    main = lance.write_dataset(table, root)
    main.create_scalar_index("id", index_type="BTREE")
    main.create_index("vec", index_type="IVF_PQ", num_partitions=4, num_sub_vectors=4)
    main = lance.dataset(str(root))
    branch = main.create_branch("indexed", main.version)
    # Rows appended on the branch are in a fragment the inherited indexes do not cover.
    branch = lance.write_dataset(table.slice(0, 20), branch, mode="append")

    branch_dir = str(root / "tree" / "indexed")
    ours = nl.dataset(branch_dir)
    theirs = lance.dataset(branch_dir)
    assert sorted(i["name"] for i in ours.list_indices()) == sorted(i["name"] for i in theirs.list_indices())
    assert all(i["base_id"] == 0 for i in ours.list_indices())

    assert ours.to_table(filter="id = 5", columns=["id"]).to_pylist() == \
        theirs.to_table(filter="id = 5", columns=["id"]).to_pylist()
    assert ours.to_table(filter="id > 290", columns=["id"]).num_rows == \
        theirs.to_table(filter="id > 290", columns=["id"]).num_rows
    assert ours.take([0, 5, 299]).to_pylist() == theirs.take([0, 5, 299]).to_pylist()
    query = [0.1] * 8
    ours_near = ours.to_table(nearest={"column": "vec", "q": query, "k": 5}, columns=["id"])
    theirs_near = theirs.to_table(nearest={"column": "vec", "q": query, "k": 5}, columns=["id"])
    assert ours_near.column("id").to_pylist() == theirs_near.column("id").to_pylist()
    assert ours.count_rows(filter="k = 'k3'") == theirs.count_rows(filter="k = 'k3'")


def test_inherited_full_text_index_answers_as_pylance(lance, tmp_path):
    root = tmp_path / "ds"
    words = ["alpha beta", "gamma delta", "alpha gamma", "beta epsilon"] * 25
    main = lance.write_dataset(pa.table({"id": range(len(words)), "text": words}), root)
    main.create_scalar_index("text", index_type="INVERTED")
    main = lance.dataset(str(root))
    branch = main.create_branch("words", main.version)
    lance.write_dataset(pa.table({"id": [1000], "text": ["alpha zeta"]}), branch, mode="append")

    branch_dir = str(root / "tree" / "words")
    ours = nl.dataset(branch_dir).to_table(full_text_query="alpha", columns=["id"])
    theirs = lance.dataset(branch_dir).to_table(full_text_query="alpha", columns=["id"])
    assert sorted(ours.column("id").to_pylist()) == sorted(theirs.column("id").to_pylist())
    assert 1000 in ours.column("id").to_pylist()


def test_inherited_deletions_read_as_pylance(lance, tmp_path):
    root = tmp_path / "ds"
    main = lance.write_dataset(pa.table({"a": list(range(10))}), root)
    main = lance.dataset(str(root))
    branch = main.create_branch("pruned", main.version)
    branch.delete("a < 3")
    branch = lance.dataset(str(root / "tree" / "pruned"))
    expected = list(range(3, 10))
    assert [r["a"] for r in _rows(branch)] == expected
    assert [r["a"] for r in _rows(nl.dataset(str(root / "tree" / "pruned")))] == expected
    assert nl.dataset(str(root)).count_rows() == 10
