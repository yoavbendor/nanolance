# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.progress``: the write progress callback interface (write_dataset, LanceFragment.create and
write_fragments call it once per data file: begin, then complete), and the index progress events
(``IndexProgress``) index builds and merges report."""

from __future__ import annotations

import json
import os
from abc import ABC
from dataclasses import dataclass
from typing import TYPE_CHECKING, Dict, Literal, Optional

if TYPE_CHECKING:
    from nanolance.lance.fragment import FragmentMetadata


class FragmentWriteProgress(ABC):
    def begin(self, fragment, **kwargs) -> None:  # pragma: no cover - an interface
        pass

    def complete(self, fragment) -> None:  # pragma: no cover - an interface
        pass


class NoopFragmentWriteProgress(FragmentWriteProgress):
    pass


# The class below is ported from pylance's lance/progress.py, whose header is:
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright The Lance Authors
class FileSystemFragmentWriteProgress(FragmentWriteProgress):
    """Progress tracking for Writing a Dataset or Fragment.

    Warns
    -----
    This tracking class is experimental and will change in the future.

    This implementation writes a JSON file to track in-progress state
    to the filesystem for each fragment.


    """

    PROGRESS_EXT: str = ".in_progress"

    def __init__(self, base_uri: str, metadata: Optional[Dict[str, str]] = None):
        """Create a FileSystemFragmentWriteProgress tracker.

        Parameters
        ----------
        base_uri : str
            The base directory to write the progress files to. Two files will be created
            under this directory: a Fragment file, and a JSON file to track progress.
        metadata : dict, optional
            Extra metadata for this Progress tracker instance. Can be used to track
            distributed worker where this tracker is running.
        """
        from pyarrow.fs import FileSystem

        fs, path = FileSystem.from_uri(base_uri)
        self._fs = fs
        self._base_path: str = path
        self._metadata = metadata if metadata else {}

    def _in_progress_path(self, fragment: "FragmentMetadata") -> str:
        return os.path.join(
            self._base_path, f"fragment_{fragment.id}{self.PROGRESS_EXT}"
        )

    def _fragment_file(self, fragment: "FragmentMetadata") -> str:
        return os.path.join(self._base_path, f"fragment_{fragment.id}.json")

    def begin(self, fragment: "FragmentMetadata", **kwargs):
        """Called when a new fragment is created.

        Parameters
        ----------
        fragment : FragmentMetadata
            The fragment that is open to write to.
        """

        self._fs.create_dir(self._base_path, recursive=True)

        with self._fs.open_output_stream(self._in_progress_path(fragment)) as out:
            progress_data = {
                "fragment_id": fragment.id,
                "metadata": self._metadata,
            }
            out.write(json.dumps(progress_data).encode("utf-8"))

        with self._fs.open_output_stream(self._fragment_file(fragment)) as out:
            out.write(json.dumps(fragment.to_json()).encode("utf-8"))

    def complete(self, fragment: "FragmentMetadata", **kwargs):
        """Called when a fragment is completed"""
        self._fs.delete_file(self._in_progress_path(fragment))


@dataclass(frozen=True)
class IndexProgress:
    """A progress event while an index is built or merged: ``event`` "start", "progress" or
    "complete" of ``stage``, with ``completed`` of ``total`` ``unit`` when known."""

    event: Literal["start", "progress", "complete"]
    stage: str
    completed: Optional[int] = None
    total: Optional[int] = None
    unit: str = ""

    @property
    def fraction(self) -> Optional[float]:
        if self.completed is None or self.total in (None, 0):
            return None
        return min(self.completed / self.total, 1.0)


def __getattr__(name: str):
    """pylance names this module does not implement import as placeholders (see _stubs.py)."""
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    value = placeholder(f"lance.progress.{name}")
    globals()[name] = value
    return value
