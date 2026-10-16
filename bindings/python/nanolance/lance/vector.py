# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.vector``: building tables of vectors. ``vec_to_table`` follows pylance's (Apache-2.0,
the Lance authors) argument for argument; the accelerator (torch) training helpers are pylance's alone."""

from __future__ import annotations

from typing import Optional, Union

import numpy as np
import pyarrow as pa

__all__ = ["vec_to_table"]


def _normalize_vectors(vectors, ndim):
    if ndim is None:
        ndim = len(next(iter(vectors)))
    values = np.array(vectors, dtype="float32").ravel()
    return pa.FixedSizeListArray.from_arrays(values, list_size=ndim)


def _validate_ndim(values, ndim):
    for v in values:
        if ndim is None:
            ndim = len(v)
        elif ndim != len(v):
            raise ValueError(f"Expected {ndim} dimensions but got {len(v)} for {v}")
    return ndim


def vec_to_table(data: Union[dict, list, np.ndarray], names: Optional[Union[str, list]] = None,
                 ndim: Optional[int] = None, check_ndim: bool = True) -> pa.Table:
    """A table of float32 fixed-size-list vectors: a dict's keys become an ``id`` column; a list's
    or an ndarray's rows are the vectors. Mirrors ``lance.vector.vec_to_table``."""
    if isinstance(data, dict):
        if names is None:
            names = ["id", "vector"]
        elif not isinstance(names, (list, tuple)) and len(names) == 2:
            raise ValueError("If data is a dict, names must be a list or tuple of 2 strings")
        values = list(data.values())
        if check_ndim:
            ndim = _validate_ndim(values, ndim)
        arrays = [pa.array(data.keys()), _normalize_vectors(values, ndim)]
    elif isinstance(data, (list, np.ndarray)):
        if names is None:
            names = ["vector"]
        elif isinstance(names, str):
            names = [names]
        elif not isinstance(names, (list, tuple)) and len(names) == 1:
            raise ValueError(f"names cannot be more than 1 got {len(names)}")
        if check_ndim:
            ndim = _validate_ndim(data, ndim)
        arrays = [_normalize_vectors(data, ndim)]
    else:
        raise NotImplementedError(f"data must be dict, list, or ndarray, got {type(data)} instead")
    return pa.Table.from_arrays(arrays, names=names)


def __getattr__(name: str):
    if name.startswith("__"):
        raise AttributeError(name)
    from nanolance.lance._stubs import placeholder

    return placeholder(f"lance.vector.{name}")
