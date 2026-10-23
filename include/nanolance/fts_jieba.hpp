// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// Lance's jieba base tokenizer (`base_tokenizer = "jieba/<model>"`): jieba-rs 0.10 in search mode with
/// its HMM, ported from its source token for token. A model is a directory under the language model home
/// holding dict.txt, or a config.json naming the main dictionary and user dictionaries.
namespace nano_lance::fts {

/// One token of a text: its bytes and, as jieba counts them, its characters.
struct JiebaToken {
    std::size_t byte_start = 0;
    std::size_t byte_end = 0;
    std::uint32_t char_start = 0;
    std::uint32_t char_end = 0;
};

class Jieba {
public:
    /// The model in `dir` (Lance's JiebaBuilder::load + build), cached by path. Null and `error` as Lance
    /// reports a missing directory, an unreadable or malformed dictionary.
    static std::shared_ptr<const Jieba> load(const std::filesystem::path& dir, std::string& error);

    /// jieba's tokenize(text, Search, hmm = true): every word in order, each preceded by the dictionary's
    /// 2- and 3-character words inside it.
    void tokenize_search(std::string_view text, std::vector<JiebaToken>& out) const;

    /// Adds a dictionary's lines (load_dict): a word, an optional frequency, an optional tag.
    bool load_dict(std::string_view text, std::string& error);

private:
    struct Record {
        std::uint64_t freq = 0;
        double log_freq = 0.0;
    };
    const Record* find(std::string_view word) const;
    void cut_block(std::string_view block, std::vector<std::string_view>& words) const;

    std::unordered_map<std::string, std::uint32_t> ids_;
    std::vector<Record> records_;
    std::uint64_t total_ = 0;
    std::size_t max_word_chars_ = 0;
};

/// LANCE_LANGUAGE_MODEL_HOME, else the platform's local data directory + "lance/language_models"
/// (Lance's language_model_home). Empty when neither is known.
std::filesystem::path language_model_home();

/// Whether this build has the jieba tokenizer (its built-in HMM model is compiled in).
bool jieba_available();

}  // namespace nano_lance::fts
