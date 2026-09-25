"""Structural dictionary encoding for scattered low-cardinality strings."""

from __future__ import annotations

import os

import pyarrow as pa

import nanolance
from tests.support import require_pylance


def test_structural_dict_roundtrip_and_size(tmp_path):
    n = 50_000
    vals = [f"row-{i % 500}" for i in range(n)]
    table = pa.table({"s": pa.array(vals, type=pa.string())})

    plain_path = tmp_path / "plain.lance"
    dict_path = tmp_path / "dict.lance"

    # Structural encodings are on by default now, so a true "plain" baseline must disable them; the
    # dictionary encoding applies WITHOUT zstd (compression=False) since it is a structural encoding.
    nanolance.write_table(table, plain_path, structural_encoding=False)
    nanolance.write_table(table, dict_path, compression=False)

    back = pa.table(nanolance.read_table(dict_path))
    assert back.column("s").to_pylist() == vals

    plain_sz = os.path.getsize(next(plain_path.rglob("*.lance")))
    dict_sz = os.path.getsize(next(dict_path.rglob("*.lance")))
    assert dict_sz < plain_sz // 4, f"dict={dict_sz} plain={plain_sz}"


def test_stock_lance_reads_a_dictionary_encoded_column(tmp_path):
    """The dictionary page must declare the u32 chunk grammar, or Lance refuses the whole FILE.

    v2.2's validate_page_layout rejects any miniblock page whose layout says has_large_chunk=false,
    and it runs over the file's entire page table before decoding anything. So a single
    dictionary-encoded column made every other column in the same dataset unreadable by stock Lance
    -- which is why this test checks a neighbouring int column too, and reads them one at a time.

    The C++ smoke test covering this used to swallow the error and exit 77, so ctest reported a
    green SKIP for a genuine interop failure. Hence a real assertion here as well.
    """
    lance = require_pylance()
    n = 50_000
    table = pa.table(
        {
            "s": pa.array([f"row-{i % 500}" for i in range(n)], type=pa.string()),
            "i": pa.array(range(n), type=pa.int64()),
        }
    )
    path = tmp_path / "dict_interop.lance"
    nanolance.write_table(table, path, compression=True)

    dataset = lance.dataset(str(path))
    assert dataset.to_table().to_pydict() == table.to_pydict()
    for column in ("s", "i"):
        assert dataset.to_table(columns=[column]).column(0).to_pylist() == table.column(column).to_pylist()
