# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Native errors, raised as the exception types pylance raises for the same failure."""

from __future__ import annotations

import contextlib
import os


class NotSupportedError(NotImplementedError):
    """A pylance feature nanolance does not implement (indexes, vector search, remote storage, ...).

    A subclass of ``NotImplementedError``: code that probes for a feature can catch that.
    """


def unsupported(what: str) -> NotSupportedError:
    return NotSupportedError(f"nanolance.lance does not support {what}")


def refuse_object_store_write(target, what: str) -> None:
    """Writes to an object store are not implemented (Wave 3): a write to ``s3://`` names itself and
    raises before anything is touched. ``target`` is a uri, a path or a dataset."""
    if isinstance(target, str):
        text = target
    elif isinstance(target, os.PathLike):
        text = os.fspath(target)
    else:
        text = str(getattr(target, "uri", "") or "")
    if text.startswith("s3://"):
        raise unsupported(f"object-store writes ({what} on {text})")


# Manifest feature flags this build does not support, as pylance raises them. The native message carries
# the prefix of the call that failed, so the message is cut down to pylance's text.
_FEATURE_FLAG_ERRORS = (
    ("Not supported: This dataset cannot be read by", ValueError),
    ("Not supported: This dataset cannot be written by", OSError),
    ("Encountered corrupt file manifest:", ValueError),
)


def translate(message: str) -> Exception:
    text = str(message)
    lower = text.lower()
    for marker, kind in _FEATURE_FLAG_ERRORS:
        if marker in text:
            return kind(text[text.index(marker):])
    # Filter errors, as pylance words them (DataFusion's planner behind "Invalid user input").
    import re

    missing = re.match(r"filter column '(.*)' not found in schema", text)
    if missing:
        return ValueError(f"Invalid user input: Schema error: No field named {missing.group(1)}.")
    if text.startswith("a filter must be a boolean expression"):
        return ValueError(f"Invalid user input: the filter does not return a boolean ({text})")
    if text.startswith(("invalid filter", "cannot compare", "cannot CAST")):
        return ValueError(f"Invalid user input: {text}")
    if "Legacy blob columns" in text:
        return OSError("Invalid user input: " + text[text.index("Legacy blob columns"):])
    for marker in ("Blob v2 field '", "Invalid blob threshold metadata"):
        if marker in text:
            return OSError("Invalid user input: " + text[text.index(marker):])
    if "is declared as non-nullable but contains null values" in text:
        return OSError("LanceError(Arrow): Invalid argument error: " + text[text.index("Column '"):])
    if "Empty structs with rep/def information" in text:
        return OSError("Invalid user input: " + text.split("write failed: ", 1)[-1])
    if "dimension must be a positive integer" in text:
        return OSError(text)
    if "already exists" in lower:
        return OSError(text)
    if text.startswith("Invalid user input: CompactionOptions::"):
        return OSError(text)
    if "append" in lower and "schema" in lower:
        return OSError(text)
    if "merge insert failed" in lower:
        return OSError(text)
    if "not found" in lower or "no manifest" in lower:
        return ValueError(text)
    if "not supported" in lower or "unsupported" in lower:
        return NotSupportedError(text)
    if "failed to open" in lower or "failed to read" in lower or "i/o" in lower:
        return OSError(text)
    return ValueError(text)


def index_build(fn):
    """pylance raises NotImplementedError, not OSError, when an index build meets a writer feature flag
    it does not support (create_scalar_index and create_index; measured on pylance 12.0.0)."""
    import functools

    @functools.wraps(fn)
    def wrapper(*args, **kwargs):
        try:
            return fn(*args, **kwargs)
        except OSError as exc:
            if str(exc).startswith("Not supported: This dataset cannot be written by"):
                raise NotImplementedError(str(exc)) from None
            raise

    return wrapper


@contextlib.contextmanager
def native():
    """Re-raise a native RuntimeError as the type pylance would raise."""
    try:
        yield
    except RuntimeError as exc:
        raise translate(str(exc)) from None
