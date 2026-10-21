# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Full-text queries. Mirrors ``lance.query`` (pylance 12): the same classes and arguments, each
describing itself as Lance's query JSON (``{"match": {...}}``, ...) for nanolance's search.
"""

from __future__ import annotations

import abc
from enum import Enum
from typing import Optional


class FullTextQueryType(Enum):
    MATCH = "match"
    MATCH_PHRASE = "match_phrase"
    BOOST = "boost"
    MULTI_MATCH = "multi_match"
    BOOLEAN = "boolean"


class FullTextOperator(Enum):
    AND = "AND"
    OR = "OR"


class DocumentGranularity(str, Enum):
    """The unit treated as one full-text-search document."""

    ROW = "row"
    LIST_ELEMENT = "list_element"


class Occur(Enum):
    SHOULD = "SHOULD"
    MUST = "MUST"
    MUST_NOT = "MUST_NOT"


class PyFullTextQuery:
    """A query's description (pylance's Rust-side query object)."""

    def __init__(self, spec: dict):
        self.spec = spec

    def __repr__(self) -> str:
        return f"PyFullTextQuery({self.spec!r})"


def _granularity(document_granularity) -> Optional[str]:
    """The query's document granularity as the native query JSON names it (None: the index decides)."""
    if document_granularity is None:
        return None
    return DocumentGranularity(document_granularity).value


_U32_MAX = 2**32 - 1
_U64_MAX = 2**64 - 1


def _unsigned(name: str, value, limit: int, negative_message: str) -> int:
    """An argument pylance takes as an unsigned integer, with pylance's exception and message."""
    import operator as _op

    try:
        number = _op.index(value)
    except TypeError as exc:
        raise TypeError(f"argument '{name}': {exc}") from None
    if number < 0:
        raise OverflowError(negative_message)
    if number > limit:
        raise OverflowError("out of range integral type conversion attempted")
    return number


def _operator(operator) -> str:
    value = operator.value if isinstance(operator, FullTextOperator) else str(operator)
    if value.upper() not in ("AND", "OR"):
        raise ValueError(f"Invalid operator: {value}")
    return "And" if value.upper() == "AND" else "Or"


class FullTextQuery(abc.ABC):
    _inner: PyFullTextQuery

    @property
    def inner(self) -> PyFullTextQuery:
        """The query's description."""
        return self._inner

    @abc.abstractmethod
    def query_type(self) -> FullTextQueryType:
        """The type of the query."""

    def __and__(self, other: "FullTextQuery") -> "FullTextQuery":
        return BooleanQuery([(Occur.MUST, self), (Occur.MUST, other)])

    def __or__(self, other: "FullTextQuery") -> "FullTextQuery":
        return BooleanQuery([(Occur.SHOULD, self), (Occur.SHOULD, other)])


class MatchQuery(FullTextQuery):
    def __init__(self, query: str, column: str, *, boost: float = 1.0, fuzziness: Optional[int] = 0,
                 max_expansions: int = 50, operator: FullTextOperator = FullTextOperator.OR,
                 prefix_length: int = 0, document_granularity: Optional[DocumentGranularity] = None):
        granularity = _granularity(document_granularity)
        # Checked as pylance checks them: None means exact matching for fuzziness, and nothing else
        # is accepted but an integer in the range of its Rust type.
        edits = 0 if fuzziness is None else _unsigned("fuzziness", fuzziness, _U32_MAX,
                                                       "out of range integral type conversion attempted")
        expansions = _unsigned("max_expansions", max_expansions, _U64_MAX, "can't convert negative int to unsigned")
        prefix = _unsigned("prefix_length", prefix_length, _U32_MAX,
                           "out of range integral type conversion attempted")
        spec = {
            "column": column, "terms": query, "boost": float(boost), "fuzziness": edits,
            "max_expansions": expansions, "operator": _operator(operator), "prefix_length": prefix}
        if granularity is not None:
            spec["granularity"] = granularity
        self._inner = PyFullTextQuery({"match": spec})

    def query_type(self) -> FullTextQueryType:
        return FullTextQueryType.MATCH


class PhraseQuery(FullTextQuery):
    def __init__(self, query: str, column: str, *, slop: int = 0,
                 document_granularity: Optional[DocumentGranularity] = None):
        spec = {"column": column, "terms": query, "slop": int(slop)}
        granularity = _granularity(document_granularity)
        if granularity is not None:
            spec["granularity"] = granularity
        self._inner = PyFullTextQuery({"match_phrase": spec})

    def query_type(self) -> FullTextQueryType:
        return FullTextQueryType.MATCH_PHRASE


class BoostQuery(FullTextQuery):
    def __init__(self, positive: FullTextQuery, negative: FullTextQuery, *, negative_boost: float = 0.5):
        self._inner = PyFullTextQuery({"boost": {"positive": positive.inner.spec, "negative": negative.inner.spec,
                                                 "negative_boost": float(negative_boost)}})

    def query_type(self) -> FullTextQueryType:
        return FullTextQueryType.BOOST


class MultiMatchQuery(FullTextQuery):
    def __init__(self, query: str, columns: list[str], *, boosts: Optional[list[float]] = None,
                 operator: FullTextOperator = FullTextOperator.OR):
        if boosts is not None and len(boosts) != len(columns):
            raise ValueError("The number of boosts must match the number of columns")
        self._inner = PyFullTextQuery({"multi_match": {"match_queries": [
            {"column": c, "terms": query, "boost": 1.0 if boosts is None else float(boosts[i]), "fuzziness": 0,
             "max_expansions": 50, "operator": _operator(operator), "prefix_length": 0}
            for i, c in enumerate(columns)]}})

    def query_type(self) -> FullTextQueryType:
        return FullTextQueryType.MULTI_MATCH


class BooleanQuery(FullTextQuery):
    def __init__(self, queries: list[tuple[Occur, FullTextQuery]]):
        spec = {"must": [], "should": [], "must_not": []}
        for occur, query in queries:
            spec[Occur(occur).value.lower()].append(query.inner.spec)
        self._inner = PyFullTextQuery({"boolean": spec})

    def query_type(self) -> FullTextQueryType:
        return FullTextQueryType.BOOLEAN
