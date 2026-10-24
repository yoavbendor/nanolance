# expr: what a filter means (no storage)

The expr layer parses and evaluates SQL expressions over Arrow record batches: the filters of a scan, count,
delete or update (`id > 100 AND name LIKE 'a%'`), and the values an update or an added column writes
(`price * 2`). It follows DataFusion's SQL as pylance runs it, coercions and errors included, and refuses
what it does not cover by name instead of evaluating it differently. It reads no files: the scan layer feeds
it batches, and the index layer asks it for the shape of a filter (`Condition`) and evaluates single
predicates on index keys. It uses `base` (Unicode, 128-bit integers, float16) and `arrow` (JSONB).

## Using it

- The entry point is `nano_lance::expr::Expression` (`include/nanolance/expr.hpp`):
  `Expression::parse(sql, e, error)`, then `e.bind(schema, error)` against the batch's struct schema, then
  `e.filter(batch, keep, error)` (`keep[i]` is 1 where the expression is TRUE; NULL and FALSE drop the row,
  as WHERE does) or `e.evaluate(batch, type, out, error)` for a value cast to `type`. `bind` must come
  before either and binding again rebinds; `columns()` names the top-level columns a scan must read.
- A column name matches exactly, else case-insensitively when that is unambiguous; quote a name with `"`
  or `` ` `` when it has spaces or must match exactly. `s.child` and `s['child']` read struct fields.
- Index planning: `conditions()` gives the filter's AND / OR / NOT tree with each `Predicate` (one column
  against constants); `Predicate::filter` evaluates one on any array of the column's type (an index's keys)
  with the filter's own semantics, and `constant_as` casts a constant to the column's type as the filter
  does. A part no index answers is `Condition::Kind::Other`.
- An `Expression` is movable, not copyable. `filter` and `evaluate` are const and keep no state between calls
  (the expression has no mutable members), so the scan runs one bound expression on several batches; parse
  and bind before sharing it, and never bind while another thread evaluates.
- Errors are a `false` return with a message worded as pylance words it where the two differ in nothing else
  (a bad cast, a decimal clash, an unknown function).

## Modules

| module | what it holds | tests |
|---|---|---|
| `expr` | tokenizer, parser, binder (coercions), evaluator, index predicates | `nano_lance_expr` |

## Spec coverage

The Lance format specification at https://lance.org/format/ (as of Lance 12.0.0) has no page for filters:
the rows follow DataFusion's SQL as pylance 12.0.0 runs it.

| ID | Spec | Status | Implementation | Tests | Verified by | Notes |
|---|---|---|---|---|---|---|
| `expr.literals` | ref: DataFusion SQL literals | done | `src/expr/expr.cpp::tokenize`, `src/expr/expr.cpp::parse_datetime` | `nano_lance_expr`, `bindings/python/tests/test_pylance_compat.py::test_filters_agree` | unit+interop | Numbers, strings, `TRUE` / `FALSE` / `NULL`, `DATE '...'`, `TIMESTAMP '...'`, `[...]` and `ARRAY[...]`. |
| `expr.columns` | ref: DataFusion SQL identifiers and field access | done | `src/expr/expr.cpp::find_child`, `src/expr/expr.cpp::read_column` | `nano_lance_expr`, `bindings/python/tests/test_expr_types.py::test_bracket_filter_equals_pylance`, `bindings/python/tests/test_expr_types.py::test_bracket_errors_like_pylance` | unit+interop | Quoted names, struct fields (`s.a`, `s['a']`), list elements (`l[1]`). |
| `expr.compare` | ref: DataFusion SQL comparisons and type coercion | done | `src/expr/expr.cpp::compare`, `src/expr/expr.cpp::coerce`, `src/expr/expr.cpp::float_way` | `nano_lance_expr`, `bindings/python/tests/test_pylance_compat.py::test_filters_agree`, `bindings/python/tests/test_expr_types.py::test_numeric_filters_keep_their_rows` | unit+interop | `= == != <> < <= > >=`; a string literal compared with a date or timestamp column is cast to it; a number with a float32 column is rounded to float32; floats in IEEE total order with the zeros equal. |
| `expr.logic` | ref: DataFusion SQL `AND` / `OR` / `NOT`, `IS [NOT] TRUE / FALSE` | done | `src/expr/expr.cpp::Expression` | `nano_lance_expr`, `bindings/python/tests/test_pylance_compat.py::test_filters_agree` | unit+interop | SQL's three-valued logic. |
| `expr.in_between` | ref: DataFusion SQL `[NOT] IN`, `[NOT] BETWEEN` | done | `src/expr/expr.cpp::compare` | `nano_lance_expr`, `bindings/python/tests/test_pylance_compat.py::test_filters_agree` | unit+interop | A NULL in the IN list makes a non-match NULL, as SQL has it. |
| `expr.is_null` | ref: DataFusion SQL `IS [NOT] NULL`, `is_null()`, `is_valid()` | done | `src/expr/expr.cpp::Expression` | `nano_lance_expr`, `bindings/python/tests/test_pylance_compat.py::test_filters_agree` | unit+interop | |
| `expr.like` | ref: DataFusion SQL `[NOT] LIKE` / `ILIKE` | partial | `src/expr/expr.cpp::like`, `src/expr/expr.cpp::like_bytes` | `nano_lance_expr`, `bindings/python/tests/test_unicode_filters.py::test_filter_matches_pylance` | unit+interop | `%`, `_` (one character, not one byte), `\` escapes; ILIKE compares Unicode simple case folds. Missing: the `ESCAPE '<c>'` clause. |
| `expr.cast` | ref: DataFusion SQL `CAST(x AS type)`, `x::type`, `arrow_cast(x, 'Type')` | done | `src/expr/expr.cpp::fold_decimal_cast`, `src/expr/expr.cpp::coerce` | `nano_lance_expr`, `bindings/python/tests/test_expr_types.py::test_arrow_cast_decimal_equal_pylance`, `bindings/python/tests/test_json_columns.py::test_other_functions`, `bindings/python/tests/test_expr_types.py::test_cast_list_f16_equals_pylance` | unit+interop | |
| `expr.arithmetic` | ref: DataFusion SQL arithmetic (`+ - * / %`, unary minus, string concatenation) | done | `src/expr/expr.cpp::Expression` | `nano_lance_expr`, `bindings/python/tests/test_expr_types.py::test_numeric_projection_equals_pylance`, `bindings/python/tests/test_expr_types.py::test_integer_arithmetic_filters_keep_their_rows` | unit+interop | Integer arithmetic in int64, float64 once a float is involved; division by zero is an error. |
| `expr.decimal` | ref: DataFusion decimal comparison and arithmetic | done | `src/expr/expr.cpp::compare_decimal`, `src/expr/expr.cpp::decimals_clash` | `bindings/python/tests/test_expr_types.py::test_decimal_filters_equal_pylance`, `bindings/python/tests/test_expr_types.py::test_decimal_projection_equals_pylance`, `bindings/python/tests/test_expr_types.py::test_decimal_clash_is_refused_like_pylance` | interop | Exact, at full precision; mixed widths refused past 76 digits as DataFusion refuses them. |
| `expr.temporal` | ref: DataFusion date, timestamp and duration comparison and arithmetic | done | `src/expr/expr.cpp::compare_duration`, `src/expr/expr.cpp::parse_datetime` | `nano_lance_expr`, `bindings/python/tests/test_expr_types.py::test_duration_filters_equal_pylance`, `bindings/python/tests/test_expr_types.py::test_c3_projection_equals_pylance`, `bindings/python/tests/test_expr_types.py::test_c3_mixed_unit_duration_arithmetic_is_refused` | unit+interop | `now()`, `current_date()`, `current_timestamp()` too. Mixed-unit duration arithmetic (an interval in pylance) is refused. |
| `expr.functions.string` | ref: DataFusion `lower`, `upper`, `length` / `char_length`, `starts_with`, `ends_with`, `contains` | done | `src/expr/expr.cpp::arity` | `nano_lance_expr`, `bindings/python/tests/test_unicode_filters.py::test_filter_matches_pylance`, `bindings/python/tests/test_unicode_filters.py::test_lower_upper_values_match_pylance` | unit+interop | Over characters: Unicode case mappings (the final sigma, ß -> SS), length in characters. |
| `expr.functions.other` | ref: DataFusion `abs`, `coalesce` | done | `src/expr/expr.cpp::arity` | `nano_lance_expr`, `bindings/python/tests/test_expr_types.py::test_numeric_projection_equals_pylance` | unit+interop | Result types as DataFusion gives them. |
| `expr.functions.list` | ref: DataFusion `array_has`, `array_has_any`, `array_has_all`, `array_contains` (and the `list_*` aliases) | done | `src/expr/expr.cpp::is_array_function` | `bindings/python/tests/test_json_columns.py::test_other_functions`, `bindings/python/tests/test_scalar_index.py::test_vectorized_filters_match_the_interpreter` | unit+interop | |
| `expr.regexp` | ref: DataFusion `regexp_like`, `regexp_match` (Rust regex crate syntax and flags) | done | `src/expr/expr.cpp::arity` | `bindings/python/tests/test_unicode_filters.py::test_filter_matches_pylance`, `bindings/python/tests/test_json_columns.py::test_other_functions` | interop | Over characters: `.`, ranges, `\w \d \s` from Unicode tables, the `i` flag by case fold; `[[:alpha:]]`-style classes stay ASCII, as in that crate. Runs on the C++ library's ECMAScript engine, so syntax that crate has and it lacks (named groups `(?P<n>)`, inline flags) differs. |
| `expr.json` | ref: Lance's JSON UDFs (`json_get`, `json_get_string` / `_int` / `_float` / `_bool`, `json_extract`, `json_exists`, `json_array_contains`, `json_array_length`) | done | `src/expr/expr.cpp::arity`, `src/arrow/jsonb.cpp::select` | `nano_lance_jsonb`, `bindings/python/tests/test_json_columns.py::test_json_filters_match_pylance`, `bindings/python/tests/test_json_columns.py::test_json_errors` | unit+interop | |
| `expr.index_predicates` | ref: pylance's scalar-index routing of filters | done | `include/nanolance/expr.hpp::Condition`, `include/nanolance/expr.hpp::Predicate` | `bindings/python/tests/test_expr_types.py::test_index_is_used_for_comparisons_and_in`, `bindings/python/tests/test_expr_types.py::test_index_filters_equal_pylance` | interop | |
| `expr.refusals` | ref: DataFusion's errors for constructs it rejects | done | `src/expr/expr.cpp::Expression` | `nano_lance_expr`, `bindings/python/tests/test_expr_types.py::test_c3_refusals_equal_pylance`, `bindings/python/tests/test_expr_types.py::test_bare_float_literal_against_decimal_errors` | unit+interop | Unsupported syntax and functions are refused by name ("function 'x' is not supported in nanolance filters"). |
| `expr.case` | ref: DataFusion SQL `CASE WHEN ... THEN ... ELSE ... END` | missing | | | - | Refused as a syntax error. |
| `expr.distinct_from` | ref: DataFusion SQL `IS [NOT] DISTINCT FROM` | missing | | | - | |
| `expr.functions.missing` | ref: DataFusion's other scalar functions (`substr`, `concat`, `trim`, `replace`, `regexp_replace`, `date_trunc`, `date_part`, `round`, `floor`, `ceil`, ...) | missing | | | - | Refused by name. |

## Known gaps

The common filters are all here; what is missing is the long tail of DataFusion's scalar functions, `CASE`,
`IS DISTINCT FROM` and `LIKE ... ESCAPE`. Each is refused by name, so a filter never returns different rows
from pylance's, only an error where pylance answers. Adding a function means an `arity` entry, a type rule
in the binder and an evaluator case, plus a parity test against pylance. Regular expressions run on the C++
standard library's engine with Unicode classes added: patterns using Rust regex features that ECMAScript
lacks behave differently, which a parity test per feature would pin down.
