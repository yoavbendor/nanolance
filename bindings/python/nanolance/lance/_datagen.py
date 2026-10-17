# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance._datagen``: pylance's random-data generator for its own tests and benchmarks. Its wheels
are built without the datagen feature, and so is nanolance: the same answers as such a build."""

from __future__ import annotations

from typing import Optional

import pyarrow as pa


def is_datagen_supported() -> bool:
    return False


def rand_batches(
    schema: pa.Schema,
    *,
    num_batches: Optional[int] = None,
    batch_size_bytes: Optional[int] = None,
    rows_per_batch: Optional[int] = None,
):
    raise NotImplementedError("This version of lance was not built with the datagen feature")
