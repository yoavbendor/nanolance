# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Native errors, raised as the exception types pylance raises for the same failure."""

from __future__ import annotations

import contextlib


class NotSupportedError(NotImplementedError):
    """A pylance feature nanolance does not implement (indexes, vector search, remote storage, ...).

    A subclass of ``NotImplementedError``: code that probes for a feature can catch that.
    """


def unsupported(what: str) -> NotSupportedError:
    return NotSupportedError(f"nanolance.lance does not support {what}")


def translate(message: str) -> Exception:
    text = str(message)
    lower = text.lower()
    # Filter errors, as pylance words them (DataFusion's planner behind "Invalid user input").
    import re

    missing = re.match(r"filter column '(.*)' not found in schema", text)
    if missing:
        return ValueError(f"Invalid user input: Schema error: No field named {missing.group(1)}.")
    if text.startswith("a filter must be a boolean expression"):
        return ValueError(f"Invalid user input: the filter does not return a boolean ({text})")
    if text.startswith(("invalid filter", "cannot compare", "cannot CAST")):
        return ValueError(f"Invalid user input: {text}")
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


@contextlib.contextmanager
def native():
    """Re-raise a native RuntimeError as the type pylance would raise."""
    try:
        yield
    except RuntimeError as exc:
        raise translate(str(exc)) from None
