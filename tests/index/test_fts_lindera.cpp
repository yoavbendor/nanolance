// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// The lindera tokenizer's loading (nanolance/fts_lindera.hpp) without a dictionary: a Lindera 3 dictionary
// is binary (rkyv archives, a daachorse automaton), so tokenizing with one is left to
// bindings/python/tests/test_fts_lindera.py, which writes them (tests/lindera_dict.py) and compares both
// engines. Here, every refusal before a dictionary is read, with Lance's messages: a model path that is
// not a directory, a config.yml that is not YAML or not a mapping, no dictionary field, a dictionary path
// that does not exist or is a file, a dictionary whose metadata.json is not Lindera's, and a config read
// through LINDERA_CONFIG_PATH.

#include "nanolance/fts_lindera.hpp"

#include "index_test_util.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using index_test::require;
namespace fts = nano_lance::fts;

namespace {

void write(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

bool refused(const std::filesystem::path& dir, const std::string& part, std::string& error) {
    return fts::Lindera::load(dir, error) == nullptr && error.find(part) != std::string::npos;
}

std::filesystem::path model(const std::filesystem::path& root, const std::string& name, const std::string& config) {
    const auto d = root / "lindera" / name;
    std::filesystem::create_directories(d);
    write(d / "config.yml", config);
    return d;
}

}  // namespace

int main() {
    ::unsetenv("LINDERA_CONFIG_PATH");
    const auto root = index_test::scratch("nano_lance_fts_lindera_test");
    std::filesystem::create_directories(root);
    std::string error;

    require(refused(root / "lindera" / "missing", "Invalid directory path", error),
            "a missing model directory: " + error);

    // No config.yml and no LINDERA_CONFIG_PATH: the default tokenizer, which names no dictionary.
    {
        const auto d = root / "lindera" / "empty";
        std::filesystem::create_directories(d);
        require(refused(d, "dictionary field is missing", error), "no config: " + error);
        require(error.find("Failed to initialize default tokenizer") != std::string::npos, "the default's message");
    }

    require(refused(model(root, "noyaml", "segmenter: [unclosed\n"), "Failed to load tokenizer config", error),
            "malformed YAML: " + error);
    require(refused(model(root, "scalar", "just a string\n"), "Failed to load tokenizer config", error),
            "a config that is not a mapping: " + error);
    require(refused(model(root, "nodict", "segmenter:\n  mode: normal\n"), "dictionary field is missing", error),
            "no dictionary field: " + error);
    require(refused(model(root, "nopath", "segmenter:\n  dictionary: \"" + (root / "nowhere").string() + "\"\n"),
                    "Dictionary path does not exist", error),
            "a dictionary path that does not exist: " + error);
    write(root / "afile", "x");
    require(refused(model(root, "file", "segmenter:\n  dictionary: \"" + (root / "afile").string() + "\"\n"),
                    "is not a directory", error),
            "a dictionary path that is a file: " + error);

    // A dictionary directory whose metadata.json is not Lindera's.
    {
        const auto dict = root / "baddict";
        std::filesystem::create_directories(dict);
        write(dict / "metadata.json", "{\"name\": \"not lindera\"}");
        for (const char* f : {"char_def.bin", "matrix.mtx", "dict.da", "dict.vals", "unk.bin", "dict.wordsidx",
                              "dict.words"}) {
            write(dict / f, "");
        }
        require(refused(model(root, "meta", "segmenter:\n  dictionary: \"" + dict.string() + "\"\n"),
                        "Failed to deserialize metadata", error),
                "a malformed metadata.json: " + error);
    }

    // Through LINDERA_CONFIG_PATH when the model has no config.yml of its own.
    {
        const auto d = root / "lindera" / "viaenv";
        std::filesystem::create_directories(d);
        write(root / "env.yml", "segmenter:\n  mode: normal\n");
        ::setenv("LINDERA_CONFIG_PATH", (root / "env.yml").string().c_str(), 1);
        require(refused(d, "dictionary field is missing", error), "the env config is read: " + error);
        ::unsetenv("LINDERA_CONFIG_PATH");
    }

    std::filesystem::remove_all(root);
    std::cout << "fts_lindera: ok (config and dictionary refusals)\n";
    return 0;
}
