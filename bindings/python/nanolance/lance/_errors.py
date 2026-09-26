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
    if "already exists" in lower:
        return OSError(text)
    if "append" in lower and "schema" in lower:
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
