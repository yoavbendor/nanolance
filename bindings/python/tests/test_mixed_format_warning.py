"""MixedFormatWarning: a write that adds the first format-2.2 data file to a dataset whose data files are
all 2.0 or all 2.1 warns once, before it commits. Only pylance 12 and later read the dataset afterwards
(see test_format_v2_0.py and docs/PYLANCE_COMPAT.md). An ``error`` filter on the warning aborts the write.
"""

from __future__ import annotations

import os
import warnings

import pyarrow as pa
import pytest

import nanolance.lance as nl
from nanolance.lance import MixedFormatWarning
from tests.support import require_pylance

N_ROWS = 60


@pytest.fixture(scope="module")
def lance():
    return require_pylance()


def _rows(ids) -> pa.Table:
    ids = list(ids)
    return pa.table({
        "id": pa.array(ids, pa.int64()),
        "tag": pa.array([f"t{i % 7}" for i in ids]),
    })


def _older(lance, tmp_path, version: str) -> str:
    """A dataset of several small pylance-written fragments in format 2.0 or 2.1."""
    path = str(tmp_path / f"v{version}.lance")
    lance.write_dataset(_rows(range(N_ROWS)), path, data_storage_version=version, max_rows_per_file=20)
    assert lance.dataset(path).data_storage_version == version
    return path


def _append(path):
    nl.write_dataset(_rows(range(1000, 1010)), path, mode="append")


def _update(path):
    nl.dataset(path).update({"tag": "'changed'"}, where="id < 10")


def _merge_insert(path):
    nl.dataset(path).merge_insert("id").when_matched_update_all().when_not_matched_insert_all().execute(
        _rows(range(50, 70)))


def _add_columns(path):
    nl.dataset(path).add_columns({"doubled": "id * 2"})


def _compact(path):
    nl.dataset(path).optimize.compact_files()


def _fragment_create(path):
    # Written, not committed: the warning comes before the data file is written.
    nl.LanceFragment.create(path, _rows(range(2000, 2010)), mode="append")


OPS = {"append": _append, "update": _update, "merge_insert": _merge_insert, "add_columns": _add_columns,
       "compact": _compact, "fragment_create": _fragment_create}


def _mixed_warnings(recorded) -> list:
    return [w for w in recorded if issubclass(w.category, MixedFormatWarning)]


def _data_files(path: str) -> set:
    return set(os.listdir(os.path.join(path, "data")))


@pytest.mark.parametrize("version", ["2.0", "2.1"])
@pytest.mark.parametrize("op", list(OPS))
def test_first_2_2_file_warns_once(lance, tmp_path, recwarn, version, op):
    path = _older(lance, tmp_path, version)
    OPS[op](path)
    hits = _mixed_warnings(recwarn)
    assert len(hits) == 1, [str(w.message) for w in hits]
    message = str(hits[0].message)
    assert f"format 2.2 data files into a format {version} dataset" in message
    assert "Lance (pylance) 12 and later" in message
    assert "mode='overwrite'" in message
    assert f"use pylance to keep writing {version}" in message


@pytest.mark.parametrize("version", ["2.0", "2.1"])
def test_a_second_write_into_the_mixed_dataset_does_not_warn(lance, tmp_path, recwarn, version):
    path = _older(lance, tmp_path, version)
    _append(path)
    recwarn.clear()
    _append(path)
    _update(path)
    assert _mixed_warnings(recwarn) == []
    assert nl.dataset(path).count_rows() == N_ROWS + 20


@pytest.mark.parametrize("version", ["2.0", "2.1"])
def test_delete_writes_no_data_file_and_does_not_warn(lance, tmp_path, recwarn, version):
    path = _older(lance, tmp_path, version)
    before = _data_files(path)
    nl.dataset(path).delete("id < 5")
    assert _data_files(path) == before
    assert _mixed_warnings(recwarn) == []


def test_2_2_dataset_does_not_warn(tmp_path, recwarn):
    path = str(tmp_path / "v22.lance")
    nl.write_dataset(_rows(range(N_ROWS)), path, max_rows_per_file=20)
    recwarn.clear()
    _append(path)
    _update(path)
    _merge_insert(path)
    _add_columns(path)
    _compact(path)
    assert _mixed_warnings(recwarn) == []


def test_new_dataset_does_not_warn(tmp_path, recwarn):
    path = str(tmp_path / "new.lance")
    nl.write_dataset(_rows(range(N_ROWS)), path)
    assert _mixed_warnings(recwarn) == []


@pytest.mark.parametrize("version", ["2.0", "2.1"])
def test_overwrite_does_not_warn(lance, tmp_path, recwarn, version):
    path = _older(lance, tmp_path, version)
    nl.write_dataset(_rows(range(N_ROWS)), path, mode="overwrite")
    assert _mixed_warnings(recwarn) == []
    assert nl.dataset(path).data_storage_version == "2.2"


@pytest.mark.parametrize("version", ["2.0", "2.1"])
@pytest.mark.parametrize("op", list(OPS))
def test_error_filter_aborts_before_the_commit(lance, tmp_path, version, op):
    path = _older(lance, tmp_path, version)
    before_version = lance.dataset(path).version
    before_rows = lance.dataset(path).count_rows()
    before_files = _data_files(path)
    with warnings.catch_warnings():
        warnings.simplefilter("error", MixedFormatWarning)
        with pytest.raises(MixedFormatWarning):
            OPS[op](path)
    after = lance.dataset(path)
    assert after.version == before_version
    assert after.count_rows() == before_rows
    assert _data_files(path) == before_files
    assert after.data_storage_version == version


def test_mixed_format_warning_is_a_user_warning():
    assert issubclass(MixedFormatWarning, UserWarning)
    assert nl.MixedFormatWarning is MixedFormatWarning
