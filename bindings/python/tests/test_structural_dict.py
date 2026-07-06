"""Structural dictionary encoding for scattered low-cardinality strings."""

from __future__ import annotations

import os

import pyarrow as pa

import nanolance


def test_structural_dict_roundtrip_and_size(tmp_path):
    n = 50_000
    vals = [f"row-{i % 500}" for i in range(n)]
    table = pa.table({"s": pa.array(vals, type=pa.string())})

    plain_path = tmp_path / "plain.lance"
    dict_path = tmp_path / "dict.lance"

    nanolance.write_table(table, plain_path, compression=False)
    nanolance.write_table(table, dict_path, compression=True)

    back = pa.table(nanolance.read_table(dict_path))
    assert back.column("s").to_pylist() == vals

    plain_sz = os.path.getsize(next(plain_path.rglob("*.lance")))
    dict_sz = os.path.getsize(next(dict_path.rglob("*.lance")))
    assert dict_sz < plain_sz // 4, f"dict={dict_sz} plain={plain_sz}"
