# Task: SQL expressions in projections, `LanceDataset.sql`

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Two phases, each a
fresh session: **S1** projection expressions, **S2** `ds.sql(...)`. ~15 of pylance's tests.

## 0. Context budget

About 100k tokens per session. `grep -n` then `sed -n` windows of at most ~120 lines; never read
`bindings/python/nanolance/lance/dataset.py` (3.5k+), `src/expr.cpp` (3k+), `src/dataset_ops.cpp`
or `bindings/python/src/nanolance_bindings.cpp` whole. Cut all output. Real pylance 12.0.0 is
`import lance` in plain `python3`: measure every behaviour there first. Write nanolance's own code.

## 1. Facts

nanolance already has a SQL expression engine in C++ (`include/nanolance/expr.hpp`):
`Expression::parse`, `bind(schema)`, `filter(batch)`, **`evaluate(batch, type, out)`** and
**`result_type(name, out)`** (int64 for integer arithmetic, float64 with a float, utf8 for string
functions, bool for predicates), with `lower`, `upper`, `length`, `abs`, `coalesce`, `json_get_*`,
`json_array_length`, `arrow_cast`, ... (grep the function table near src/expr.cpp:1170).
`add_columns` with SQL expressions uses them (src/dataset_ops.cpp ~1409): the model for evaluating
expressions over scanned batches.

**S1 -- projection expressions** (`test_dataset.py::test_take_with_projection`,
`test_nested_projection_list`, `test_dynamic_projection`, `test_schema_evolution.py::
test_add_columns_from_file`, `test_vector_index.py::test_dynamic_projection_with_vectors_index` --
read each): `columns={"alias": "<sql expr>", ...}` in `to_table`, `scanner`, `take`, `_take_rows`,
`to_batches` (pylance: `{'a2': 'a*2', 'bu': 'UPPER(b)', 'c': 'a > 1'}` -> `a2: int64, bu: string,
c: bool`, in dict order). Today Python refuses anything but a rename
(dataset.py ~120: `unsupported("SQL expressions in a column projection")`). Design: read the columns
the expressions name, evaluate each expression per batch in C++ (a new binding over
`Expression::evaluate`, typed by `result_type`), keep plain columns and renames as today; with
filters, limits, KNN (`_distance`), FTS (`_score`) and system columns (`_rowid` ...) working as for a
plain projection. Measure pylance's result types for literals, mixed arithmetic, strings, json, and
nested fields (`s.x`), and its errors for unknown columns.

**S2 -- `LanceDataset.sql(sql)`** (`test_dataset.py::test_dataset_sql*` 4 tests, `test_json.py` 5
tests): returns a `SqlQueryBuilder` with `table_name(name)` (default `dataset`), `batch_size(n)`
(1..4294967295, else ValueError `batch_size must be between 1 and 4294967295`), `batch_size_bytes`,
`blob_handling`, `with_row_id`, `with_row_addr`, `build()` -> `SqlQuery` with `to_batch_records()`
(list of RecordBatch) and `to_stream_reader()` (a RecordBatchReader). The tests use
`SELECT * | <expr> [AS alias], ... FROM <name> [WHERE <expr>]`; pylance (DataFusion) also does
`ORDER BY`, `LIMIT`, `IN`, `count(*)` (measured: `SELECT a, b FROM dataset ORDER BY a DESC LIMIT 2`,
`SELECT count(*) AS n FROM dataset`). Scope: a small SELECT parser (in Python is fine) for: select
list with `*`, expressions, aliases; one FROM table matching `table_name`; WHERE; ORDER BY columns /
aliases ASC/DESC; LIMIT / OFFSET; `count(*)` alone. Run WHERE as a nanolance filter (so scalar
indexes apply) and the select list through S1's evaluation. Anything else (joins, GROUP BY,
subqueries, other aggregates) -> NotSupportedError naming the construct. Unknown table -> ValueError
like pylance's `table '...' not found` (without the DataFusion path / Rust location).

## 2. Oracles

`python3 tools/pylance_suite.py -k "<expr>" 2>&1 | grep -E "^FAILED|passed|failed" | tail -15`
(ignore the `tests/test_datagen.py` collection error). S1: `-k "take_with_projection or
nested_projection_list or dynamic_projection or add_columns_from_file"`. S2: `-k "dataset_sql or
test_json_path_queries or test_json_get_functions or test_nested_json_access or
test_json_array_operations or json_filter_append_missing_json_cast"`.

## 3. Tests you write

`bindings/python/tests/test_sql_surface.py` (new), both engines side by side via `require_pylance()`
from `tests.support`: same schemas and rows for many projections / queries, with filters, indexes,
deletions, multiple fragments; errors.

## 4. Rules, builds, stop rules

`AGENTS.md` sections 5 and 7. No skipped / weakened tests. C++ without warnings. Do not commit, push,
change branches, or run `--update`. Do not schedule reminders or triggers. No `pytest -n`; clean temp
dirs. Builds: `cd build && ninja`, then `cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja
&& cp _nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.
Stop and ask (question first) if the expression engine lacks a construct a target test needs and
adding it is more than a function-table entry, or if pylance's result types differ from
`result_type`'s in a way that needs a type-system change. If context nears its limit, stop at a
clean point and report what is done and left.

## 5. Done per phase -- report (under 40 lines, sent once)

Tails of: `cd build && ctest -j8 | grep "tests passed"`; the oracle `-k` run; your test file;
`python3 tools/pylance_suite.py | tail -4`; last, once:
`cd bindings/python && python3 -m pytest -o addopts= -q -rf -p no:cacheprovider tests | tail -3`.
Files changed, per-test status, decisions, questions.

**S1 outcome (2026-10-16):** done except two tests. `columns={"alias": "<sql>"}` in to_table / scanner /
to_batches / take / _take_rows, evaluated per batch in C++ (`_project_batch` over
`Expression::evaluate`, typed by `result_type`) after filter / limit / offset; aliased system columns;
`_distance` / `_score` after the named columns as pylance. +3 pylance tests (1135). Left (reviewer):
`test_nested_projection_list` needs list / bracket indexing (`l[1]`, `s['x']`) in the expression
grammar, and `test_dynamic_projection_with_vectors_index` needs a float16 list cast -- both roadmap
items, the tests stay failing. Result-type differences from pylance (DataFusion's coercion), not
changed: int32 + float32 -> float32 (nanolance double), uint8 + int32 -> int32 (int64), int32 column +
float literal is an error in pylance, `abs(int32)` -> int64 there, `coalesce(x, 'lit')` non-null there,
`S.X` case-insensitive struct access accepted by nanolance only, a stray trailing `)` accepted by
pylance only. S2 next.

**S2 outcome (2026-10-16):** done. `ds.sql(sql)` -> `SqlQueryBuilder` -> `SqlQuery` (`_sql.py`): a
SELECT parser for `*` / expressions / aliases, one FROM table (case-insensitive), WHERE through
nanolance's filter (scalar indexes apply), ORDER BY columns / aliases (NULLS last ASC, first DESC),
LIMIT / OFFSET, `count(*)` alone; every other construct is a NotSupportedError naming it. All 9
target tests (+9, 1144). Divergences: `int32_col * <int literal>` is int64 in DataFusion, int32 here
(the result-type small item); unaliased computed columns are named as written (pylance: `dataset.id +
Int64(1)`); without `batch_size` one batch for the result (pylance: one per fragment);
`batch_size_bytes` validated, not applied; `to_stream_reader` materializes first; `SELECT FROM
dataset` (no columns) is an error here. Task SQL_SURFACE done.
