"""nanolance reads s3:// datasets, checked against pylance on the same dataset.

Skipped unless NANOLANCE_S3_TEST_ENDPOINT names an S3-compatible endpoint (moto's server locally, MinIO
in CI). Each test writes its dataset with pylance under a fresh prefix of NANOLANCE_S3_TEST_BUCKET (default
"nl-test"; the bucket must exist), then reads it with nanolance and with pylance and compares the two. The
credentials are AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY / AWS_REGION (default "testing", "testing",
"us-east-1"), passed as storage_options the way pylance takes them.
"""

from __future__ import annotations

import os
import uuid

import numpy as np
import pyarrow as pa
import pytest

import lance
import nanolance
import nanolance.lance as nl

ENDPOINT = os.environ.get("NANOLANCE_S3_TEST_ENDPOINT", "")
BUCKET = os.environ.get("NANOLANCE_S3_TEST_BUCKET", "nl-test")
ROWS = 1000
FRAGMENT_ROWS = 334  # 1,000 rows in 3 fragments

pytestmark = pytest.mark.skipif(not ENDPOINT, reason="NANOLANCE_S3_TEST_ENDPOINT is not set (no S3 endpoint)")


def options(spelling: str = "pylance") -> dict:
    key = os.environ.get("AWS_ACCESS_KEY_ID", "testing")
    secret = os.environ.get("AWS_SECRET_ACCESS_KEY", "testing")
    region = os.environ.get("AWS_REGION", "us-east-1")
    if spelling == "aws":
        return {"aws_access_key_id": key, "aws_secret_access_key": secret, "aws_region": region,
                "aws_endpoint": ENDPOINT, "allow_http": "true"}
    return {"access_key_id": key, "secret_access_key": secret, "region": region, "endpoint": ENDPOINT,
            "allow_http": "true"}


def fresh_uri(name: str = "ds") -> str:
    return f"s3://{BUCKET}/nanolance-{uuid.uuid4().hex}/{name}.lance"


def sample_table(rows: int = ROWS) -> pa.Table:
    rng = np.random.default_rng(7)
    return pa.table({
        "id": pa.array(range(rows), pa.int64()),
        "name": pa.array([f"row {i}" for i in range(rows)]),
        "text": pa.array([f"alpha beta gamma{i % 13}" for i in range(rows)]),
        "score": pa.array(rng.random(rows)),
        "vec": pa.FixedSizeListArray.from_arrays(pa.array(rng.random(rows * 8).astype("float32")), 8),
    })


def written_by_pylance(table: pa.Table, uri: str, **kwargs) -> str:
    lance.write_dataset(table, uri, storage_options=options(), max_rows_per_file=FRAGMENT_ROWS, **kwargs)
    return uri


def both(uri: str, spelling: str = "pylance"):
    """(nanolance, pylance) datasets at uri, opened with the same storage options."""
    return nl.dataset(uri, storage_options=options(spelling)), lance.dataset(uri, storage_options=options())


def assert_same_table(ours: pa.Table, theirs: pa.Table) -> None:
    assert ours.column_names == theirs.column_names
    assert ours.num_rows == theirs.num_rows
    assert ours.to_pydict() == theirs.to_pydict()


@pytest.fixture
def uri() -> str:
    return written_by_pylance(sample_table(), fresh_uri())


def test_whole_table_matches_pylance(uri):
    ours, theirs = both(uri)
    assert ours.count_rows() == theirs.count_rows() == ROWS
    assert_same_table(ours.to_table(), theirs.to_table())


def test_projection_matches_pylance(uri):
    ours, theirs = both(uri)
    assert_same_table(ours.to_table(columns=["name", "score"]), theirs.to_table(columns=["name", "score"]))


@pytest.mark.parametrize("filter_sql", ["id = 777", "name = 'row 5'", "score > 0.5 AND id < 300", "id IN (1, 2, 3)"])
def test_filter_matches_pylance(uri, filter_sql):
    ours, theirs = both(uri)
    assert_same_table(ours.to_table(filter=filter_sql, columns=["id", "name"]),
                      theirs.to_table(filter=filter_sql, columns=["id", "name"]))
    assert ours.count_rows(filter_sql) == theirs.count_rows(filter_sql)


def test_take_matches_pylance(uri):
    ours, theirs = both(uri)
    rows = [0, 500, 999, 333, 334]  # across all three fragments, out of order
    assert_same_table(ours.take(rows), theirs.take(rows))


def test_btree_search_matches_pylance(uri):
    lance.dataset(uri, storage_options=options()).create_scalar_index("id", "BTREE")
    ours, theirs = both(uri)
    assert "id_idx" in [i["name"] for i in ours.list_indices()]
    assert_same_table(ours.to_table(filter="id = 777", columns=["id"]),
                      theirs.to_table(filter="id = 777", columns=["id"]))
    assert_same_table(ours.to_table(filter="id BETWEEN 100 AND 110", columns=["id"]),
                      theirs.to_table(filter="id BETWEEN 100 AND 110", columns=["id"]))


def test_full_text_search_matches_pylance(uri):
    lance.dataset(uri, storage_options=options()).create_scalar_index("text", "INVERTED")
    ours, theirs = both(uri)
    ours_hits = sorted(ours.to_table(full_text_query="gamma3", columns=["id"]).column("id").to_pylist())
    theirs_hits = sorted(theirs.to_table(full_text_query="gamma3", columns=["id"]).column("id").to_pylist())
    assert ours_hits == theirs_hits
    assert len(ours_hits) == len(range(3, ROWS, 13))


def test_ivf_flat_search_matches_pylance(uri):
    theirs = lance.dataset(uri, storage_options=options())
    theirs.create_index("vec", index_type="IVF_FLAT", num_partitions=4, metric="l2")
    ours, theirs = both(uri)
    query = np.random.default_rng(11).random(8).astype("float32")
    ours_hits = ours.to_table(nearest={"column": "vec", "q": query, "k": 5}, columns=["id"])
    theirs_hits = theirs.to_table(nearest={"column": "vec", "q": query, "k": 5}, columns=["id"])
    assert ours_hits.column("id").to_pylist() == theirs_hits.column("id").to_pylist()


def test_deletions_match_pylance(uri):
    theirs = lance.dataset(uri, storage_options=options())
    theirs.delete("id % 10 = 3")
    ours, theirs = both(uri)
    assert ours.count_rows() == theirs.count_rows() == ROWS - ROWS // 10
    assert_same_table(ours.to_table(columns=["id"]), theirs.to_table(columns=["id"]))


def test_two_versions_match_pylance(uri):
    theirs = lance.dataset(uri, storage_options=options())
    theirs.delete("id < 100")
    ours, theirs = both(uri)
    assert [v["version"] for v in ours.versions()] == [v["version"] for v in theirs.versions()] == [1, 2]
    assert ours.version == theirs.version == 2
    for version in (1, 2):
        old_ours = nl.dataset(uri, version=version, storage_options=options())
        old_theirs = lance.dataset(uri, version=version, storage_options=options())
        assert old_ours.count_rows() == old_theirs.count_rows()
        assert_same_table(old_ours.to_table(columns=["id"]), old_theirs.to_table(columns=["id"]))
    # checkout_version re-opens the dataset by its uri: it must keep the storage options.
    checked_out = nl.dataset(uri, storage_options=options()).checkout_version(1)
    assert checked_out.version == 1
    assert_same_table(checked_out.to_table(columns=["id"]),
                      lance.dataset(uri, version=1, storage_options=options()).to_table(columns=["id"]))


def test_filtered_scan_reads_a_fraction_of_the_dataset():
    # 20,000 rows in 8 fragments: a one-row filter must not read the dataset's bytes, one read at a time.
    uri = written_by_pylance(sample_table(20000), fresh_uri("big"))
    ds = nl.dataset(uri, storage_options=options())
    nanolance._reset_work_stats()
    ds.to_table()
    full = nanolance._work_stats()
    nanolance._reset_work_stats()
    hit = ds.to_table(filter="id = 777", columns=["id"])
    filtered = nanolance._work_stats()
    assert hit.column("id").to_pylist() == [777]
    assert filtered["data_bytes_read"] <= full["data_bytes_read"] // 4, (filtered, full)
    assert filtered["largest_read"] <= 64 * 1024, filtered


def test_storage_option_spellings_agree(uri):
    plain = nl.dataset(uri, storage_options=options("pylance")).to_table(columns=["id"])
    prefixed = nl.dataset(uri, storage_options=options("aws")).to_table(columns=["id"])
    assert_same_table(plain, prefixed)


WRITES = {
    "delete": lambda ds, uri: ds.delete("id = 1"),
    "update": lambda ds, uri: ds.update({"name": "'x'"}, where="id = 1"),
    "add_columns": lambda ds, uri: ds.add_columns({"extra": "id + 1"}),
    "drop_columns": lambda ds, uri: ds.drop_columns(["score"]),
    "create_scalar_index": lambda ds, uri: ds.create_scalar_index("id", "BTREE"),
    "create_vector_index": lambda ds, uri: ds.create_index("vec", index_type="IVF_FLAT", num_partitions=4),
    "drop_index": lambda ds, uri: ds.drop_index("id_idx"),
    "update_config": lambda ds, uri: ds.update_config({"k": "v"}),
    "update_metadata": lambda ds, uri: ds.update_metadata({"k": "v"}),
    "restore": lambda ds, uri: ds.restore(),
    "create_tag": lambda ds, uri: ds.tags.create("t1", 1),
    "cleanup_old_versions": lambda ds, uri: ds.cleanup_old_versions(retain_versions=1),
    "compact_files": lambda ds, uri: ds.optimize.compact_files(),
    "optimize_indices": lambda ds, uri: ds.optimize.optimize_indices(),
    "merge_insert": lambda ds, uri: ds.merge_insert("id").when_matched_update_all().execute(sample_table(10)),
    "drop_dataset": lambda ds, uri: nl.LanceDataset.drop(uri, storage_options=options()),
    "fragment_delete": lambda ds, uri: ds.get_fragments()[0].delete("id = 1"),
    "fragment_create": lambda ds, uri: nl.LanceFragment.create(uri, sample_table(10)),
}


@pytest.mark.parametrize("write", list(WRITES.values()), ids=list(WRITES.keys()))
def test_writes_to_object_stores_are_refused_by_name(uri, write):
    ds = nl.dataset(uri, storage_options=options())
    with pytest.raises(nl.NotSupportedError, match="object-store writes") as refused:
        write(ds, uri)
    assert "s3://" in str(refused.value)
    assert ds.count_rows() == ROWS  # nothing was touched


LANCE_C_S3_READ = os.environ.get("NANOLANCE_LANCE_C_S3_READ", "")


@pytest.mark.skipif(not LANCE_C_S3_READ, reason="NANOLANCE_LANCE_C_S3_READ is not set (lance-c's s3 reader)")
def test_lance_c_reads_s3_dataset():
    """liblance_c's lance_dataset_open takes the storage options (tests/lance_c_s3_read.cpp)."""
    import subprocess

    uri = written_by_pylance(sample_table(), fresh_uri())
    ds = lance.dataset(uri, storage_options=options())
    ds.delete("id < 10")
    ds = lance.dataset(uri, storage_options=options())
    rows = [0, 5, 500, ds.count_rows() - 1]
    out = subprocess.run([LANCE_C_S3_READ, uri, ENDPOINT, *map(str, rows)], capture_output=True, text=True,
                         check=False)
    assert out.returncode == 0, out.stderr
    ids = ds.take(rows, columns=["id"]).column("id").to_pylist()
    expected = [f"version {ds.version}", f"rows {ds.count_rows()}"]
    expected += [f"id {r} {i}" for r, i in zip(rows, ids)]
    assert out.stdout.splitlines() == expected
