# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.util``: the helpers of pylance's utility module that nanolance implements.

Other names raise ``NotSupportedError`` when used, as the placeholders of ``_stubs`` do.
"""

from __future__ import annotations

from typing import Optional

import numpy as np

from nanolance.lance._errors import unsupported


def _target_partition_size_to_num_partitions(num_rows: int, target_partition_size: Optional[int]) -> int:
    """How many IVF partitions a dataset of ``num_rows`` gets for a target rows-per-partition (8192 by default)."""
    if target_partition_size is None:
        target_partition_size = 8192
    num_partitions = num_rows // target_partition_size
    return min(max(1, num_partitions), 4096)


def validate_vector_index(dataset, column: str, refine_factor: int = 5, sample_size: Optional[int] = None,
                          pass_threshold: float = 1.0) -> None:
    """Run in-sample queries and make sure that the recall for k=1 is very high (near 100%).

    ``sample_size`` rows are checked (all of them by default); a ValueError is raised when fewer than
    ``pass_threshold`` of them find themselves.
    """
    data = dataset.to_table() if sample_size is None else dataset.sample(sample_size)
    vecs = data[column].to_numpy(zero_copy_only=False)
    passes = 0
    total = len(vecs)
    for vec in vecs:
        if np.isnan(vec).any():
            total -= 1
            continue
        distance = dataset.to_table(
            nearest={"column": column, "q": vec, "k": 1, "nprobes": 1, "refine_factor": refine_factor}
        )["_distance"].to_pylist()[0]
        passes += 1 if abs(distance) < 1e-6 else 0
    if passes / total < pass_threshold:
        raise ValueError(f"Vector index failed sanity check, only {passes}/{total} passed")


def __getattr__(name: str):
    if name.startswith("__"):
        raise AttributeError(name)
    raise unsupported(f"lance.util.{name}")
