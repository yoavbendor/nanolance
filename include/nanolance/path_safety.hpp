// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Path-safety substrate for the nanolance READ path.
//
// A Lance dataset's manifest and fragment records carry file *paths* (a data file's name, an external
// blob's URI). Those bytes are untrusted — a hostile or corrupt dataset can put "../../etc/passwd", an
// absolute path, or a symlink-style escape where the reader expects a dataset-relative name. Joining
// such a value naively (`dataset / "data" / p`) lets the file leak: `std::filesystem::path::operator/`
// *replaces* the left side when the right side is absolute, and a ".." component walks out of the tree.
//
// This header centralizes the "confine an untrusted relative path under a trusted base" check so every
// place that opens a manifest-derived path uses the same jail.

#pragma once

#include "nanolance/byte_source.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>

namespace nano_lance {

// Confine an untrusted, dataset-relative path under `base`.
//
// Returns base/relative (lexically normalized) IFF `relative` is a safe subpath — non-empty, not
// absolute, and containing no root or ".." component that would escape — AND the normalized join still
// lies within `base`. Returns std::nullopt for anything that could point outside the tree.
//
// This is a *lexical* check (no filesystem access, no symlink resolution): it is cheap, deterministic,
// and runs before we ever touch the path. The writer only ever stores a bare filename here, so
// legitimate datasets pass unconditionally; the jail exists purely to reject hostile inputs.
[[nodiscard]] inline std::optional<std::filesystem::path> safe_join_under(
    const std::filesystem::path& base, const std::filesystem::path& relative) {
    if (relative.empty() || relative.is_absolute()) {
        return std::nullopt;
    }
    // Reject any component that carries a root (e.g. a Windows "C:") or walks upward. A leading "." is
    // harmless and normalized away; "" (from a trailing separator) is likewise inert.
    for (const auto& part : relative) {
        if (part == "..") {
            return std::nullopt;
        }
        if (part.has_root_name() || part.has_root_directory()) {
            return std::nullopt;
        }
        // A backslash separates components on Windows, so a name carrying one would walk a path there.
        // Refused on every platform, so a dataset means the same thing wherever it is read.
        const std::string name = part.string();
        if (name.find('\\') != std::string::npos) {
            return std::nullopt;
        }
#ifdef _WIN32
        // A drive-relative name ("C:x"), an NTFS alternate data stream ("a:stream") or a reserved device
        // name ("CON", "nul.txt"): each opens something other than the file the name says.
        if (name.find(':') != std::string::npos) {
            return std::nullopt;
        }
        std::string stem = name.substr(0, name.find('.'));
        while (!stem.empty() && stem.back() == ' ') {
            stem.pop_back();
        }
        for (auto& c : stem) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        static constexpr std::array<const char*, 22> reserved{
            "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
        for (const char* device : reserved) {
            if (stem == device) {
                return std::nullopt;
            }
        }
#endif
    }

    if (io::is_remote(base.string())) {
        // A URI is not a file-system path: lexical normalization would collapse its "//" (mem:///x to
        // mem:/x). The components were checked above, so the URI is only joined, with "/" whatever the
        // platform's separator: object keys are "/"-separated, and std::filesystem would add a "\" on Windows.
        // (string(), not generic_string(): the latter rebuilds the path from its elements and drops the "//".)
        std::string joined = base.string();
#ifdef _WIN32
        std::replace(joined.begin(), joined.end(), '\\', '/');
#endif
        for (const auto& part : relative) {
            if (part == ".") {
                continue;
            }
            if (joined.empty() || joined.back() != '/') {
                joined += '/';
            }
            joined += part.string();
        }
        return std::filesystem::path(joined);
    }
    const std::filesystem::path base_norm = base.lexically_normal();
    const std::filesystem::path joined = (base_norm / relative).lexically_normal();

    // Defense in depth: even after the component scan, confirm the normalized result stays under base.
    // lexically_relative yields a path that starts with ".." (or is empty) when `joined` escapes `base`.
    const std::filesystem::path rel = joined.lexically_relative(base_norm);
    if (rel.empty() || *rel.begin() == "..") {
        return std::nullopt;
    }
    return joined;
}

}  // namespace nano_lance
