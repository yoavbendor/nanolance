# SPDX-License-Identifier: Apache-2.0
"""Writes to object-store URIs nanolance does not write (gs://, az://) are refused, and leave nothing
behind locally (no endpoint needed). s3:// writes are tested against an endpoint in
test_object_store_commits.py.

pathlib collapses "s3://bucket/x" into "s3:/bucket/x", so a writer that took the URI as a path would
create a local directory named "s3:". Every writer entry point refuses the URI before that happens.
"""

import os

import pyarrow as pa
import pytest

import nanolance
import nanolance.lance as nl


URIS = ["gs://bucket/x.lance", "az://container/x.lance"]


@pytest.fixture
def in_tmp(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    return tmp_path


@pytest.mark.parametrize("uri", URIS)
def test_low_level_write_table_refuses_uri(in_tmp, uri):
    with pytest.raises(NotImplementedError, match="are not supported"):
        nanolance.write_table(pa.table({"a": [1]}), uri)
    assert os.listdir(in_tmp) == []


@pytest.mark.parametrize("uri", URIS)
def test_low_level_writer_refuses_uri(in_tmp, uri):
    with pytest.raises(NotImplementedError, match="are not supported"):
        nanolance.LanceWriter(uri)
    assert os.listdir(in_tmp) == []


@pytest.mark.parametrize("uri", URIS)
def test_lance_write_dataset_refuses_other_schemes(in_tmp, uri):
    with pytest.raises(nl.NotSupportedError, match="storage scheme"):
        nl.write_dataset(pa.table({"a": [1]}), uri)
    assert os.listdir(in_tmp) == []
