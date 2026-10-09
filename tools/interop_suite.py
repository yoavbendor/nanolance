#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run bindings/python/tests/test_interop.py where its optional readers exist.

The main environment has DuckDB, Polars and pandas; two of the tests need more, each installed here
in a virtualenv of its own (over the system site-packages, so nanolance, pylance and pyarrow are the
ones under test):

* DuckDB's ``lance`` community extension, built only for some DuckDB releases (not 1.5.5): DuckDB
  ``DUCKDB_VERSION`` with the extension installed;
* LanceDB ``LANCEDB_VERSION``, whose own Rust Lance reads and writes the tables.

In each environment the test that needs it must run -- a skip there fails this script.

    python3 tools/interop_suite.py            # all three environments
"""

from __future__ import annotations

import subprocess
import sys
import venv
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PYTHON_DIR = ROOT / "bindings" / "python"
DEPS = ROOT / ".deps" / "interop"
DUCKDB_VERSION = "1.5.0"
LANCEDB_VERSION = "0.40.0"


def environment(name: str, requirement: str) -> Path:
    path = DEPS / name
    python = path / "bin" / "python"
    if not python.exists():
        venv.EnvBuilder(system_site_packages=True, with_pip=True).create(path)
    subprocess.run([str(python), "-m", "pip", "install", "-q", requirement], check=True)
    return python


def run(python: str, must_run: str = "") -> bool:
    args = [python, "-m", "pytest", "tests/test_interop.py", "-rs", "-p", "no:cacheprovider"]
    result = subprocess.run(args, cwd=PYTHON_DIR, capture_output=True, text=True)
    counts = [line for line in result.stdout.splitlines() if " passed" in line or " failed" in line]
    print("  " + (counts[-1].strip("= ") if counts else result.stderr[-2000:]))
    skipped = [line for line in result.stdout.splitlines() if line.startswith("SKIPPED") and must_run in line]
    if must_run and skipped:
        print(f"  {must_run} was skipped in its own environment:\n  " + "\n  ".join(skipped))
        return False
    if result.returncode != 0:
        print(result.stdout[-4000:])
    return result.returncode == 0


def main() -> int:
    ok = True
    print("main environment:")
    ok &= run(sys.executable)
    print(f"DuckDB {DUCKDB_VERSION} with the lance extension:")
    duck = environment("duckdb", f"duckdb=={DUCKDB_VERSION}")
    subprocess.run([str(duck), "-c", "import duckdb; duckdb.sql('INSTALL lance FROM community')"], check=True)
    ok &= run(str(duck), must_run="lance extension")
    print(f"LanceDB {LANCEDB_VERSION}:")
    ok &= run(str(environment("lancedb", f"lancedb=={LANCEDB_VERSION}")), must_run="lancedb")
    print("all interop tests passed" if ok else "interop tests FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
