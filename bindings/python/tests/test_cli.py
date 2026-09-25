"""`python -m nanolance convert|inspect` -- the parquet on-ramp (plan item 3.4)."""

from __future__ import annotations

import os
import sys

import pyarrow as pa
import pytest

import nanolance
from nanolance.__main__ import main
from tests.support import require_pylance

pq = pytest.importorskip("pyarrow.parquet")


@pytest.fixture
def source_parquet(tmp_path):
    n = 20_000
    table = pa.table(
        {
            "id": pa.array(range(n), type=pa.int64()),
            "level": pa.array([["INFO", "WARN", "ERROR"][i % 3] for i in range(n)]),
            "val": pa.array([i * 0.25 for i in range(n)], type=pa.float64()),
        }
    )
    path = tmp_path / "source.parquet"
    pq.write_table(table, path)
    return path, table


def test_convert_roundtrips_the_data(source_parquet, tmp_path, capsys):
    path, table = source_parquet
    dest = tmp_path / "out.lance"
    assert main(["convert", str(path), str(dest)]) == 0

    assert pa.table(nanolance.read_table(dest)).to_pydict() == table.to_pydict()
    out = capsys.readouterr().out
    assert "20,000 rows" in out
    assert "parquet :" in out and "lance   :" in out


def test_converted_output_is_readable_by_stock_lance(source_parquet, tmp_path):
    """The conversion demo is the on-ramp; producing a dataset only nanolance can read would be
    worse than not having it. The `level` column here is the scattered low-cardinality shape that
    picks the structural-dictionary encoding, which is exactly the one that used to make the whole
    file unreadable by Lance."""
    lance = require_pylance()
    path, table = source_parquet
    dest = tmp_path / "interop.lance"
    assert main(["convert", str(path), str(dest), "--compress"]) == 0
    assert lance.dataset(str(dest)).to_table().to_pydict() == table.to_pydict()


def test_convert_parquet_with_lists_and_maps(tmp_path):
    """A parquet file with nested columns -- the common case for event data -- converts, and stock
    Lance reads the result. Parquet names a list's child `element`, not `item`; the name is carried
    through as given."""
    lance = require_pylance()
    n = 5_000
    table = pa.table(
        {
            "id": pa.array(range(n), type=pa.int64()),
            "tags": pa.array([None if i % 13 == 0 else [f"t{j}" for j in range(i % 4)] for i in range(n)]),
            "attrs": pa.array([[("k", i), ("j", None)][: i % 3] for i in range(n)], pa.map_(pa.utf8(), pa.int64())),
            "hits": pa.array([{"n": i, "at": [i, i + 1][: i % 3]} for i in range(n)]),
        }
    )
    path = tmp_path / "nested.parquet"
    pq.write_table(table, path)
    expected = pq.read_table(path)
    dest = tmp_path / "nested.lance"
    assert main(["convert", str(path), str(dest), "--compress"]) == 0
    assert pa.table(nanolance.read_table(dest)).to_pydict() == expected.to_pydict()
    assert lance.dataset(str(dest)).to_table().to_pydict() == expected.to_pydict()


def test_convert_projects_and_bounds_fragments(source_parquet, tmp_path):
    path, table = source_parquet
    dest = tmp_path / "subset.lance"
    assert (
        main(
            [
                "convert",
                str(path),
                str(dest),
                "--columns",
                "id",
                "level",
                "--rows-per-fragment",
                "5000",
                "--batch-size",
                "1000",
            ]
        )
        == 0
    )
    back = pa.table(nanolance.read_table(dest))
    assert back.column_names == ["id", "level"]
    assert back.to_pydict() == table.select(["id", "level"]).to_pydict()
    # 20k rows capped at 5k per fragment.
    assert len(list((dest / "data").glob("*.lance"))) == 4


def test_convert_refuses_to_clobber_without_overwrite(source_parquet, tmp_path):
    path, _ = source_parquet
    dest = tmp_path / "twice.lance"
    assert main(["convert", str(path), str(dest)]) == 0
    with pytest.raises(SystemExit, match="--overwrite"):
        main(["convert", str(path), str(dest)])
    # ...and does replace it when told to.
    assert main(["convert", str(path), str(dest), "--overwrite"]) == 0


@pytest.mark.parametrize(
    "argv, needle",
    [
        pytest.param(["convert", "missing.parquet", "out.lance"], "not found", id="missing_input"),
        pytest.param(["convert", "notes.txt", "out.lance"], "expected a parquet file", id="wrong_suffix"),
    ],
)
def test_convert_refuses_bad_input_by_name(argv, needle, tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    (tmp_path / "notes.txt").write_text("not parquet")
    with pytest.raises(SystemExit, match=needle):
        main(argv)


def test_inspect_reports_rows_size_and_schema(source_parquet, tmp_path, capsys):
    path, table = source_parquet
    dest = tmp_path / "inspect.lance"
    assert main(["convert", str(path), str(dest)]) == 0
    capsys.readouterr()

    assert main(["inspect", str(dest)]) == 0
    out = capsys.readouterr().out
    assert "20,000" in out
    for name in table.column_names:
        assert name in out


def test_module_is_runnable_as_python_dash_m(source_parquet, tmp_path):
    """`python -m nanolance` is the form the docs show, so check the entry point itself resolves."""
    import subprocess

    path, _ = source_parquet
    dest = tmp_path / "subproc.lance"
    env = dict(os.environ)
    result = subprocess.run(
        [sys.executable, "-m", "nanolance", "convert", str(path), str(dest)],
        capture_output=True,
        text=True,
        env=env,
    )
    assert result.returncode == 0, result.stderr
    assert dest.is_dir()
