# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""A dataset's base path: a directory outside the dataset where its data files can live."""

from __future__ import annotations

import os
from typing import Optional


class DatasetBasePath:
    """pylance's ``DatasetBasePath``: ``path`` names the directory, ``is_dataset_root`` says whether it
    is a dataset of its own (its files are under ``<path>/data``) or holds the files directly."""

    __slots__ = ("id", "is_dataset_root", "name", "path")

    def __init__(self, path: str, name: Optional[str] = None, is_dataset_root: bool = False,
                 id: Optional[int] = None):
        self.path = os.fspath(path)
        self.name = name
        self.is_dataset_root = bool(is_dataset_root)
        self.id = 0 if id is None else int(id)

    def __repr__(self) -> str:
        name = "None" if self.name is None else f'Some("{self.name}")'
        root = "true" if self.is_dataset_root else "false"
        return f"DatasetBasePath(id={self.id}, name={name}, path={self.path}, is_dataset_root={root})"


def _base_data_dir(dataset_uri: str, base_paths: list, base_id: Optional[int]) -> str:
    """The directory of a data file in base ``base_id`` (the manifest's ``base_paths`` entries): the
    dataset's own ``data/`` when there is no base, else the base's path, or its ``data/`` when it is a
    dataset root. Lance's rule, as the reader applies it."""
    if not base_id:
        return os.path.join(dataset_uri, "data")
    for base in base_paths:
        if base["id"] == base_id:
            return os.path.join(base["path"], "data") if base["is_dataset_root"] else base["path"]
    raise OSError(f"LanceError(IO): data file refers to unknown base path {base_id}")
