// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Names and paths as UTF-8, on every platform.
//
// A name a user or a dataset gives (a tag, a branch, a dataset URI, a file name read back from a listing) is
// UTF-8 bytes in a std::string. On Windows, std::filesystem::path(std::string) and path::string() convert
// through the ANSI code page, not UTF-8, so "café" becomes mojibake or is refused. These two helpers are the
// one place that crosses that boundary: from_utf8 builds a path from UTF-8 (char8_t, standard C++20), and
// string_of / generic_of return a path's text as UTF-8. On Linux and macOS the native encoding is UTF-8
// already, so both are byte-for-byte the same as before.
//
// io:: takes UTF-8 path strings on every platform (it converts them to wide characters on Windows), so
// anything passed to it is string_of(path), never path.string().

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace nano_lance::utf8 {

/// A path from UTF-8 bytes.
[[nodiscard]] inline std::filesystem::path from_utf8(std::string_view s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

/// A path's text as UTF-8.
[[nodiscard]] inline std::string string_of(const std::filesystem::path& p) {
    const auto text = p.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// A path's text as UTF-8, with "/" separators (path::generic_string, as UTF-8).
[[nodiscard]] inline std::string generic_of(const std::filesystem::path& p) {
    const auto text = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

}  // namespace nano_lance::utf8
