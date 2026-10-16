# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``LanceDataset.sql``: one SELECT over one dataset (docs/tasks/SQL_SURFACE.md, S2).

Statements: ``SELECT <items> FROM <table> [WHERE <expr>] [ORDER BY <name> [ASC|DESC] [NULLS FIRST|LAST],
...] [LIMIT n] [OFFSET n]``, and ``SELECT count(*) [AS name] FROM <table> [WHERE <expr>]``. The select
list is a projection of SQL expressions (the S1 ``columns={alias: expr}`` path), WHERE is a nanolance
filter, so scalar indexes apply. Anything else raises NotSupportedError naming the construct.

Measured on pylance 12.0.0: ORDER BY puts NULLS last for ASC and first for DESC; unquoted names are
lowercased; a string literal quotes its quote with ``''`` and a backslash is a plain character;
``count(*)`` is named ``count(*)`` unless aliased; a result with no rows has no batches.
"""

from __future__ import annotations

import re
from typing import Dict, List, NamedTuple, Optional, Tuple

import pyarrow as pa
import pyarrow.compute as pc

from nanolance.lance._errors import unsupported

_BLOB_MODES = ("all_binary", "blobs_descriptions", "all_descriptions")
_MAX_BATCH_SIZE = 4294967295
_KEYWORDS = {"and", "or", "not", "is", "null", "true", "false", "in", "between", "like", "ilike", "as",
             "asc", "desc", "nulls", "first", "last", "case", "when", "then", "else", "end"}
_AGGREGATES = {"count", "sum", "avg", "min", "max", "stddev", "stddev_samp", "stddev_pop", "variance",
               "var_samp", "var_pop", "median", "array_agg", "string_agg", "bool_and", "bool_or",
               "first_value", "last_value", "approx_distinct", "approx_median", "corr", "covar_samp",
               "covar_pop", "bit_and", "bit_or", "bit_xor"}
# Clause words this parser does not run: a statement that uses one raises NotSupportedError naming it.
_UNSUPPORTED = {"join": "JOIN", "inner": "JOIN", "outer": "JOIN", "cross": "JOIN", "group": "GROUP BY",
                "having": "HAVING", "union": "UNION", "intersect": "INTERSECT", "except": "EXCEPT",
                "window": "WINDOW", "qualify": "QUALIFY", "with": "WITH", "over": "window functions",
                "fetch": "FETCH", "distinct": "DISTINCT", "lateral": "LATERAL", "pivot": "PIVOT",
                "unnest": "UNNEST", "tablesample": "TABLESAMPLE", "into": "INTO"}

_TOKEN = re.compile(r"""\s+|--[^\n]*|'(?:[^']|'')*'|"(?:[^"]|"")*"|`(?:[^`]|``)*`
|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|\.\d+|[A-Za-z_][A-Za-z0-9_$]*|<=|>=|<>|!=|\|\||[-+*/%=<>(),.;]""", re.X)
_SIMPLE_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")


class _Tok(NamedTuple):
    kind: str  # word, qident, str, num, op
    text: str
    start: int
    end: int


def _tokenize(sql: str) -> List[_Tok]:
    toks, pos = [], 0
    while pos < len(sql):
        m = _TOKEN.match(sql, pos)
        if m is None:
            raise ValueError(f"syntax error at or near {sql[pos:pos + 12]!r}")
        text = m.group(0)
        pos = m.end()
        if text.isspace() or text.startswith("--"):
            continue
        if text[0] == "'":
            kind = "str"
        elif text[0] in "\"`":
            kind = "qident"
        elif text[0].isdigit() or text[0] == "." and len(text) > 1:
            kind = "num"
        elif text[0].isalpha() or text[0] == "_":
            kind = "word"
        else:
            kind = "op"
        toks.append(_Tok(kind, text, m.start(), m.end()))
    return toks


def _unquote(tok: _Tok) -> str:
    quote = tok.text[0]
    return tok.text[1:-1].replace(quote * 2, quote)


def _quote(name: str) -> str:
    """A column name as nanolance's expression grammar reads it: bare when it is a simple name."""
    return name if _SIMPLE_NAME.match(name) else "`" + name.replace("`", "``") + "`"


def _is_op(tok: Optional[_Tok], text: str) -> bool:
    return tok is not None and tok.kind == "op" and tok.text == text


def _is_word(tok: Optional[_Tok], *words: str) -> bool:
    return tok is not None and tok.kind == "word" and tok.text.lower() in words


def _ident(tok: _Tok) -> Tuple[str, bool]:
    """A name token's column name and whether it was quoted: an unquoted name is lowercased."""
    if tok.kind == "qident":
        return _unquote(tok), True
    return tok.text.lower(), False


def _is_name(tok: Optional[_Tok]) -> bool:
    return tok is not None and (tok.kind == "qident" or (tok.kind == "word" and tok.text.lower() not in _KEYWORDS))


class _Plan(NamedTuple):
    count: Optional[str]  # the output name of count(*), or None
    columns: List[Tuple[str, str]]  # (output name, projection value: a column or a SQL expression)
    where: Optional[str]
    order: List[Tuple[str, bool, bool]]  # (column or output name, ascending, nulls first)
    limit: Optional[int]
    offset: Optional[int]
    star: List[str]  # the columns a `*` in the select list expands to, if any


class _Parser:
    def __init__(self, sql: str, names: List[str], table: str):
        self.sql = sql
        self.names = names
        self.table = table
        self.toks = _tokenize(sql)
        self.i = 0

    def peek(self) -> Optional[_Tok]:
        return self.toks[self.i] if self.i < len(self.toks) else None

    def error(self, tok: Optional[_Tok]) -> ValueError:
        at = "end of input" if tok is None else repr(tok.text)
        return ValueError(f"syntax error at or near {at}")

    def segment(self, stops: Tuple[str, ...], comma: bool = False) -> List[_Tok]:
        """The tokens up to a comma (if ``comma``), a stop word, ';' or the end, outside parentheses."""
        seg, depth = [], 0
        while (tok := self.peek()) is not None:
            if depth == 0 and (_is_op(tok, ";") or (comma and _is_op(tok, ",")) or _is_word(tok, *stops)):
                break
            if _is_op(tok, "("):
                depth += 1
            elif _is_op(tok, ")"):
                depth -= 1
                if depth < 0:
                    raise self.error(tok)
            seg.append(tok)
            self.i += 1
        if depth:
            raise ValueError("syntax error: unbalanced parentheses")
        return seg

    def column(self, tok: _Tok) -> str:
        name, _quoted = _ident(tok)
        if name not in self.names:
            raise ValueError(f"Invalid user input: Schema error: No field named {name}.")
        return name

    def render(self, toks: List[_Tok]) -> str:
        """An expression as nanolance's filter / projection text. Names are checked against the table."""
        out, prev = "", None
        for i, tok in enumerate(toks):
            nxt = toks[i + 1] if i + 1 < len(toks) else None
            after_dot = prev is not None and _is_op(prev, ".")
            if tok.kind == "word" and tok.text.lower() in _AGGREGATES and _is_op(nxt, "("):
                raise unsupported(f"the aggregate function {tok.text.lower()}() (only count(*) alone)")
            if _is_op(tok, "(") and _is_word(nxt, "select"):
                raise unsupported("subqueries")
            if tok.kind in ("word", "qident") and after_dot:
                piece = _quote(_ident(tok)[0])
            elif tok.kind == "qident" or (tok.kind == "word" and tok.text.lower() not in _KEYWORDS
                                          and not _is_op(nxt, "(")):
                piece = _quote(self.column(tok))
            elif tok.kind == "word":
                piece = tok.text.lower()
            else:
                piece = self.sql[tok.start:tok.end]
            if out and not after_dot and not _is_op(tok, "."):
                out += " "
            out += piece
            prev = tok
        return out

    def parse(self) -> _Plan:
        if not self.toks:
            raise ValueError("No SQL statements were provided in the query string")
        if not _is_word(self.peek(), "select"):
            self.reject_clause(self.peek())
            raise self.error(self.peek())
        self.i += 1
        if _is_word(self.peek(), "distinct"):
            raise unsupported("SELECT DISTINCT")
        items = []
        while True:
            seg = self.segment(("from",) + tuple(_UNSUPPORTED), comma=True)
            if not seg:
                raise self.error(self.peek())
            items.append(seg)
            if not _is_op(self.peek(), ","):
                break
            self.i += 1
        if not _is_word(self.peek(), "from"):
            self.reject_clause(self.peek())
            raise self.error(self.peek())
        self.i += 1
        table = self.peek()
        if _is_op(table, "("):
            raise unsupported("subqueries in FROM")
        if table is None or table.kind not in ("word", "qident") or table.text.lower() in _KEYWORDS:
            raise self.error(table)
        self.i += 1
        if _is_op(self.peek(), ","):
            raise unsupported("several tables in FROM (joins)")
        after = self.peek()
        if after is not None and after.kind == "word" and after.text.lower() not in ("where", "order", "limit",
                                                                                     "offset"):
            self.reject_clause(after)
            raise unsupported("table aliases in FROM")
        name = _ident(table)[0]
        if name.lower() != self.table.lower():
            raise ValueError(f"table '{name}' not found")

        where = None
        if _is_word(self.peek(), "where"):
            self.i += 1
            seg = self.segment(("order", "limit", "offset") + tuple(_UNSUPPORTED))
            if not seg:
                raise self.error(self.peek())
            where = self.render(seg)
        self.reject_clause(self.peek())

        order = []
        if _is_word(self.peek(), "order"):
            self.i += 1
            if not _is_word(self.peek(), "by"):
                raise self.error(self.peek())
            self.i += 1
            while True:
                order.append(self.order_key(self.segment(("limit", "offset") + tuple(_UNSUPPORTED), comma=True)))
                if not _is_op(self.peek(), ","):
                    break
                self.i += 1

        limit = offset = None
        while True:
            if _is_word(self.peek(), "limit") and limit is None:
                self.i += 1
                limit = self.count()
            elif _is_word(self.peek(), "offset") and offset is None:
                self.i += 1
                offset = self.count()
            else:
                break
        if _is_op(self.peek(), ";"):
            self.i += 1
        if self.peek() is not None:
            self.reject_clause(self.peek())
            raise self.error(self.peek())

        return self.plan(items, where, order, limit, offset)

    def reject_clause(self, tok: Optional[_Tok]) -> None:
        if tok is not None and tok.kind == "word" and tok.text.lower() in _UNSUPPORTED:
            raise unsupported(f"{_UNSUPPORTED[tok.text.lower()]} in ds.sql()")

    def count(self) -> int:
        tok = self.peek()
        if tok is None or tok.kind != "num" or not tok.text.isdigit():
            raise self.error(tok)
        self.i += 1
        return int(tok.text)

    def order_key(self, seg: List[_Tok]) -> Tuple[str, bool, Optional[bool]]:
        if not seg:
            raise self.error(self.peek())
        if seg[0].kind == "num":
            raise unsupported("ORDER BY a column position")
        if not _is_name(seg[0]):
            raise unsupported("ORDER BY an expression (only columns and aliases)")
        name = _ident(seg[0])[0]
        rest = [t.text.lower() for t in seg[1:]]
        asc, nulls_first = True, None
        if rest[:1] in (["asc"], ["desc"]):
            asc = rest.pop(0) == "asc"
        if rest[:1] == ["nulls"] and rest[1:2] in (["first"], ["last"]):
            nulls_first = rest[1] == "first"
            rest = rest[2:]
        if rest:
            raise unsupported("ORDER BY an expression (only columns and aliases)")
        return name, asc, nulls_first

    def select_item(self, seg: List[_Tok]) -> Tuple[Optional[str], str, Optional[str]]:
        """(output name or None, projection value, the text the name is taken from): one select-list item."""
        alias, body = None, seg
        if len(seg) >= 3 and _is_word(seg[-2], "as"):
            alias, body = _ident(seg[-1])[0], seg[:-2]
        elif len(seg) >= 2 and _is_name(seg[-1]) and (seg[-2].kind in ("word", "qident", "num", "str")
                                                      and not _is_word(seg[-2], *_KEYWORDS)
                                                      or _is_op(seg[-2], ")")):
            alias, body = _ident(seg[-1])[0], seg[:-1]
        if not body:
            raise self.error(seg[0])
        if len(body) == 1 and _is_name(body[0]):
            column = self.column(body[0])
            return alias or column, _quote(column), None
        return alias, self.render(body), self.sql[body[0].start:body[-1].end]

    def plan(self, items, where, order, limit, offset) -> _Plan:
        count_alias = None
        if len(items) == 1 and len(items[0]) >= 4 and _is_word(items[0][0], "count") \
                and _is_op(items[0][1], "(") and _is_op(items[0][2], "*") and _is_op(items[0][3], ")"):
            tail = items[0][4:]
            if not tail or (len(tail) == 2 and _is_word(tail[0], "as") and _is_name(tail[1])):
                if order or limit is not None or offset is not None:
                    raise unsupported("ORDER BY, LIMIT or OFFSET on count(*)")
                count_alias = _ident(tail[1])[0] if tail else "count(*)"
                return _Plan(count_alias, [], where, [], None, None, [])

        columns: List[Tuple[str, str]] = []
        star: List[str] = []
        for seg in items:
            if len(seg) == 1 and _is_op(seg[0], "*"):
                star = list(self.names)
                columns += [(name, _quote(name)) for name in self.names]
                continue
            name, value, text = self.select_item(seg)
            columns.append((name or text, value))
        outputs = [name for name, _ in columns]
        dup = next((n for n in outputs if outputs.count(n) > 1), None)
        if dup is not None:
            raise ValueError(f"Projections require unique expression names but the name {dup!r} is used more "
                             "than once. Consider aliasing (\"AS\") one of them.")

        resolved = []
        for name, asc, nulls_first in order:
            if name not in outputs and name not in self.names:
                raise ValueError(f"Invalid user input: Schema error: No field named {name}.")
            if nulls_first is None:
                nulls_first = not asc  # NULLS sort as the largest value: last for ASC, first for DESC
            resolved.append((name, asc, nulls_first))
        return _Plan(None, columns, where, resolved, limit, offset, star)


def _batches(table: pa.Table, batch_size: Optional[int], batch_size_bytes: Optional[int]) -> List[pa.RecordBatch]:
    """The table as batches of at most ``batch_size`` rows, cut within each of its chunks (a fragment's rows,
    as pylance cuts them: a filtered scan gives 3, 1, 3 over fragments of 4, 4, 2 rows). Without it, a byte
    budget gives the rows per batch by the table's average row size, as nanolance's scanner does."""
    size = batch_size
    if size is None and batch_size_bytes and table.num_rows:
        size = max(1, batch_size_bytes * table.num_rows // max(table.nbytes, 1))
    return table.to_batches(max_chunksize=size)


class SqlQueryBuilder:
    """pylance's ``SqlQueryBuilder``: the options of one ``LanceDataset.sql`` query, then ``build()``."""

    def __init__(self, dataset, sql: str):
        self._dataset = dataset
        self._sql = sql
        self._table_name = "dataset"
        self._batch_size: Optional[int] = None
        self._batch_size_bytes: Optional[int] = None
        self._blob_handling: Optional[str] = None
        self._with_row_id = False
        self._with_row_addr = False

    def table_name(self, table_name: str) -> "SqlQueryBuilder":
        self._table_name = table_name
        return self

    def batch_size(self, batch_size: int) -> "SqlQueryBuilder":
        if batch_size < 0:
            raise OverflowError("can't convert negative int to unsigned")
        if not 1 <= batch_size <= _MAX_BATCH_SIZE:
            raise ValueError(f"batch_size must be between 1 and {_MAX_BATCH_SIZE}, got {batch_size}")
        self._batch_size = batch_size
        return self

    def batch_size_bytes(self, batch_size_bytes: int) -> "SqlQueryBuilder":
        if batch_size_bytes < 0:
            raise OverflowError("can't convert negative int to unsigned")
        self._batch_size_bytes = batch_size_bytes
        return self

    def blob_handling(self, blob_handling: str) -> "SqlQueryBuilder":
        if blob_handling not in _BLOB_MODES:
            raise ValueError(f"Invalid blob_handling: {blob_handling}. Expected one of: {', '.join(_BLOB_MODES)}")
        self._blob_handling = blob_handling
        return self

    def with_row_id(self, with_row_id: bool = True) -> "SqlQueryBuilder":
        self._with_row_id = bool(with_row_id)
        return self

    def with_row_addr(self, with_row_addr: bool = True) -> "SqlQueryBuilder":
        self._with_row_addr = bool(with_row_addr)
        return self

    def build(self) -> "SqlQuery":
        plan = _Parser(self._sql, list(self._dataset.schema.names), self._table_name).parse()
        return SqlQuery(self._dataset, plan, self)


class SqlQuery:
    """pylance's ``SqlQuery``: runs the parsed statement and returns its batches or a reader over them."""

    def __init__(self, dataset, plan: _Plan, options: SqlQueryBuilder):
        self._dataset = dataset
        self._plan = plan
        self._batch_size = options._batch_size
        self._batch_size_bytes = options._batch_size_bytes
        self._blob_handling = options._blob_handling
        self._with_row_id = options._with_row_id
        self._with_row_addr = options._with_row_addr

    def _scan(self, columns: Dict[str, str], limit: Optional[int], offset: Optional[int]) -> pa.Table:
        return self._dataset.scanner(
            columns=columns, filter=self._plan.where, limit=limit, offset=offset,
            with_row_id=self._with_row_id or None, with_row_address=self._with_row_addr or None,
            blob_handling=self._blob_handling).to_table()

    def _table(self) -> pa.Table:
        plan = self._plan
        if plan.count is not None:
            n = self._dataset.count_rows(filter=plan.where)
            return pa.Table.from_arrays([pa.array([n], pa.int64())],
                                        schema=pa.schema([pa.field(plan.count, pa.int64(), nullable=False)]))
        columns = dict(plan.columns)
        hidden = []
        for name, _asc, _nulls_first in plan.order:
            if name not in columns:  # a column that is sorted on but not returned
                columns[name] = _quote(name)
                hidden.append(name)
        if not any(_SIMPLE_NAME.match(v) and v.lower() not in _KEYWORDS for v in columns.values()):
            # Only expressions (`SELECT 'x' AS s`): nanolance's projection needs a column to read, so one
            # is read and dropped below, which keeps the row count.
            anchor = "_sql_anchor"
            while anchor in columns:
                anchor += "_"
            columns[anchor] = _quote(self._dataset.schema.names[0])
            hidden.append(anchor)
        if not plan.order:
            table = self._scan(columns, plan.limit, plan.offset)
        else:
            table = self._scan(columns, None, None)
            for name, asc, nulls_first in reversed(plan.order):  # stable passes, least significant key first
                order = pc.sort_indices(table.select([name]), sort_keys=[
                    (name, "ascending" if asc else "descending", "at_start" if nulls_first else "at_end")])
                table = table.take(order)
            table = table.slice(plan.offset or 0, plan.limit).combine_chunks()  # one chunk: batches cut evenly
        table = table.select([c for c in table.column_names if c not in hidden])
        system = [c for c in ("_rowid", "_rowaddr") if c in table.column_names]
        if plan.star and system:  # pylance: the row ids come right after the columns of a `*`
            names = [c for c in table.column_names if c not in system]
            pos = names.index(plan.star[-1]) + 1
            table = table.select(names[:pos] + system + names[pos:])
        return table

    def to_batch_records(self) -> List[pa.RecordBatch]:
        return _batches(self._table(), self._batch_size, self._batch_size_bytes)

    def to_stream_reader(self) -> pa.RecordBatchReader:
        table = self._table()
        return pa.RecordBatchReader.from_batches(table.schema, _batches(table, self._batch_size,
                                                                        self._batch_size_bytes))
