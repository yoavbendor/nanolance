// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The jieba tokenizer (nanolance/fts_jieba.hpp) with a model the test writes itself (a dict.txt in a
// temporary directory, as bindings/python/tests/test_fts_jieba.py does; no downloaded model): its
// search-mode tokens are byte and character spans of the text, a dictionary word comes out whole and
// preceded by its dictionary sub-words; a config.json with a user dictionary adds words; a missing
// directory, a malformed frequency and a bad config are refused. language_model_home follows
// LANCE_LANGUAGE_MODEL_HOME.

#include "nanolance/fts_jieba.hpp"

#include "index_test_util.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using index_test::require;
namespace fts = nano_lance::fts;

namespace {

void write(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

std::vector<std::string> words(const fts::Jieba& j, const std::string& text) {
    std::vector<fts::JiebaToken> tokens;
    j.tokenize_search(text, tokens);
    std::vector<std::string> out;
    for (const auto& t : tokens) {
        require(t.byte_start < t.byte_end && t.byte_end <= text.size(), "a token is a non-empty byte span");
        require(t.char_start < t.char_end, "a token is a non-empty character span");
        out.push_back(text.substr(t.byte_start, t.byte_end - t.byte_start));
    }
    return out;
}

bool has(const std::vector<std::string>& v, const std::string& w) {
    for (const auto& x : v) {
        if (x == w) {
            return true;
        }
    }
    return false;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) {
        s += "[" + x + "]";
    }
    return s;
}

}  // namespace

int main() {
    require(fts::jieba_available(), "this build has jieba's HMM model");
    const auto root = index_test::scratch("nano_lance_fts_jieba_test");
    const auto model = root / "jieba" / "small";
    std::filesystem::create_directories(model);
    write(model / "dict.txt",
          "中华人民共和国 100 ns\n中华 50 nz\n人民 800 n\n共和国 300 n\n北京 900 ns\n大学 700 n\n"
          "北京大学 400 nt\n学生 600 n\n的 5000 uj\n");

    std::string error;
    const auto jieba = fts::Jieba::load(model, error);
    require(jieba != nullptr, "the model loads: " + error);
    require(fts::Jieba::load(model, error) == jieba, "a loaded model is cached by path");

    {
        const auto w = words(*jieba, "中华人民共和国");
        require(!w.empty() && w.back() == "中华人民共和国", "the whole word comes last: " + joined(w));
        require(has(w, "中华") && has(w, "人民") && has(w, "共和国"), "preceded by its sub-words: " + joined(w));
    }
    {
        const std::string text = "北京大学的学生";
        const auto w = words(*jieba, text);
        require(has(w, "北京大学") && has(w, "北京") && has(w, "大学") && has(w, "学生"),
                "dictionary words found: " + joined(w));
        std::vector<fts::JiebaToken> tokens;
        jieba->tokenize_search(text, tokens);
        require(tokens.back().char_end == 7U && tokens.back().byte_end == text.size(),
                "the last token ends at the text's end, in characters and bytes");
    }
    {
        const auto w = words(*jieba, "abc 123，北京");
        require(has(w, "abc") && has(w, "123") && has(w, "北京"), "ASCII runs and words: " + joined(w));
    }
    {
        std::vector<fts::JiebaToken> none;
        jieba->tokenize_search("", none);
        require(none.empty(), "no text, no tokens");
    }

    // A user dictionary through config.json.
    {
        const auto custom = root / "jieba" / "custom";
        std::filesystem::create_directories(custom);
        write(custom / "main.txt", "北京 900 ns\n");
        write(custom / "user.txt", "向量检索 5000 n\n");
        write(custom / "config.json", R"({"main": "main.txt", "users": ["user.txt"]})");
        const auto j = fts::Jieba::load(custom, error);
        require(j != nullptr, "the configured model loads: " + error);
        const auto w = words(*j, "北京向量检索");
        require(has(w, "向量检索"), "the user dictionary's word is one token: " + joined(w));
    }

    // Refusals.
    require(fts::Jieba::load(root / "jieba" / "missing", error) == nullptr && !error.empty(),
            "a missing model directory is refused");
    {
        const auto bad = root / "jieba" / "bad";
        std::filesystem::create_directories(bad);
        write(bad / "dict.txt", "北京 notanumber\n");
        require(fts::Jieba::load(bad, error) == nullptr && error.find("frequency") != std::string::npos,
                "a malformed frequency is refused: " + error);
        const auto badcfg = root / "jieba" / "badcfg";
        std::filesystem::create_directories(badcfg);
        write(badcfg / "config.json", "[1, 2]");
        require(fts::Jieba::load(badcfg, error) == nullptr, "a config that is not a model configuration is refused");
    }
    {
        fts::Jieba j;
        require(j.load_dict("自然语言 10\n处理\n", error), "load_dict takes a word with or without a frequency: " + error);
    }

    // The model home.
    ::setenv("LANCE_LANGUAGE_MODEL_HOME", root.string().c_str(), 1);
    require(fts::language_model_home() == root, "LANCE_LANGUAGE_MODEL_HOME is the model home");

    std::filesystem::remove_all(root);
    std::cout << "fts_jieba: ok (search-mode tokens, user dictionaries, refusals)\n";
    return 0;
}
