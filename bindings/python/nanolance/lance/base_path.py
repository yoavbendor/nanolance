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
    if base_id is None:
        return os.path.join(dataset_uri, "data")
    for base in base_paths:
        if base["id"] == base_id:
            return os.path.join(base["path"], "data") if base["is_dataset_root"] else base["path"]
    raise OSError(f"LanceError(IO): data file refers to unknown base path {base_id}")


def _new_base_dicts(initial_bases) -> list:
    """The base paths a dataset created with ``initial_bases`` registers, as the manifest's entries
    (ids count from 1 in the order given)."""
    return [{"id": i + 1, "name": base.name, "path": base.path, "is_dataset_root": base.is_dataset_root}
            for i, base in enumerate(initial_bases)]


def _base_id_of(reference, bases: list, dataset_uri: str) -> int:
    """The id of the base a ``target_bases`` entry names: by its name, or by its path. The dataset's own
    uri names the primary storage, id 0."""
    text = os.fspath(reference)
    if os.path.normpath(text) == os.path.normpath(dataset_uri):
        return 0
    for base in bases:
        if base["name"] == text or os.path.normpath(base["path"]) == os.path.normpath(text):
            return base["id"]
    raise OSError(f"Invalid user input: Base reference '{text}' not found in available bases")


def _fragment_targets(dataset_uri: str, bases: list, target_bases, target_all_bases,
                      include_primary: bool = True) -> list:
    """Where new fragments go, in turn, as (directory, base_id) pairs (base_id 0: the dataset itself):
    every base in turn after the dataset with ``target_all_bases`` (without the dataset when
    ``include_primary`` is false), the named ones in turn with ``target_bases``, else the dataset alone."""
    if target_all_bases:
        ids = ([0] if include_primary else []) + [base["id"] for base in bases]
    elif target_bases:
        ids = [_base_id_of(reference, bases, dataset_uri) for reference in target_bases]
    else:
        ids = [0]
    return [(_base_data_dir(dataset_uri, bases, base_id or None), base_id) for base_id in ids]


class _DatasetBases:
    """The part of pylance's inner dataset handle (``dataset._ds``) that base paths and the index cache live on."""

    def __init__(self, dataset):
        self._dataset = dataset

    def index_cache_hit_rate(self) -> float:
        """The share of this handle's index partition lookups that the index cache answered (0.0 before any)."""
        hits = getattr(self._dataset, "_index_cache_hits", 0)
        misses = getattr(self._dataset, "_index_cache_misses", 0)
        return hits / (hits + misses) if hits + misses else 0.0

    def base_paths(self) -> dict:
        """The dataset's base paths by id, as DatasetBasePath objects."""
        return {base["id"]: DatasetBasePath(base["path"], name=base["name"],
                                            is_dataset_root=base["is_dataset_root"], id=base["id"])
                for base in self._dataset._info.get("base_paths", [])}

    def take_scan(self, ranges, columns=None, batch_readahead=None):
        """pylance's ``take_scan``: one RecordBatch per ``(start, end)`` row range, in the order given.

        Rows are logical (deleted rows are skipped, as in ``take``). ``ranges`` may be a generator.
        """
        for start, end in ranges:
            table = self._dataset.to_table(columns=columns, offset=start, limit=end - start)
            yield from table.combine_chunks().to_batches()
