# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor

"""Tests for tools/compare_bench.py: a file with no change passes, a slowdown past the threshold fails, and a case in
only one file is reported."""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import compare_bench  # noqa: E402


def write_results(directory, name, medians):
    """A result file with one case per {name: median_ns} entry, in the layout the benchmarks write."""
    path = directory / name
    cases = [{"name": case, "rows": 100, "bytes": None, "median_ns": ns} for case, ns in medians.items()]
    path.write_text(json.dumps({"header": {"program": "test"}, "results": cases}), encoding="utf-8")
    return str(path)


def test_no_change_passes(tmp_path, capsys):
    old = write_results(tmp_path, "old.json", {"decode/a": 1000, "decode/b": 2500})
    new = write_results(tmp_path, "new.json", {"decode/a": 1000, "decode/b": 2500})
    assert compare_bench.main([old, new]) == 0
    assert "no case is slower" in capsys.readouterr().out


def test_thirty_percent_slowdown_fails(tmp_path, capsys):
    old = write_results(tmp_path, "old.json", {"decode/a": 1000, "decode/b": 2500})
    new = write_results(tmp_path, "new.json", {"decode/a": 1300, "decode/b": 2500})
    assert compare_bench.main([old, new]) == 1
    out = capsys.readouterr().out
    assert "decode/a" in out
    assert "+30.0%" in out
    assert "SLOWER" in out


def test_threshold_moves_the_line(tmp_path):
    old = write_results(tmp_path, "old.json", {"decode/a": 1000})
    new = write_results(tmp_path, "new.json", {"decode/a": 1300})
    assert compare_bench.main([old, new, "--threshold", "50"]) == 0
    assert compare_bench.main([old, new, "--threshold", "10"]) == 1


def test_speedup_passes(tmp_path):
    old = write_results(tmp_path, "old.json", {"decode/a": 1000})
    new = write_results(tmp_path, "new.json", {"decode/a": 400})
    assert compare_bench.main([old, new]) == 0


def test_missing_benchmark_is_reported(tmp_path, capsys):
    old = write_results(tmp_path, "old.json", {"decode/a": 1000, "decode/gone": 500})
    new = write_results(tmp_path, "new.json", {"decode/a": 1000, "decode/added": 700})
    assert compare_bench.main([old, new]) == 0
    out = capsys.readouterr().out
    assert "decode/gone" in out and "missing in new" in out
    assert "decode/added" in out and "missing in old" in out
    assert "missing from one file" in out


def test_unreadable_file_exits_2(tmp_path, capsys):
    old = write_results(tmp_path, "old.json", {"decode/a": 1000})
    assert compare_bench.main([old, str(tmp_path / "absent.json")]) == 2
    assert "compare_bench" in capsys.readouterr().err
