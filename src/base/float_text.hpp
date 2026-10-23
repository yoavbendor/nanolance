// Floating-point text in the form std::to_chars writes it, without std::to_chars itself. Apple's
// libc++ marks the floating-point to_chars and from_chars unavailable below macOS 13.3, and the
// macOS builds target older systems. The output matches std::to_chars: the shortest digits that
// read back as the same double, in fixed or scientific layout as the format asks, and for the
// general format the shorter of the two (fixed on a tie).
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace nano_lance::float_text {

enum class Format { General, Fixed, Scientific };

inline std::string to_text(double value, Format format = Format::General) {
    if (std::isnan(value)) {
        return std::signbit(value) ? "-nan" : "nan";
    }
    if (std::isinf(value)) {
        return value < 0 ? "-inf" : "inf";
    }
    const std::string sign = std::signbit(value) ? "-" : "";
    const double magnitude = std::fabs(value);
    if (magnitude == 0.0) {
        return sign + (format == Format::Scientific ? "0e+00" : "0");
    }
    // The fewest significant digits that read back as the same double. %.<p>e rounds to the nearest
    // decimal with p+1 digits, so if any decimal of that length round-trips, this one does.
    // Seventeen digits always round-trip, so the loop ends with a round-tripping string.
    char buf[64];
    for (int precision = 0; precision < 17; ++precision) {
        std::snprintf(buf, sizeof buf, "%.*e", precision, magnitude);
        if (std::strtod(buf, nullptr) == magnitude) {
            break;
        }
    }
    const std::string_view text(buf);
    const auto e_at = text.find('e');
    std::string digits;
    for (const char c : text.substr(0, e_at)) {
        if (c != '.') {
            digits += c;
        }
    }
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
    }
    const int exponent = std::atoi(buf + e_at + 1);

    // Fixed prints %f with just the decimals the shortest digits need. That is the exact value of
    // the double in the integer part (1e300 prints all its digits), as std::to_chars prints it.
    // At most 325 decimals (the smallest denormal), so 1024 bytes hold the longest fixed form.
    const int needed = static_cast<int>(digits.size()) - 1 - exponent;
    const int decimals = needed < 0 ? 0 : (needed > 325 ? 325 : needed);
    char fixed_buf[1024];
    std::snprintf(fixed_buf, sizeof fixed_buf, "%.*f", decimals, magnitude);
    const std::string fixed = fixed_buf;

    std::string scientific = digits.substr(0, 1);
    if (digits.size() > 1U) {
        scientific += "." + digits.substr(1);
    }
    char exp_buf[16];
    std::snprintf(exp_buf, sizeof exp_buf, "e%c%02d", exponent < 0 ? '-' : '+', exponent < 0 ? -exponent : exponent);
    scientific += exp_buf;

    switch (format) {
        case Format::Fixed: return sign + fixed;
        case Format::Scientific: return sign + scientific;
        case Format::General: break;
    }
    return sign + (fixed.size() <= scientific.size() ? fixed : scientific);
}

// Reads a decimal floating-point number from the start of the text, as std::from_chars does for
// the characters the callers have already scanned. Returns false when no number starts there.
inline bool parse(std::string_view text, double& out) {
    const std::string copy(text);
    char* end = nullptr;
    out = std::strtod(copy.c_str(), &end);
    return end != copy.c_str();
}

}  // namespace nano_lance::float_text
