// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/// Lance's lindera base tokenizer (`base_tokenizer = "lindera/<model>"`): Lindera 3.0's tokenizer over a
/// Lindera 3 dictionary directory, ported from its source. A model is a directory under the language
/// model home holding config.yml (else LINDERA_CONFIG_PATH names one): the segmenter's dictionary,
/// optional user dictionary (.csv or .bin), mode and keep_whitespace, and the mapping character filter.
namespace nano_lance::fts {

/// One token: its text (after the character filters) and Lindera's position (the token's index).
struct LinderaToken {
    std::string surface;
    std::uint32_t position = 0;
};

class Lindera {
public:
    /// The tokenizer for the model directory `dir` (Lance's LinderaBuilder::load + build), cached by its
    /// config. Null and `error` as Lance reports a missing directory or a config, dictionary or user
    /// dictionary that does not load; "not supported" for token filters and character filters other
    /// than mapping.
    static std::shared_ptr<const Lindera> load(const std::filesystem::path& dir, std::string& error);

    /// Lindera's Tokenizer::tokenize: the character filters, then the segmenter.
    void tokenize(std::string_view text, std::vector<LinderaToken>& out) const;

    struct Impl;

private:
    std::shared_ptr<const Impl> impl_;
};

}  // namespace nano_lance::fts
