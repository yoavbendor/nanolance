"""Shared pytest fixtures for nanolance Python bindings."""

from __future__ import annotations

import pyarrow as pa
import pytest


@pytest.fixture
def sample_table() -> pa.Table:
    return pa.table(
        {
            "i32": pa.array([-2000, -1000, 0, 1000, 2000], type=pa.int32()),
            "i64": pa.array([0, 1_000_000, 2_000_000, 3_000_000, 4_000_000], type=pa.int64()),
            "u32": pa.array([4_000_000_000 + i for i in range(5)], type=pa.uint32()),
            "f32": pa.array([0.5, 1.5, 2.5, 3.5, 4.5], type=pa.float32()),
            "f64": pa.array([0.0, 1.25, 2.5, 3.75, 5.0], type=pa.float64()),
            "flag": [True, False, True, False, True],
            "name": ["alpha", "beta", "alpha", "gamma", "beta"],
            "fsb": [bytes([i, i + 1, i + 2, i + 3]) for i in range(5)],
        }
    )


@pytest.fixture
def pylance_interop_table() -> pa.Table:
    """Small table known to round-trip through both nanolance and pylance."""
    return pa.table({"id": [1, 2, 3], "name": ["alpha", "beta", "gamma"]})


@pytest.fixture
def nullable_table() -> pa.Table:
    return pa.table(
        {
            "a": pa.array([0, None, 200, None, 400, 500], type=pa.int32()),
            "s": pa.array([None, "alpha", "beta", "alpha", None, "beta"], type=pa.string()),
            "f": pa.array([False, True, None, True, False, True], type=pa.bool_()),
            "nul": pa.nulls(6),
            "ctrl": pa.array([0, 1, 2, 3, 4, 5], type=pa.int32()),
        }
    )
