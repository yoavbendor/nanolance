# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lancedb.pydantic``: ``LanceModel`` and the ``Vector`` / ``MultiVector`` field types."""

from __future__ import annotations

from typing import List

import pyarrow as pa
from pydantic import BaseModel

from nanolance.lance.pydantic import FixedSizeListMixin, MultiVector, Vector, pydantic_to_schema

__all__ = ["LanceModel", "Vector", "MultiVector", "FixedSizeListMixin", "pydantic_to_schema"]


class LanceModel(BaseModel):
    """A pydantic model whose fields are a table's columns (``create_table(schema=Model)``)."""

    @classmethod
    def to_arrow_schema(cls) -> pa.Schema:
        return pydantic_to_schema(cls)

    @classmethod
    def field_names(cls) -> List[str]:
        return list(cls.safe_get_fields().keys())

    @classmethod
    def safe_get_fields(cls):
        return cls.model_fields if hasattr(cls, "model_fields") else cls.__fields__

    @classmethod
    def parse_embedding_functions(cls) -> list:
        return []  # embedding functions are not supported: callers embed and pass vectors
