# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
#
# What cibuildwheel runs against each built wheel, in a fresh interpreter with only the wheel and
# pyarrow installed. It is deliberately NOT the pytest suite: that needs pylance, polars and pandas,
# which would test those packages' wheels rather than this one. What matters here is that the wheel
# is self-contained -- the extension loads with no zstd or nanoarrow on the system -- and that a
# round trip through it gives back what went in, nulls included.
#
# Run it by hand the same way: `python bindings/python/tests/wheel_smoke.py`.

import sys
import tempfile
from pathlib import Path

import pyarrow as pa

import nanolance


def main() -> int:
    print(f"nanolance {nanolance.__version__} from {nanolance.__file__}")

    table = pa.table(
        {
            "i": pa.array([1, None, 3], type=pa.int64()),
            "f": pa.array([1.5, 2.5, None], type=pa.float64()),
            "s": pa.array(["alpha", None, ""], type=pa.string()),
            "b": pa.array([True, False, None]),
            "t": pa.array([1_700_000_000_000, None, 1], type=pa.timestamp("ms")),
        }
    )

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "smoke.lance"
        nanolance.write_table(table, path)
        back = pa.table(nanolance.read_table(path))

    if back.column_names != table.column_names:
        print(f"FAIL: columns {back.column_names} != {table.column_names}", file=sys.stderr)
        return 1
    for name in table.column_names:
        if back.column(name).to_pylist() != table.column(name).to_pylist():
            print(f"FAIL: column {name!r} differs", file=sys.stderr)
            print(f"  got      {back.column(name).to_pylist()}", file=sys.stderr)
            print(f"  expected {table.column(name).to_pylist()}", file=sys.stderr)
            return 1

    print("wheel smoke passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
