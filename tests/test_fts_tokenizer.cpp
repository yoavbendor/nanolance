// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// nanolance's full-text analyzer against Lance's: tests/golden/fts_tokenizer/cases.txt holds lines
// and the tokens Lance 12 (LanceDB defaults) makes of them, as tools/fts_tables printed them.

#include "nanolance/fts_tokenizer.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: test_fts_tokenizer <cases.txt>\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 2;
    }
    nano_lance::fts::Analyzer analyzer;
    std::string error;
    if (!analyzer.init({}, error)) {
        std::cerr << error << "\n";
        return 1;
    }
    std::string line;
    std::getline(in, line);  // the header
    std::string expected;
    std::size_t cases = 0;
    std::size_t failed = 0;
    std::vector<nano_lance::fts::Token> tokens;
    while (std::getline(in, line) && std::getline(in, expected)) {
        tokens.clear();
        analyzer.tokenize(line, tokens);
        std::string got;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            got += (i == 0 ? "" : " ") + tokens[i].text + "@" + std::to_string(tokens[i].position);
        }
        ++cases;
        if (got != expected) {
            if (++failed <= 5) {
                std::cerr << "input:    " << line << "\nexpected: " << expected << "\ngot:      " << got << "\n";
            }
        }
    }
    // A few by hand, and the params Lance writes.
    const std::pair<const char*, const char*> by_hand[] = {
        {"The quick brown fox jumps over the lazy dog's back", "quick@1 brown@2 fox@3 jump@4 lazi@7 dog@8 back@10"},
        {"Café crème brûlée is DELICIOUS", "cafe@0 creme@1 brulee@2 delici@4"},
        {"running runners run quickly", "run@0 runner@1 run@2 quick@3"},
        {"", ""},
    };
    for (const auto& [text, want] : by_hand) {
        tokens.clear();
        analyzer.tokenize(text, tokens);
        std::string got;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            got += (i == 0 ? "" : " ") + tokens[i].text + "@" + std::to_string(tokens[i].position);
        }
        ++cases;
        if (got != want) {
            ++failed;
            std::cerr << "input:    " << text << "\nexpected: " << want << "\ngot:      " << got << "\n";
        }
    }
    nano_lance::fts::AnalyzerParams parsed;
    const std::string lance_json =
        "{\"lance_tokenizer\":\"text\",\"base_tokenizer\":\"simple\",\"language\":\"English\",\"with_position\":false,"
        "\"max_token_length\":40,\"lower_case\":true,\"stem\":true,\"remove_stop_words\":true,\"custom_stop_words\":null,"
        "\"ascii_folding\":true,\"min_ngram_length\":3,\"max_ngram_length\":3,\"prefix_only\":false,\"block_size\":128,"
        "\"split_identifiers\":false,\"split_on_numerics\":false,\"preserve_original\":false,\"index_operators\":false}";
    if (!nano_lance::fts::parse_params(lance_json, parsed, error) || nano_lance::fts::params_json(parsed) != lance_json) {
        ++failed;
        std::cerr << "params round trip: " << error << "\n" << nano_lance::fts::params_json(parsed) << "\n";
    }
    std::cout << cases - failed << " of " << cases << " cases tokenized as Lance does\n";
    return failed == 0 ? 0 : 1;
}
