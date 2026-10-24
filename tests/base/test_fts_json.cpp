// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// The small JSON reader and writer (base/fts_json.hpp) that an INVERTED index's params and full-text
// queries are read with: every value kind, objects keeping their key order, string escapes (\uXXXX and
// surrogate pairs as UTF-8), whitespace, and the refusals (trailing text, unterminated strings and
// containers, missing colons or commas, bad numbers and \u escapes, nesting deeper than 64). quote()
// writes a string that reads back as itself.

#include "base/fts_json.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

namespace json = nano_lance::fts::json;

json::Value parsed(const std::string& text) {
    json::Value v;
    require(json::parse(text, v), "parses: " + text);
    return v;
}

void refused(const std::string& text) {
    json::Value v;
    require(!json::parse(text, v), "refuses: " + text);
}

}  // namespace

int main() {
    // Scalars.
    require(parsed("null").kind == json::Value::Null, "null");
    require(parsed(" true ").kind == json::Value::Bool && parsed("true").b, "true");
    require(parsed("false").kind == json::Value::Bool && !parsed("false").b, "false");
    require(parsed("-12.5e1").kind == json::Value::Number && parsed("-12.5e1").n == -125.0, "a number");
    require(parsed("0").n == 0.0 && parsed("3").n == 3.0, "integers");
    require(parsed("\"abc\"").kind == json::Value::String && parsed("\"abc\"").s == "abc", "a string");

    // An object keeps its members in order; get() finds them by key.
    {
        const auto v = parsed(R"({"base_tokenizer": "simple", "k": 1.5, "lower_case": true, "z": null, "a": [1, 2]})");
        require(v.kind == json::Value::Object && v.fields.size() == 5, "five members");
        require(v.fields[0].first == "base_tokenizer" && v.fields[4].first == "a", "members in order");
        require(v.get("base_tokenizer") != nullptr && v.get("base_tokenizer")->s == "simple", "get a string");
        require(v.get("k")->n == 1.5, "get a number");
        require(v.get("lower_case")->b, "get a bool");
        require(v.get("z")->kind == json::Value::Null, "get a null");
        require(v.get("a")->items.size() == 2 && v.get("a")->items[1].n == 2.0, "get an array");
        require(v.get("missing") == nullptr, "a missing key is null");
    }
    require(parsed("{}").kind == json::Value::Object && parsed("{}").fields.empty(), "an empty object");
    require(parsed("[ ]").kind == json::Value::Array && parsed("[ ]").items.empty(), "an empty array");
    require(parsed("[[[]]]").items[0].items[0].kind == json::Value::Array, "nested arrays");

    // Escapes.
    require(parsed(R"("a\"b\\c\/d\n\t\r\b\f")").s == "a\"b\\c/d\n\t\r\b\f", "simple escapes");
    require(parsed(R"("\u0041\u00e9\u05d0")").s == "A\xC3\xA9\xD7\x90", "\\u escapes as UTF-8");
    require(parsed(R"("\ud83d\ude00")").s == "\xF0\x9F\x98\x80", "a surrogate pair is one code point");
    require(parsed("\"\xD7\xA9\xD7\x9C\"").s == "\xD7\xA9\xD7\x9C", "raw UTF-8 passes through");

    // Refusals.
    refused("");
    refused("   ");
    refused("nul");
    refused("1 2");
    refused("{} x");
    refused("\"open");
    refused("\"bad \\u12\"");
    refused("\"bad \\uZZZZ\"");
    refused("[1, 2");
    refused("[1 2]");
    refused("{\"a\" 1}");
    refused("{\"a\": 1,}");
    refused("{a: 1}");
    refused("1.2.3");
    refused("-");
    {
        std::string deep(70, '[');
        deep += std::string(70, ']');
        refused(deep);
        std::string ok(60, '[');
        ok += std::string(60, ']');
        require(parsed(ok).kind == json::Value::Array, "60 levels of nesting are fine");
    }

    // quote() escapes quotes, backslashes and control characters, and reads back as the same string.
    {
        const std::string s = std::string("he said \"hi\"\\ \x01\n\x1f end \xC3\xA9");
        const auto q = json::quote(s);
        require(q.front() == '"' && q.back() == '"', "quoted");
        require(q.find("\\u0001") != std::string::npos && q.find("\\u000a") != std::string::npos &&
                    q.find("\\u001f") != std::string::npos,
                "control characters as \\u00XX");
        require(parsed(q).s == s, "a quoted string reads back as itself");
        require(json::quote("") == "\"\"", "the empty string");
    }

    std::cout << "fts_json: all checks passed\n";
    return 0;
}
