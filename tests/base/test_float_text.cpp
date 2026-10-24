// float_text::to_text must match std::to_chars byte for byte, in every format. Apple's libc++ has
// no floating-point std::to_chars below macOS 13.3, so the comparison runs only where it exists;
// the fixed expectations run everywhere.

#include "base/float_text.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#if !defined(__APPLE__)
#include <charconv>
#endif

namespace ft = nano_lance::float_text;

namespace {

int failures = 0;

void expect(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        std::fprintf(stderr, "%s: got '%s', want '%s'\n", what, got.c_str(), want.c_str());
        ++failures;
    }
}

void expect_true(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "%s\n", what);
        ++failures;
    }
}

void known_values() {
    expect(ft::to_text(1.0), "1", "1.0");
    expect(ft::to_text(100.0), "100", "100.0");
    expect(ft::to_text(0.1), "0.1", "0.1");
    expect(ft::to_text(0.001), "0.001", "0.001 (fixed on a tie)");
    expect(ft::to_text(1e15), "1e+15", "1e15");
    expect(ft::to_text(1e20), "1e+20", "1e20");
    expect(ft::to_text(1e-5), "1e-05", "1e-5 general");
    expect(ft::to_text(1e-5, ft::Format::Fixed), "0.00001", "1e-5 fixed");
    expect(ft::to_text(0.5, ft::Format::Fixed), "0.5", "0.5 fixed");
    expect(ft::to_text(-2.5), "-2.5", "-2.5");
    expect(ft::to_text(-0.0), "-0", "-0.0");
    expect(ft::to_text(0.0, ft::Format::Scientific), "0e+00", "0 scientific");
    expect(ft::to_text(1.5e300, ft::Format::Scientific), "1.5e+300", "1.5e300 scientific");
    expect(ft::to_text(std::numeric_limits<double>::quiet_NaN()), "nan", "nan");
    expect(ft::to_text(std::numeric_limits<double>::infinity()), "inf", "inf");
    expect(ft::to_text(-std::numeric_limits<double>::infinity()), "-inf", "-inf");

    double v = 0;
    expect_true(ft::parse("1.25", v) && v == 1.25, "parse 1.25");
    expect_true(ft::parse("1e3", v) && v == 1000.0, "parse 1e3");
    expect_true(!ft::parse("", v), "parse of empty text must fail");
}

#if !defined(__APPLE__)
std::string reference(double v, ft::Format format) {
    char buf[512];
    std::to_chars_result r{};
    switch (format) {
        case ft::Format::General: r = std::to_chars(buf, buf + sizeof buf, v); break;
        case ft::Format::Fixed: r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed); break;
        case ft::Format::Scientific: r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific); break;
    }
    return std::string(buf, r.ptr);
}

void compare(double v) {
    for (const auto format : {ft::Format::General, ft::Format::Fixed, ft::Format::Scientific}) {
        const std::string want = reference(v, format);
        const std::string got = ft::to_text(v, format);
        if (got != want) {
            std::fprintf(stderr, "to_text(%.17g) format %d: got '%s', std::to_chars '%s'\n", v,
                         static_cast<int>(format), got.c_str(), want.c_str());
            ++failures;
            return;
        }
    }
}

void compare_with_std() {
    const double edges[] = {0.0,
                            -0.0,
                            1.0,
                            2.5,
                            100.0,
                            1e15,
                            1e16,
                            1e20,
                            0.1,
                            1e-5,
                            1.5e-5,
                            123456.789,
                            std::numeric_limits<double>::max(),
                            std::numeric_limits<double>::min(),
                            std::numeric_limits<double>::denorm_min(),
                            std::numeric_limits<double>::epsilon()};
    for (const double v : edges) {
        compare(v);
        compare(-v);
    }
    std::mt19937_64 rng(20261018);
    for (int i = 0; i < 200000; ++i) {
        const std::uint64_t bits = rng();
        double v = 0;
        std::memcpy(&v, &bits, sizeof v);
        if (std::isfinite(v)) {
            compare(v);
        }
    }
    // Values in the range the writers see: small integers, probabilities, and their short decimals.
    for (int i = 1; i <= 100000; ++i) {
        compare(i / 1000.0);
        compare(static_cast<double>(i));
    }
}
#endif

}  // namespace

int main() {
    known_values();
#if !defined(__APPLE__)
    compare_with_std();
#endif
    if (failures != 0) {
        std::fprintf(stderr, "%d float_text failures\n", failures);
        return 1;
    }
    std::printf("float_text: ok\n");
    return 0;
}
