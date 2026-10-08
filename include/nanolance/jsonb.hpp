// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// JSON as Lance stores it: a column of logical type "json" holds JSONB (Databend's binary JSON,
/// the `jsonb` crate Lance links), one value per row, in a large-binary column marked
/// `ARROW:extension:name = lance.json`. Readers see it as `arrow.json` text.
///
/// encode() parses JSON text the way Lance's writer does (the crate's lenient parser: single quotes,
/// unquoted keys, NaN / Infinity, hex, a leading '+' or '.'; duplicate keys refused) and produces
/// the same bytes; to_text() prints JSONB as Lance prints it (compact, keys in byte order, floats in
/// shortest round-trip form). Item navigates a value without decoding it, for the json_* functions.
namespace nano_lance::jsonb {

bool encode(std::string_view text, std::string& out, std::string& error);
bool to_text(std::string_view jsonb, std::string& out, std::string& error);

struct Number {
    enum class Kind { Int, UInt, Float } kind = Kind::UInt;
    std::int64_t i = 0;
    std::uint64_t u = 0;
    double f = 0;
    double as_double() const { return kind == Kind::Int ? static_cast<double>(i) : kind == Kind::UInt ? static_cast<double>(u) : f; }
};

/// One JSONB value: a container (its own header and entries) or a scalar (its payload alone).
class Item {
public:
    enum class Type { Null, False, True, Number, String, Array, Object, Other };

    /// The top-level value of a JSONB document; false when the bytes are not JSONB.
    static bool root(std::string_view jsonb, Item& out);

    Type type() const { return type_; }
    /// Array: its elements; object: its keys.
    std::size_t size() const { return count_; }
    bool at(std::size_t index, Item& out) const;               // Array element
    bool get(std::string_view key, Item& out) const;            // Object member, by exact key
    bool key_at(std::size_t index, std::string_view& out) const;  // Object key
    bool value_at(std::size_t index, Item& out) const;          // Object value
    std::string_view string() const { return payload_; }        // String
    bool number(Number& out) const;                             // Number
    /// This value as a JSONB document of its own (a scalar wrapped in a scalar container).
    std::string document() const;
    /// This value as JSON text, as Lance prints it.
    bool text(std::string& out, std::string& error) const;

private:
    Type type_ = Type::Null;
    std::string_view payload_;  // scalar payload, or the whole container
    std::size_t count_ = 0;
    std::uint32_t tag_ = 0;     // the scalar's JEntry type, for document()
    bool entry(std::size_t index, std::size_t entries, Item& out) const;
};

/// A JSONPath as the json_* functions take it: `$`, `.key`, `['key']`, `[n]`, `[*]`, `.*`. The
/// values it selects, in document order.
bool select(const Item& root, std::string_view path, std::vector<Item>& out, std::string& error);

}  // namespace nano_lance::jsonb
