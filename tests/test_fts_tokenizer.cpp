// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// nanolance's full-text analyzer against Lance's: tests/golden/fts_tokenizer/cases.txt holds lines
// and the tokens Lance 12 (LanceDB defaults) makes of them, as tools/fts_tables printed them.

#include "nanolance/fts_tokenizer.hpp"

#include <fstream>
#include <cstdint>
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
    while (std::getline(in, line)) {
        if (line.rfind("@@code ", 0) == 0) {
            // A section of code-analyzer cases: "@@code ops=1 split=1 preserve=1 numerics=1".
            const auto flag = [&line](const std::string& key) {
                const auto at = line.find(key);
                return at != std::string::npos && at + key.size() < line.size() && line[at + key.size()] == '1';
            };
            nano_lance::fts::AnalyzerParams code;
            code.base_tokenizer = "code";
            code.stem = false;
            code.remove_stop_words = false;
            code.index_operators = flag("ops=");
            code.split_identifiers = flag("split=");
            code.preserve_original = flag("preserve=");
            code.split_on_numerics = flag("numerics=");
            if (!analyzer.init(code, error)) {
                std::cerr << error << "\n";
                return 1;
            }
            continue;
        }
        if (line.rfind("@@ngram ", 0) == 0) {
            // A section of ngram cases: "@@ngram min=3 max=3 prefix=0", the ngram base tokenizer with stemming
            // and stop words off (as the Python binding resolves base_tokenizer='ngram').
            const auto number = [&line](const std::string& key) {
                const auto at = line.find(key);
                return at == std::string::npos ? 0U : static_cast<std::uint32_t>(std::stoul(line.substr(at + key.size())));
            };
            nano_lance::fts::AnalyzerParams ngram;
            ngram.base_tokenizer = "ngram";
            ngram.stem = false;
            ngram.remove_stop_words = false;
            ngram.min_ngram_length = number("min=");
            ngram.max_ngram_length = number("max=");
            ngram.prefix_only = number("prefix=") != 0U;
            if (!analyzer.init(ngram, error)) {
                std::cerr << error << "\n";
                return 1;
            }
            continue;
        }
        if (!std::getline(in, expected)) {
            break;
        }
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
    if (!analyzer.init({}, error)) {
        std::cerr << error << "\n";
        return 1;
    }
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
    // Lance's NgramTokenizer::new messages for a bad gram range.
    const std::pair<std::uint32_t, std::uint32_t> bad_ranges[] = {{0, 3}, {4, 3}};
    const char* bad_messages[] = {"min_gram must be greater than 0", "min_gram must not be greater than max_gram"};
    for (std::size_t i = 0; i < 2; ++i) {
        nano_lance::fts::AnalyzerParams bad;
        bad.base_tokenizer = "ngram";
        bad.min_ngram_length = bad_ranges[i].first;
        bad.max_ngram_length = bad_ranges[i].second;
        ++cases;
        if (analyzer.init(bad, error) || error != bad_messages[i]) {
            ++failed;
            std::cerr << "ngram range " << bad_ranges[i].first << ".." << bad_ranges[i].second << " gave: " << error << "\n";
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
