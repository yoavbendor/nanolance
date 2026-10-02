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
///                   contains, is_null, is_valid, CAST(x AS type); on a list column,
///                   array_has_any(c, ['a', 'b']), array_has_all(c, [...]), array_contains(c, 'a')
///                   (array_has, list_has, list_has_any, list_has_all likewise)
///
/// A column compared with a string or DATE / TIMESTAMP literal compares as the column's type, as
/// DataFusion coerces it. What the dialect does not cover is refused with an error naming it, never
/// evaluated differently.
namespace nano_lance::expr {

struct Node;

class Predicate;

/// A filter's shape as an index sees it: AND, OR and NOT over predicates, each a test of one
/// column against constants. Any other part of the filter is `Other`, which no index answers.
struct Condition {
    enum class Kind { And, Or, Not, Predicate, Other };
    Kind kind = Kind::Other;
    std::vector<Condition> children;              // And, Or (two), Not (one)
    std::shared_ptr<const Predicate> predicate;  // Predicate
};

/// A test of one column against constants: `x < 5`, `x IN (1, 2)`, `x BETWEEN 1 AND 9`,
/// `x IS NULL`, `array_has_any(tags, ['a', 'b'])`, ... Evaluated with the filter's own semantics, on
/// any values of the column's type -- an index's keys, a page's bounds.
class Predicate {
public:
    enum class Test { Compare, In, Between, IsNull, IsNotNull, HasAny, HasAll, Has };

    Test test() const { return test_; }
    /// The column, as written ("s.child" is {"s", "child"}).
    const std::vector<std::string>& column() const { return column_; }
    /// Compare: "=", "!=", "<", "<=", ">", ">=", the column on the left (`5 > x` is `x < 5`).
    const std::string& op() const { return op_; }
    /// The constants: In's items, Between's two bounds, Compare's and Has's one, and the items of
    /// HasAny's and HasAll's list.
    std::size_t constants() const { return constants_.size(); }

    /// `column <op> constant k`, a Compare.
    Predicate compare_with(const std::string& op, std::size_t k) const;

    /// Evaluate on `values`, of the column's type -- for HasAny, HasAll and Has, of its elements'
    /// type, and then TRUE for an element the test names. `pass[i]` is 1 where the test is TRUE.
    bool filter(const ArrowSchema& type, const ArrowArray& values, std::vector<std::uint8_t>& pass,
                std::string& error) const;

    std::string to_string() const;

private:
    friend class Expression;
    Test test_ = Test::Compare;
    std::vector<std::string> column_;
    std::string op_;
    std::vector<std::shared_ptr<const Node>> constants_;
};

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

    /// The expression's predicates an index could answer, in its AND / OR / NOT structure.
    Condition conditions() const;

    bool empty() const { return root_ == nullptr; }

private:
    friend class Predicate;
    std::unique_ptr<Node> root_;
    struct Binding;
    std::unique_ptr<Binding> binding_;
};

}  // namespace nano_lance::expr
