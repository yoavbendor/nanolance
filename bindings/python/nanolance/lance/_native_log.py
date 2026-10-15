# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Lance's native log, for the events nanolance itself raises (opens, writes, commits).

Switched on by ``LANCE_LOG``, a filter list of ``level`` (the default for every target) and
``target=level`` entries, comma separated, e.g. ``warn,lance::events::dataset_events=info``. Lines are
env_logger's::

    [2026-10-15T18:43:41Z INFO  lance::events::dataset_events] target="lance::dataset_events" event=...

They go to stderr, or to the file named by ``LANCE_LOG_FILE`` (its parent directories are created; a
path that cannot be opened gets one message on stderr and the operation goes on without a log).
``LANCE_LOG_TS_PRECISION`` picks the timestamp: ``s`` (default), ``ms``, ``us`` or ``ns``.

Targets and fields are the ones pylance's Rust side logs: ``lance::events::dataset_events`` with
loading / writing / committed, and ``lance::events::file_audit`` with data, manifest and deletion files.
Nothing is formatted unless the filter enables the line.
"""

from __future__ import annotations

import os
import sys
import threading
import time
from contextlib import contextmanager
from typing import Optional, Tuple

DATASET_EVENTS = "lance::events::dataset_events"
FILE_AUDIT = "lance::events::file_audit"

_LEVELS = {"off": 0, "error": 1, "warn": 2, "info": 3, "debug": 4, "trace": 5}
_LABELS = {1: "ERROR", 2: "WARN", 3: "INFO", 4: "DEBUG", 5: "TRACE"}
_DIGITS = {"s": 0, "ms": 3, "us": 6, "ns": 9}

_lock = threading.Lock()
_tls = threading.local()
_state = {"spec": None, "default": 0, "targets": (), "precision": "s", "file_path": None, "file": None,
          "failed": set(), "warned": set()}


def _parse(spec: str) -> Tuple[int, Tuple[Tuple[str, int], ...]]:
    default = 0
    targets = []
    for raw in spec.split(","):
        entry = raw.strip()
        if not entry:
            continue
        if "=" in entry:
            target, _, level = entry.rpartition("=")
            if level.strip().lower() in _LEVELS:
                targets.append((target.strip(), _LEVELS[level.strip().lower()]))
        elif entry.lower() in _LEVELS:
            default = _LEVELS[entry.lower()]
        else:
            # env_logger reads a bare name that is not a level as a target, at every level.
            targets.append((entry, _LEVELS["trace"]))
    targets.sort(key=lambda item: -len(item[0]))
    return default, tuple(targets)


def _refresh_config() -> None:
    """Re-read the environment (cheap) and re-parse the filter when it changed."""
    spec = os.environ.get("LANCE_LOG", "")
    precision = os.environ.get("LANCE_LOG_TS_PRECISION", "s").strip().lower() or "s"
    if (spec, precision) == _state["spec"]:
        return
    default, targets = _parse(spec)
    _state["default"], _state["targets"] = default, targets
    if precision not in _DIGITS:
        if spec.strip():
            _warn_once("precision", "Invalid timestamp precision (valid values: ns, us, ms, s): "
                                    f"{precision}, using default")
        precision = "s"
    _state["precision"] = precision
    _state["spec"] = (spec, os.environ.get("LANCE_LOG_TS_PRECISION", "s"))


def _warn_once(key: str, message: str) -> None:
    if key not in _state["warned"]:
        _state["warned"].add(key)
        sys.stderr.write(message + "\n")
        sys.stderr.flush()


def _limit_for(target: str) -> int:
    for name, level in _state["targets"]:
        if target == name or target.startswith(name + "::"):
            return level
    return _state["default"]


def enabled(target: str, level: str = "info") -> bool:
    """Whether a line at ``level`` for ``target`` would be written now."""
    if getattr(_tls, "depth", 0):
        return False
    _refresh_config()
    limit = _limit_for(target)
    return limit > 0 and _LEVELS[level] <= limit


def _timestamp() -> str:
    ns = time.time_ns()
    seconds, fraction = divmod(ns, 1_000_000_000)
    text = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(seconds))
    digits = _DIGITS[_state["precision"]]
    if digits:
        text += "." + f"{fraction:09d}"[:digits]
    return text + "Z"


def _sink():
    """stderr, or the LANCE_LOG_FILE file (opened once, in append mode). None when it cannot be opened."""
    path = os.environ.get("LANCE_LOG_FILE", "")
    if not path:
        return sys.stderr
    if path in _state["failed"]:
        return None
    if _state["file_path"] != path:
        old = _state["file"]
        if old is not None:
            old.close()
        _state["file"], _state["file_path"] = None, None
        try:
            parent = os.path.dirname(os.path.abspath(path))
            os.makedirs(parent, exist_ok=True)
            _state["file"] = open(path, "a", encoding="utf-8")
        except OSError as exc:
            _state["failed"].add(path)
            sys.stderr.write(f"lance: cannot open LANCE_LOG_FILE {path!r} ({exc.strerror or exc}); "
                             "the log is not written\n")
            sys.stderr.flush()
            return None
        _state["file_path"] = path
    return _state["file"]


def emit(target: str, level: str, body: str) -> None:
    """Write one line (no-op unless the filter enables it)."""
    if not enabled(target, level):
        return
    line = f"[{_timestamp()} {_LABELS[_LEVELS[level]]:<5} {target}] {body}"
    with _lock:
        out = _sink()
        if out is None:
            return
        out.write(line + "\n")
        out.flush()


def _q(text) -> str:
    return '"' + str(text).replace("\\", "\\\\").replace('"', '\\"') + '"'


def _fields(*pairs: Tuple[str, str]) -> str:
    return "".join(f"{key}={value} " for key, value in pairs)


def loading(uri: str, *, status: str, version: Optional[int] = None) -> None:
    """A dataset was opened (``status="success"``, with its version) or could not be (``"error"``)."""
    if not enabled(DATASET_EVENTS):
        return
    pairs = [("target", _q("lance::dataset_events")), ("event", _q("loading")), ("uri", _q(uri)),
             ("target_ref", "None")]
    if version is not None:
        pairs.append(("version", str(int(version))))
    pairs.append(("status", _q(status)))
    emit(DATASET_EVENTS, "info", _fields(*pairs))


def writing(uri: str, mode: str) -> None:
    """A write starts; ``mode`` is Create, Append or Overwrite."""
    if enabled(DATASET_EVENTS):
        emit(DATASET_EVENTS, "info", _fields(("target", _q("lance::dataset_events")), ("event", _q("writing")),
                                             ("uri", _q(uri)), ("mode", mode)))


def committed(uri: str, *, read_version: int, committed_version: int, operation: str,
              detached: bool = False) -> None:
    """A new version was committed; ``operation`` is the transaction's name (Append, Delete, ...)."""
    if enabled(DATASET_EVENTS):
        emit(DATASET_EVENTS, "info", _fields(
            ("target", _q("lance::dataset_events")), ("event", _q("committed")), ("uri", _q(uri)),
            ("read_version", str(int(read_version))), ("committed_version", str(int(committed_version))),
            ("detached", "true" if detached else "false"), ("operation", _q(operation))))


def file_audit(mode: str, kind: str, path: str) -> None:
    """A file of the dataset was created: ``kind`` is data, manifest or deletion."""
    if enabled(FILE_AUDIT):
        emit(FILE_AUDIT, "info", _fields(("target", _q("lance::file_audit")), ("mode", _q(mode)),
                                         ("type", _q(kind)), ("path", _q(path))))


def debug(target: str, message: str) -> None:
    """nanolance's own DEBUG line, under its own target (``nanolance::...``)."""
    emit(target, "debug", message)


def manifest_name(dataset_path: str, version: int) -> str:
    """The manifest file of ``version``: the V2 name (reversed version, 20 digits) when the dataset
    uses them, else the V1 name."""
    versions = os.path.join(dataset_path, "_versions")
    v2 = f"{(1 << 64) - 1 - int(version):020d}.manifest"
    if os.path.exists(os.path.join(versions, v2)):
        return v2
    return f"{int(version)}.manifest"


def data_files(dataset_path: str) -> set:
    """The names of the data files in a dataset (empty for none)."""
    data = os.path.join(dataset_path, "data")
    return set(os.listdir(data)) if os.path.isdir(data) else set()


def new_versions(uri: str, dataset_path: str, before: int, after: int, operation: str,
                 read_version: Optional[int] = None) -> None:
    """Versions ``before+1 .. after`` were committed: a manifest audit line and a committed line. The
    transaction's read version defaults to ``before``."""
    if after <= before or not enabled(DATASET_EVENTS):
        return
    file_audit("create", "manifest", manifest_name(dataset_path, after))
    committed(uri, read_version=before if read_version is None else read_version,
              committed_version=after, operation=operation)


@contextmanager
def quiet():
    """Log nothing inside: for opens nanolance makes for itself, not for the caller."""
    _tls.depth = getattr(_tls, "depth", 0) + 1
    try:
        yield
    finally:
        _tls.depth -= 1

