#!/usr/bin/env python3
"""Generate small Arrow IPC and Lance v2.2 golden fixtures.

The script is intentionally optional for build machines that do not have the
Python Lance stack installed. Use --check-only in CTest-style validation to
report whether the environment can regenerate the fixtures.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path


def _import_or_skip():
    try:
        import lance  # type: ignore
        import polars as pl  # type: ignore
        import pyarrow as pa  # type: ignore
        import pyarrow.ipc as ipc  # type: ignore
    except Exception as exc:  # pragma: no cover - depends on local env
        print(f"golden fixture generation skipped: missing Python dependency: {exc}", file=sys.stderr)
        return None

    return lance, pl, pa, ipc


def _write_ipc(path: Path, table, ipc_module) -> None:
    with path.open("wb") as sink:
        with ipc_module.new_stream(sink, table.schema) as writer:
            writer.write_table(table)


def _polars_equal(ipc_path: Path, lance_path: Path, lance_module, pl_module) -> bool:
    left = pl_module.read_ipc_stream(ipc_path)
    right = pl_module.from_arrow(lance_module.dataset(str(lance_path)).to_table())
    return left.equals(right)


def generate(output_dir: Path) -> int:
    modules = _import_or_skip()
    if modules is None:
        return 77

    lance, pl, pa, ipc = modules
    output_dir.mkdir(parents=True, exist_ok=True)

    fixtures = {
        "primitive_nullable": pa.table(
            {
                "id": pa.array([1, 2, 3, 4], type=pa.int64()),
                "score": pa.array([1.0, None, 3.5, 4.25], type=pa.float64()),
                "flag": pa.array([True, False, None, True], type=pa.bool_()),
            }
        ),
        "strings_binary": pa.table(
            {
                "name": pa.array(["alpha", None, "gamma", "delta"], type=pa.string()),
                "payload": pa.array([b"a", b"bb", None, b"dddd"], type=pa.binary()),
            }
        ),
    }

    for name, table in fixtures.items():
        fixture_dir = output_dir / name
        lance_dir = fixture_dir / "dataset.lance"
        ipc_path = fixture_dir / "input.arrow"
        if fixture_dir.exists():
            shutil.rmtree(fixture_dir)
        fixture_dir.mkdir(parents=True)
        _write_ipc(ipc_path, table, ipc)
        lance.write_dataset(table, str(lance_dir), mode="create", data_storage_version="2.2")
        if not _polars_equal(ipc_path, lance_dir, lance, pl):
            print(f"fixture validation failed for {name}", file=sys.stderr)
            return 1

    print(f"generated Lance v2.2 golden fixtures in {output_dir}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).with_name("golden"))
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args(argv)

    modules = _import_or_skip()
    if args.check_only:
        if modules is None:
            return 77
        print("Python Lance, Polars, and PyArrow are available")
        return 0
    if modules is None:
        return 77
    return generate(args.output_dir)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
