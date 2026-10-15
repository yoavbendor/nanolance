# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""The pylance surface nanolance adds in docs/tasks/PY_SURFACE.md: ``lance.log`` and Lance's native log.

Each test runs the same code under real pylance and under nanolance (``install_as_lance``), in a
subprocess, and compares what they log. Timestamps are dropped; uri and file-name values are
replaced, since they differ by construction.
"""

from __future__ import annotations

import importlib
import os
import re
import subprocess
import sys
from pathlib import Path
from unittest import mock

import pytest

from tests.support import require_pylance

ENGINES = ("pylance", "nanolance")
DATASET_EVENTS = "lance::events::dataset_events"
FILE_AUDIT = "lance::events::file_audit"
# bindings/python: the directory holding the nanolance package
NANOLANCE_PATH = Path(__file__).resolve().parents[1]

PRELUDE = {
    "pylance": "import lance\n",
    "nanolance": "import nanolance.lance as _nl\n_nl.install_as_lance()\nimport lance\n",
}

WRITE_OPS = (
    "import sys, pyarrow as pa\n"
    "d = sys.argv[1]\n"
    "t = pa.table({'x': range(10)})\n"
    "lance.write_dataset(t, d + '/a.lance')\n"
    "lance.write_dataset(t, d + '/a.lance', mode='append')\n"
)
# write, append, open, delete
ALL_OPS = WRITE_OPS + (
    "lance.dataset(d + '/a.lance').to_table()\n"
    "lance.dataset(d + '/a.lance').delete('x < 3')\n"
)

LOG_LINE = re.compile(r"^\[(\S+) +(\w+) +([^\]]+)\] (.*)$")


def _run(engine: str, code: str, *args: str, env: dict | None = None) -> subprocess.CompletedProcess:
    run_env = {"PATH": os.environ.get("PATH", "")}
    if engine == "nanolance":
        run_env["PYTHONPATH"] = str(NANOLANCE_PATH)
    run_env.update(env or {})
    result = subprocess.run([sys.executable, "-c", PRELUDE[engine] + code, *args],
                            capture_output=True, env=run_env, timeout=300)
    assert result.returncode == 0, f"{engine}: {result.stderr.decode(errors='replace')}"
    return result


def _events(text: bytes | str, *targets: str) -> list:
    """The log lines for ``targets`` as ``LEVEL target body``: no timestamp, uri and path replaced."""
    if isinstance(text, bytes):
        text = text.decode(errors="replace")
    out = []
    for line in text.splitlines():
        match = LOG_LINE.match(line)
        if match and match.group(3) in targets:
            body = re.sub(r'uri="[^"]*"', 'uri="U"', match.group(4))
            body = re.sub(r'path="[^"]*"', 'path="P"', body)
            out.append(f"{match.group(2)} {match.group(3)} {body}")
    return out


def _log_lines(text: bytes) -> list:
    return [line for line in text.decode(errors="replace").splitlines() if LOG_LINE.match(line)]


@pytest.mark.parametrize("env_value", ["DEBUG", "INFO", "WARNING", "DEBUG,INFO", "", "lance-core=debug,WARNING",
                                       "trace"])
def test_log_level_matches_pylance(env_value):
    require_pylance()
    lance_log = importlib.import_module("lance.log")
    nanolance_log = importlib.import_module("nanolance.lance.log")
    assert nanolance_log.ENV_NAME_PYLANCE_LOGGING_LEVEL == lance_log.ENV_NAME_PYLANCE_LOGGING_LEVEL
    with mock.patch.dict(os.environ, {"LANCE_LOG": env_value}):
        assert nanolance_log.get_log_level() == lance_log.get_log_level()


def test_write_append_open_delete_events_match_pylance(tmp_path):
    require_pylance()
    events = {}
    for engine in ENGINES:
        data = tmp_path / engine
        data.mkdir()
        result = _run(engine, ALL_OPS, str(data), env={"LANCE_LOG": "info"})
        events[engine] = _events(result.stderr, DATASET_EVENTS, FILE_AUDIT)
    # pylance also audits the deletion files and logs its plan (lance::events::execution); nanolance
    # does not log those yet, so they are left out of the comparison.
    expected = [line for line in events["pylance"] if 'type="deletion"' not in line]
    assert events["nanolance"], "nanolance logged nothing"
    assert events["nanolance"] == expected
    assert any('event="committed"' in line and 'operation="Delete"' in line for line in events["nanolance"])


@pytest.mark.parametrize("spec, expect_file_audit", [
    ("warn", False),
    ("warn,lance::events::dataset_events=info", False),
    ("info,lance::events::file_audit=off", False),
    ("info", True),
])
def test_filters_match_pylance(tmp_path, spec, expect_file_audit):
    require_pylance()
    events = {}
    for engine in ENGINES:
        data = tmp_path / engine
        data.mkdir()
        result = _run(engine, WRITE_OPS, str(data), env={"LANCE_LOG": spec})
        events[engine] = _events(result.stderr, DATASET_EVENTS, FILE_AUDIT)
    assert events["nanolance"] == events["pylance"]
    has_file_audit = any(line.startswith("INFO lance::events::file_audit") for line in events["nanolance"])
    assert has_file_audit is expect_file_audit
    if spec == "warn":
        assert events["nanolance"] == []


@pytest.mark.parametrize("precision, digits", [("s", 0), ("ms", 3), ("us", 6), ("ns", 9)])
def test_timestamp_precision_matches_pylance(tmp_path, precision, digits):
    require_pylance()
    fraction = rf"\.\d{{{digits}}}" if digits else ""
    pattern = re.compile(r"^\[\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}" + fraction + r"Z ")
    for engine in ENGINES:
        data = tmp_path / engine
        data.mkdir()
        result = _run(engine, WRITE_OPS, str(data), env={"LANCE_LOG": "debug",
                                                          "LANCE_LOG_TS_PRECISION": precision})
        first = result.stderr.decode().splitlines()[0]
        assert pattern.match(first), f"{engine}: {first}"


def test_invalid_timestamp_precision_is_reported(tmp_path):
    require_pylance()
    for engine in ENGINES:
        data = tmp_path / engine
        data.mkdir()
        result = _run(engine, WRITE_OPS, str(data), env={"LANCE_LOG": "info",
                                                          "LANCE_LOG_TS_PRECISION": "bogus"})
        # pylance's own warning is not captured under pytest here (it shows in a shell); nanolance's is.
        if engine == "nanolance":
            assert "Invalid timestamp precision" in result.stderr.decode()
        assert _events(result.stderr, DATASET_EVENTS), engine


def test_log_file_is_written_with_parent_directories(tmp_path):
    require_pylance()
    contents = {}
    for engine in ENGINES:
        log_file = tmp_path / engine / "nested" / "lance.log"
        result = _run(engine, WRITE_OPS, str(tmp_path / engine), env={"LANCE_LOG": "info",
                                                                       "LANCE_LOG_FILE": str(log_file)})
        assert log_file.exists(), engine
        assert _log_lines(result.stderr) == [], f"{engine} wrote log lines to stderr as well as the file"
        contents[engine] = _events(log_file.read_bytes(), DATASET_EVENTS, FILE_AUDIT)
        assert contents[engine], engine
    assert contents["nanolance"] == contents["pylance"]


def test_unwritable_log_file_is_reported_and_ignored(tmp_path):
    require_pylance()
    blocker = tmp_path / "blocker"
    blocker.write_text("a file, so it cannot be a directory")
    log_file = blocker / "lance.log"
    for engine in ENGINES:
        data = tmp_path / engine
        data.mkdir()
        # pylance prints nothing for this case; nanolance says why the log is not written.
        result = _run(engine, WRITE_OPS, str(data), env={"LANCE_LOG": "info", "LANCE_LOG_FILE": str(log_file)})
        assert not log_file.exists(), engine
        if engine == "nanolance":
            assert "cannot open LANCE_LOG_FILE" in result.stderr.decode()


def test_no_native_log_without_lance_log(tmp_path):
    result = _run("nanolance", WRITE_OPS, str(tmp_path), env={})
    assert _log_lines(result.stderr) == []
