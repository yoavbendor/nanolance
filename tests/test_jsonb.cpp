// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// JSON as Lance stores it (include/nanolance/jsonb.hpp): text is encoded to JSONB and printed back as Lance
// prints it (compact, object keys in byte order, shortest float form). Round trips for objects, arrays,
// numbers, strings with escapes, and null/true/false; malformed text and duplicate keys are errors; Item
// navigates a value without decoding it.

#include "nanolance/jsonb.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

namespace jb = nano_lance::jsonb;

// Encodes `text`, then prints the JSONB back to text. Returns false (with the error) if either step fails.
bool round_trip(std::string_view text, std::string& printed, std::string& error) {
    std::string bytes;
    if (!jb::encode(text, bytes, error)) {
        return false;
    }
    return jb::to_text(bytes, printed, error);
}

void expect_text(std::string_view input, std::string_view expected) {
    std::string printed;
    std::string error;
    require(round_trip(input, printed, error), "encode and print '" + std::string(input) + "' (" + error + ")");
    require(printed == expected,
            "'" + std::string(input) + "' prints as '" + printed + "', expected '" + std::string(expected) + "'");
}

void expect_error(std::string_view input) {
    std::string bytes;
    std::string error;
    require(!jb::encode(input, bytes, error), "malformed '" + std::string(input) + "' is an error");
    require(!error.empty(), "the error for '" + std::string(input) + "' says what is wrong");
}

}  // namespace

int main() {
    int checks = 0;

    // Scalars: null, true, false, and numbers in their shortest form.
    expect_text("null", "null");
    expect_text("true", "true");
    expect_text("false", "false");
    expect_text("0", "0");
    expect_text("-7", "-7");
    expect_text("1.5", "1.5");
    expect_text("0.25", "0.25");
    expect_text("9007199254740993", "9007199254740993");
    checks += 8;

    // Strings, with escapes, come back as the same JSON text.
    expect_text("\"hello\"", "\"hello\"");
    expect_text("\"\"", "\"\"");
    expect_text("\"a\\nb\"", "\"a\\nb\"");
    expect_text("\"quote \\\" and backslash \\\\\"", "\"quote \\\" and backslash \\\\\"");
    expect_text("\"tab\\tend\"", "\"tab\\tend\"");
    checks += 5;

    // Arrays: compact, nested, empty.
    expect_text("[1,2,3]", "[1,2,3]");
    expect_text("[ 1 , 2 , 3 ]", "[1,2,3]");
    expect_text("[]", "[]");
    expect_text("[[1],{\"a\":null},[true,false]]", "[[1],{\"a\":null},[true,false]]");
    checks += 4;

    // Objects: keys come out in byte order, whatever order they went in; empty objects; nesting.
    expect_text("{\"b\":1,\"a\":2}", "{\"a\":2,\"b\":1}");
    expect_text("{}", "{}");
    expect_text("{ \"z\" : { \"y\" : [ 1.5 ] } , \"a\" : \"x\" }", "{\"a\":\"x\",\"z\":{\"y\":[1.5]}}");
    checks += 3;

    // The lenient input Lance's writer accepts (single quotes, unquoted keys) prints as strict JSON.
    expect_text("{a: 'x'}", "{\"a\":\"x\"}");
    checks += 1;

    // Malformed text is an error with a message; a duplicate key is refused.
    expect_error("");
    expect_error("[1,");
    expect_error("{");
    expect_error("[1 2]");
    expect_error("{\"a\":1,\"a\":2}");
    checks += 10;

    // Item navigation, without decoding the whole value: root, object members by key, array elements.
    {
        std::string bytes;
        std::string error;
        require(jb::encode("{\"name\":\"nano\",\"list\":[10,\"x\",null]}", bytes, error), "encode the object");

        jb::Item root;
        require(jb::Item::root(bytes, root), "the encoded object is a JSONB document");
        require(root.type() == jb::Item::Type::Object, "the root is an object");
        require(root.size() == 2, "the object has two members");

        jb::Item name;
        require(root.get("name", name) && name.type() == jb::Item::Type::String, "the member 'name' is a string");
        require(name.string() == "nano", "the string value is nano");
        jb::Item missing;
        require(!root.get("absent", missing), "a missing key is not found");

        jb::Item list;
        require(root.get("list", list) && list.type() == jb::Item::Type::Array, "the member 'list' is an array");
        require(list.size() == 3, "the array has three elements");
        jb::Item first;
        require(list.at(0, first) && first.type() == jb::Item::Type::Number, "element 0 is a number");
        jb::Number n;
        require(first.number(n) && n.as_double() == 10.0, "element 0 is 10");
        jb::Item second;
        require(list.at(1, second) && second.string() == "x", "element 1 is the string x");
        jb::Item third;
        require(list.at(2, third) && third.type() == jb::Item::Type::Null, "element 2 is null");
        jb::Item past;
        require(!list.at(3, past), "an index past the end is not found");

        std::string_view key;
        require(root.key_at(0, key) && key == "list", "the first key in byte order is 'list'");
        require(root.key_at(1, key) && key == "name", "the second key in byte order is 'name'");
        checks += 18;
    }

    // Item text: a value printed on its own.
    {
        std::string bytes;
        std::string error;
        require(jb::encode("{\"k\":[1,2]}", bytes, error), "encode for item text");
        jb::Item root;
        require(jb::Item::root(bytes, root), "root for item text");
        jb::Item list;
        require(root.get("k", list), "the member k");
        std::string text;
        require(list.text(text, error) && text == "[1,2]", "an array item prints as its own text");
        checks += 2;
    }

    // Bytes that are not JSONB are not a document.
    {
        jb::Item root;
        require(!jb::Item::root(std::string_view(), root), "an empty byte string is not a JSONB document");
        checks += 1;
    }

    std::cout << "jsonb: " << checks << " checks passed\n";
    return 0;
}
