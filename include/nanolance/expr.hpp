// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/// SQL expressions over a dataset's rows: the filters of a scan, count, delete or update
/// (`id > 100 AND name LIKE 'a%'`), and the values of an update or of an added column (`price * 2`).
///
/// The dialect is the subset of the DataFusion SQL that Lance filters use in practice:
///
///   literals        1, -2.5, 1e3, 'text', TRUE, FALSE, NULL, DATE '2024-01-31',
///                   TIMESTAMP '2024-01-31 12:00:00'
///   columns         name, "Mixed Case", `with space`, struct_col.child
///   comparison      =  ==  !=  <>  <  <=  >  >=
///   logic           AND  OR  NOT, with SQL's three-valued logic
///   predicates      IS [NOT] NULL, IS [NOT] TRUE / FALSE, [NOT] IN (...), [NOT] BETWEEN a AND b,
///                   [NOT] LIKE / ILIKE 'pat%'
///   arithmetic      + - * / % and unary minus; || concatenates strings
///   functions       lower, upper, length / char_length, abs, coalesce, starts_with, ends_with,
///                   contains, is_null, is_valid, CAST(x AS type)
///
/// A column compared with a string or DATE / TIMESTAMP literal compares as the column's type, as
/// DataFusion coerces it. What the dialect does not cover is refused with an error naming it, never
/// evaluated differently.
namespace nano_lance::expr {

struct Node;

/// A parsed expression.
class Expression {
public:
    Expression();
    ~Expression();
    Expression(Expression&&) noexcept;
    Expression& operator=(Expression&&) noexcept;
    Expression(const Expression&) = delete;
    Expression& operator=(const Expression&) = delete;

    /// Parse `sql`. False with `error` set on a syntax error or an unsupported construct.
    static bool parse(std::string_view sql, Expression& out, std::string& error);

    /// The top-level columns the expression reads (a struct field's column for `s.child`), in the
    /// order first named. A name that matches no column exactly is resolved case-insensitively by
    /// bind(), so these are the names as written.
    std::vector<std::string> columns() const;

    /// Resolve column references against `schema` (a struct: a record batch's) and check types.
    /// Must precede evaluation; binding again rebinds.
    bool bind(const ArrowSchema& schema, std::string& error);

    /// Evaluate as a filter over `batch` (of the bound schema): `keep[i]` is 1 where the expression
    /// is TRUE (NULL and FALSE drop the row, as SQL's WHERE does).
    bool filter(const ArrowArray& batch, std::vector<std::uint8_t>& keep, std::string& error) const;

    /// Evaluate over `batch` into an Arrow array of `type` (e.g. the column an update writes):
    /// one value per row, NULL where the expression is. The value is cast to `type` as SQL would
    /// (an integer to a float, a string to a date); a value that does not fit is an error.
    bool evaluate(const ArrowArray& batch, const ArrowSchema& type, ArrowArray& out, std::string& error) const;

    /// The Arrow type the expression produces (after bind): a column's own type; int64 for integer
    /// arithmetic, float64 once a float is involved; utf8 for string functions; bool for predicates.
    /// `out` is a field named `name`.
    bool result_type(const std::string& name, ArrowSchema& out, std::string& error) const;

    /// The expression as SQL (normalized; for messages and tests).
    std::string to_string() const;

    bool empty() const { return root_ == nullptr; }

private:
    std::unique_ptr<Node> root_;
    struct Binding;
    std::unique_ptr<Binding> binding_;
};

}  // namespace nano_lance::expr
