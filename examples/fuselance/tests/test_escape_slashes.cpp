// Unit tests for the escape_slashes() helper used in fuselance.
// Column values that contain '/' would be misinterpreted as FUSE subdirectory
// separators; escape_slashes() replaces each '/' with "%2F".
#include <cassert>
#include <cstdio>
#include <string>

static std::string escape_slashes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '/') out += "%2F";
        else          out += static_cast<char>(c);
    }
    return out;
}

int main() {
    // Paths from /proc and /dev — the motivating bug
    assert(escape_slashes("/proc/info/TempDiagnostics.log") == "%2Fproc%2Finfo%2FTempDiagnostics.log");
    assert(escape_slashes("/dev/measurements/video/parking_rear/ImageInfo.raw") ==
           "%2Fdev%2Fmeasurements%2Fvideo%2Fparking_rear%2FImageInfo.raw");
    assert(escape_slashes("/dev/measurements/b2b_metrics/b2b_metric_reporter_prev_frame_data.raw") ==
           "%2Fdev%2Fmeasurements%2Fb2b_metrics%2Fb2b_metric_reporter_prev_frame_data.raw");

    // No slash — must be unchanged
    assert(escape_slashes("std.err")      == "std.err");
    assert(escape_slashes("gsf-profile.raw") == "gsf-profile.raw");
    assert(escape_slashes("")             == "");

    // Single leading slash
    assert(escape_slashes("/proc/app/devices.raw") == "%2Fproc%2Fapp%2Fdevices.raw");

    // %2F itself in input — must not be double-encoded (it's just literal chars)
    assert(escape_slashes("a%2Fb") == "a%2Fb");

    std::puts("escape_slashes: all tests passed");
    return 0;
}
