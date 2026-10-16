# Task: expression value types -- decimal, duration, bracket indexing, DataFusion result types

Status: **delegated** (implementer: Claude Haiku 5.5, reviewer: Claude Opus 5.5). Three phases, each a
fresh session: **A** decimal / duration values and scalar indexes on them, **B** bracket indexing and
float16 list casts, **C** DataFusion's result types. Do the items of a phase in order; report the ones
you could not finish rather than stretching the session.

## 0. Context budget

About 100k tokens per session. `grep -n` then `sed -n` windows of at most ~120 lines; never read
`src/expr.cpp` (3.3k lines), `src/scalar_index.cpp`, `src/index_search.cpp`,
`bindings/python/nanolance/lance/dataset.py` or `bindings/python/src/nanolance_bindings.cpp` whole.
Cut all output (`| tail`, `| head`, `cut -c1-200`). For each item: read the named pylance tests
(`.deps/pylance-tests/v12.0.0/tests/...`), measure the behaviour on real pylance 12.0.0 (`import lance`
in plain `python3`), then implement. Write nanolance's own code.

## 1. Where things are

- Filter / projection expressions: `src/expr.cpp`. `detail::Kind` (line ~32: Null, Bool, Int, UInt,
  Double, String, Binary, Date, Timestamp, List) and `detail::Value` (~35) hold one value. Column
  values are read from Arrow arrays into `Value`s (grep `NANOARROW_TYPE_` in src/expr.cpp); literals
  are parsed (grep `Tok::`); `arrow_cast` is rewritten to `Op::Cast` at ~885 with a table of type
  names, and refuses the rest at ~908 (`arrow_cast to '...' is not supported in nanolance filters`).
  Comparison of two Values: grep `compare(` in src/expr.cpp. `Expression::result_type` gives a
  projection's Arrow type.
- Scalar index keys: `src/scalar_index.cpp`: `KeyKind` (grep `enum class KeyKind`), the type switch at
  ~95-132 (DECIMAL128 -> `KeyKind::Decimal128` exists; DECIMAL256 and DURATION refuse with "a scalar
  index on a column of type ... is not supported"), `Int128` at ~244. Index search with a predicate's
  literal: `src/index_search.cpp` (grep `Decimal128`).
- ZONEMAP / BITMAP builders reuse the same key kinds (grep `KeyKind` in src/zone*.cpp,
  src/scalar_index.cpp).

## 2. Items

**Phase A -- decimal and duration values (10 of pylance's tests)**

Targets: `tests/test_scalar_index.py::test_scalar_index_types[*]`: btree / bitmap / zonemap x
decimal128 / decimal256 / duration, plus `[btree-fixed-size-binary]` (fails now too: find why). Read
the test first: it builds the index, then filters (probably `arrow_cast` literals or comparisons with
Python `Decimal` / `timedelta` values the binding turns into SQL); measure the exact filter strings.

1. A `Decimal` value kind in expressions: decimal128 and decimal256 columns read as an exact
   (unscaled integer, precision, scale) value -- 128-bit as two halves, as `Int128` in
   scalar_index.cpp does; 256-bit as four. Comparisons between decimals of different scales rescale
   exactly (no doubles); decimal vs integer literal compares exactly; decimal vs float literal:
   measure what DataFusion does (it may cast to double). `arrow_cast(x, 'Decimal128(p, s)')` /
   `'Decimal256(p, s)'`, and decimal literals as the test writes them.
2. A `Duration` value kind: duration columns of each unit (s / ms / us / ns) as (count, unit);
   comparisons convert to a common unit exactly; `arrow_cast(x, 'Duration(Millisecond)')` and the
   other units. Measure whether pylance accepts `INTERVAL` literals against durations; support what
   the test uses, report the rest.
3. Scalar index keys for decimal256 (32-byte) and duration (int64 with its unit), for BTREE, BITMAP
   and ZONEMAP. Index files must equal pylance's: build the same index with both engines on the same
   data and compare the index files' rows (`index_files::read_table` on page_data / page_lookup, or
   pyarrow on the `.lance` index files) -- earlier index tasks did exactly this. Searches with a
   decimal / duration predicate use the index (check `explain_plan` / `analyze_plan` mention it as
   for int columns).
4. `[btree-fixed-size-binary]`: find and fix the cause.

**Phase A is split into three sessions** (the first session spent its budget measuring; its findings
are below, so do not re-measure them): **A1** = items 1 (decimal values in filters, no index work);
**A2** = item 2 (duration values); **A3** = items 3-4 (index keys, index files, the uuid case).

Measured on pylance 12.0.0 (session A0, 2026-10-16) -- the reviewer's decisions follow each:
- All 10 target cases: the index builds, the filter returns the expected row, and `explain_plan`
  contains `ScalarIndexQuery`. fixed-size-binary is `pa.uuid()` (an extension type over
  fixed_size_binary(16)); its query result keeps the extension type.
- Decimal128(10,2) column: `v = 2` (int literal) matches 2.00; `v = arrow_cast(2.5, 'Decimal128(3, 1)')`
  matches 2.50; `v = arrow_cast(2.50, 'Decimal128(10, 3)')` matches; `v = arrow_cast(2.00,
  'Decimal256(76, 2)')` matches (cross-width compares work). Bare float literals (`v = 2.0`, `v > 1.5`,
  `v = 2.00`) are errors: `Received literal Float64(2) and could not convert to literal of type
  Decimal128` -- **decision: nanolance errors the same way** (message containing that text).
  `v = arrow_cast(2.0000001, 'Float64')` compares as double (no error). `Decimal('2.00')` is not SQL.
  Decimal256(76,2): the same, exact, with Decimal128 literals too. Rounding of `arrow_cast(2.555,
  'Decimal128(10, 2)')` and int-literal overflow: measure in A1.
- Duration(ms) column: `v = arrow_cast(2000, 'Duration(Microsecond)')` matches the 2 ms row (exact unit
  conversion); `Second` / `Nanosecond` too. `v = 2` (int literal) is an error (`could not convert to
  literal of type Duration`) -- **decision: nanolance errors the same way** (today it compares the raw
  count, dropping the unit: a divergence to fix). `INTERVAL '2 milliseconds'` is unsupported in lance.
- Projections (phase C material, not A): decimal `v * 2` -> decimal128(21,4), `v + 1` -> (11,2),
  `v / 2` -> (16,6), `abs(v)` -> (10,2); duration `arrow_cast(v, 'Duration(Second)')` -> duration[s]
  truncating. **Decision: phase A only compares decimals / durations and projects bare columns.**
- Index files: not yet measured -- the page_data / page_lookup files live under
  `_indices/<uuid>/`; open them with nanolance's `index_files::read_table` (src/index_files.cpp) or
  `lance.file.LanceFileReader(path)` on the exact `.lance` file path, and match their column types.

Code map from A0 (grep-located):
- src/expr.cpp: `enum class Kind` (32), `struct Value` (35): add `Decimal` (four 64-bit limbs,
  two's complement, + scale) and `Duration` (count + unit; `unit_to_ns` ~1330). `read_scalar` (~1313):
  DECIMAL128 / 256 and unit-aware DURATION, the scale threaded from bind (ColumnRef). `bind_node`
  (~1080-1160): `scalar_type()` already admits DURATION; decimals are excluded. Literal coercion
  (~1222-1262): the two errors above. Number tokens (~705-715): keep a literal's source text so
  `arrow_cast(2.50, 'Decimal128(10, 3)')` folds exactly (not through a double). arrow_cast table
  (~885-910). `cast_value` (~1535; its "decimal" branch ~1569 is a float cast -- give decimal targets
  their own name). `compare()` (~1376): exact cross-scale compare (rescale in 512 bits: 10^76 fits).
  `kind_name` (~1420). The vectorised literal compare (~2390) compares DURATION raw counts: it must
  fall back for Duration literals. Output append (~2130) appends DURATION through cast_value "bigint".
- src/scalar_index.cpp: `KeyKind` (89), `key_kind_of` (91-131), `Int128` (246), `order_items<Int128>`
  (407), `zone_extrema<Int128>` (906); the uuid refusal at ~1563 (`extension_name` -> "a scalar index on
  an extension type is not supported"): key the storage type, keep what page_data stores (measure).
- src/index_search.cpp: grep `Decimal128` for the literal-to-key path.

**A1 outcome (2026-10-16):** decimal128 / decimal256 in filters, exact (46 tests); rounding half away
from zero, pylance's overflow and float-literal messages (pylance quotes the type: `'Decimal128(10,
2)'`, and shows the literal as DataFusion types it, `Float64(2)` for `2.0`).
**A2 outcome (2026-10-16):** durations of every unit in filters, exact across units without
overflow; int / float / string literals refused with pylance's messages; bare projections keep the
type (43 more tests); duration arithmetic and `abs()` refused as pylance refuses them. Divergence
kept on purpose: `arrow_cast(2.5, 'Duration(Millisecond)')` is an error here, an empty result in
pylance. Three pylance tests pass now (`test_scalar_index_types[*-duration]`). A3 next.

**A3 outcome (2026-10-16):** BTREE / BITMAP / ZONEMAP on decimal256 (a 256-bit key) and on uuid
(keyed by its fixed_size_binary(16) storage; zonemap bounds null, as pylance writes them); index
files equal pylance's for decimal128 / decimal256 / duration / uuid x the three kinds; index-on equals
index-off. All 19 `test_scalar_index_types` cases pass (+4). Follow-up (small, A1 area): DataFusion
refuses a decimal comparison whose common type needs more than 76 digits (`Cannot infer common
argument type for comparison operation Decimal256(76, 3) = Decimal128(10, 4)`); nanolance compares
them exactly -- refuse the same way. Lists of uuid stay unindexable.

**Phase B -- bracket indexing, float16 list casts (2 of pylance's tests)**

5. `l[1]` (list element, 1-based as DataFusion's `array_element`: measure; out of range -> null) and
   `s['x']` (struct field, same as `s.x`) in the expression grammar, in filters and projections.
   Target: `tests/test_dataset.py::test_nested_projection_list` (grep it).
6. `arrow_cast(vec, 'FixedSizeList(n, Float16)')` or whatever
   `test_dynamic_projection_with_vectors_index` uses (grep it): a list cast to float16 in projections.

**B outcome (2026-10-16):** `l[k]` (1-based; 0, past the end, null list / element -> null; negative
from the end), `s['x']` (as `s.x`), chains, in filters and projections; pylance's `_cast_list_f16`
UDF with round-to-nearest-even float16 (checked against numpy); decimal comparisons of mixed widths
whose common type passes 76 digits refused with DataFusion's messages. +2 pylance tests (1183). Left:
non-literal indexes (`l[1 + 1]`), projecting a struct element (`ls[1]`), `array_element(...)`,
case-insensitive struct fields (pylance refuses `s.X`; nanolance accepts it -- an older divergence).

**Phase C -- DataFusion's result types (no single pylance test; nanolance's own tests)**

7. Arithmetic and function result types as DataFusion coerces them, in `result_type` and evaluation:
   int32 + float32 -> float32 (nanolance: double), uint8 + int32 -> int32 (nanolance: int64),
   int32 column * int literal -> int64, int32 column + float literal (pylance: error -- measure the
   message), `abs(int32)` -> int32 or int64 (measure), `coalesce(x, 'lit')` non-null. Measure each with
   `ds.to_table(columns={"r": "<expr>"}).schema` on pylance; table them in your report. Change only
   the result types, not filter semantics.

**Phase C measurement (C0, 2026-10-16):** the full table -- every case as pylance's type and values
against nanolance's -- is `docs/tasks/data/expr_types_c0_table.txt` (`=` same, `!` differs). The
rules it shows:
- A literal is narrowed to its operand's type only when that operand is a column: cast if it fits,
  else pylance's error (`Received literal Int64(1000) and could not convert to literal of type
  'Int8'`; a float literal against an int column errors the same way, against a float32 column it
  narrows). Elsewhere (`-i8 + 1`, `coalesce(i32, 0)`) a literal stays int64 / double.
- Int with int: same signedness -> the wider; mixed -> signed of width max(2 x unsigned, signed);
  u64 with any signed -> decimal128(21,0) (i64 * u64 -> decimal128(38,0)). Results wrap on
  overflow; `x / 0` and `x % 0` error `Divide by zero error`; MIN / -1 errors `Arithmetic overflow:
  Overflow happened on: -128 / -1`; `abs(MIN)` errors `Int8Array overflow on abs(-128)`; negating
  an unsigned errors (`Negation only supports numeric, interval and timestamp types`).
- Int with float32 -> float32 computed in float; with float64 -> double.
- Decimal precisions of ints: i8/u8 3, i16/u16 5, i32/u32 10, i64/u64 20. add/sub: scale
  max(s1, s2), precision max(p1-s1, p2-s2) + scale + 1; mul: p1+p2+1, s1+s2; div (s1 = 2): scale 6,
  truncating (pin the rule for other scales); mod: min(p1-s1, p2-s2) + max(s1, s2); an int literal
  takes the decimal column's type; -dec and abs(dec) keep the type; CAST(dec AS INT) truncates;
  CAST(int / float AS DECIMAL(p, s)) is decimal (too large: `100 is too large to store in a
  Decimal128(2,0)`).
- coalesce: the common type of its arguments as above, literals not narrowed (coalesce(dec, 1) ->
  decimal128(22,2), coalesce(dec, 1.5) -> decimal128(30,15)).
- Temporal: dur +/- dur (same unit) -> duration; ts - ts -> duration; ts +/- dur -> timestamp;
  d32 - d32 -> int64 days; d32 +/- dur -> pylance's coercion error.

**Reviewer's decisions:** (1) filters keep their semantics (they return rows where pylance's
literal coercion refuses `i16 > 40000` / `i32 = 7.0`) -- a recorded divergence; the typed rules
apply to projections only (a mode of evaluation inside src/expr.cpp, not a planning change).
(2) u64 with a signed int -> decimal128(21,0) / (38,0), as pylance. (3) match ts - ts, ts +/- dur,
d32 - d32; refuse mixed-unit duration arithmetic (pylance's month_day_nano interval) with a clear
error. Phase C is split: **C1** integers and floats (a typed pass through `guess()` /
`result_type` and evaluation, wrapping, the error messages); **C2** decimals; **C3** temporal,
CAST, coalesce leftovers.

## 3. Oracles

`python3 tools/pylance_suite.py -k "<test names>" 2>&1 | grep -E "^FAILED|passed|failed" | tail -15`
(ignore the `tests/test_datagen.py` collection error).

## 4. Tests you write

`bindings/python/tests/test_expr_types.py` (new), both engines side by side via `require_pylance()`
from `tests.support`, a section per item: filters and projections on each type and unit, mixed
scales, nulls, index-vs-no-index results equal, and the index files equal pylance's.

## 5. Rules, builds, stop rules

`AGENTS.md` sections 5 and 7. No skipped / weakened tests. C++ without warnings. Do not commit, push,
change branches, or run `--update`. Do NOT schedule reminders, send_later or triggers of any kind. No
`pytest -n`. The full pytest takes ~13 minutes: run it in the background (Bash `run_in_background`)
with a 1500 s timeout. Delete only your own scratch files. Builds: `cd build && ninja`, then
`cd bindings/python/build/cp311-cp311-linux_x86_64 && ninja && cp
_nanolance.cpython-311-x86_64-linux-gnu.so /usr/local/lib/python3.11/dist-packages/nanolance/`.
Stop and ask (question first) when an item needs a new on-disk format pylance does not write, a
change to shared planning / scan code beyond the expression engine and index keys, or when pylance's
behaviour contradicts this spec.

## 6. Report

Last, once: `python3 tools/pylance_suite.py | tail -4`, ctest (`cd build && ctest -j4 | tail -3`),
and the full pytest in the background. Report: files changed, per-test status, decisions, the
measured pylance behaviours, questions.
