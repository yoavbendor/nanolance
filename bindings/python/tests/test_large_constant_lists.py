# SPDX-License-Identifier: Apache-2.0
"""Lists of large repeated values: pylance stores a list<binary> of one value repeated as a constant page,
which nanolance expands. The expansion reserves its size, and the first page's buffers are taken rather than
copied (pylance's test_file.py::test_batch_sizes: 300 KB x 1,009 rows read 105 times, 55 s, now 15 s).
Every case is checked against pylance reading the same file."""

import pyarrow as pa
import pytest

from tests.support import require_pylance

from nanolance.lance.file import LanceFileReader


def _write(lance, path, table):
    from lance.file import LanceFileWriter

    with LanceFileWriter(str(path)) as writer:
        writer.write_batch(table)


@pytest.mark.parametrize("case", ["constant", "constant_then_varied", "varied_then_constant", "small", "empty_values"])
def test_constant_list_pages_read_as_pylance(tmp_path, case):
    lance = require_pylance()
    big = b"x" * 299_593
    if case == "constant":
        rows = [[big] for _ in range(1009)]
    elif case == "constant_then_varied":
        rows = [[big] for _ in range(600)] + [[bytes([i % 251]) * (i % 977), b"y"] for i in range(600)]
    elif case == "varied_then_constant":
        rows = [[bytes([i % 251]) * (i % 977)] for i in range(600)] + [[big] for _ in range(600)]
    elif case == "small":
        rows = [[b"ab", b"cd"] for _ in range(5000)]
    else:
        rows = [[b""] for _ in range(3000)]
    table = pa.table({"val": rows})
    path = tmp_path / "f.lance"
    _write(lance, path, table)
    expected = lance.file.LanceFileReader(str(path)).read_all().to_table()
    for batch_size in (10, 110, 1030):
        got = LanceFileReader(str(path)).read_all(batch_size=batch_size).to_table()
        assert got.equals(expected), (case, batch_size)
