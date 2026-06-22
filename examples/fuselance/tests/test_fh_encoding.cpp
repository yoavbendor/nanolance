// Unit tests for fuselance file-handle encode/decode round-trips.
// Root handles store the file index in the low 32 bits (bit 63 clear).
// Framed handles: bit 63=1 | bits[47:32]=file_idx (uint16) | bits[31:0]=frame_idx (uint32).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

static constexpr uint64_t kFramedBit = (uint64_t{1} << 63);

static uint64_t encode_root_fh(size_t file_idx) {
    return static_cast<uint64_t>(file_idx);
}
static uint64_t encode_frame_fh(size_t frame_idx, size_t file_idx) {
    return kFramedBit | (static_cast<uint64_t>(file_idx) << 32) | static_cast<uint64_t>(frame_idx);
}
static bool   is_framed_fh(uint64_t fh)           { return (fh & kFramedBit) != 0; }
static size_t frame_idx_from_fh(uint64_t fh)       { return static_cast<size_t>(fh & 0xFFFF'FFFF); }
static size_t framed_file_idx_from_fh(uint64_t fh) { return static_cast<size_t>((fh >> 32) & 0xFFFF); }
static size_t root_file_idx_from_fh(uint64_t fh)   { return static_cast<size_t>(fh & 0xFFFFFFFFULL); }

int main() {
    // ── Root handles ──────────────────────────────────────────────────────────
    // The regression: root_file_idx_from_fh must use low bits, not high bits.
    for (size_t i : {0u, 1u, 5u, 7u, 255u, 65535u}) {
        uint64_t fh = encode_root_fh(i);
        assert(!is_framed_fh(fh));
        assert(root_file_idx_from_fh(fh) == i);
        // Verify the old buggy decode (>> 32) would have returned 0 for all of these
        assert(((fh >> 32) & 0xFFFF) == 0);   // demonstrates the original bug
    }

    // ── Framed handles ────────────────────────────────────────────────────────
    {
        uint64_t fh = encode_frame_fh(3, 5);
        assert(is_framed_fh(fh));
        assert(frame_idx_from_fh(fh) == 3);
        assert(framed_file_idx_from_fh(fh) == 5);
    }
    {
        uint64_t fh = encode_frame_fh(0, 0);
        assert(is_framed_fh(fh));
        assert(frame_idx_from_fh(fh) == 0);
        assert(framed_file_idx_from_fh(fh) == 0);
    }
    {
        // Max frame index (uint32) and max file index (uint16)
        uint64_t fh = encode_frame_fh(0xFFFFFFFFu, 0xFFFFu);
        assert(is_framed_fh(fh));
        assert(frame_idx_from_fh(fh) == 0xFFFF'FFFFu);
        assert(framed_file_idx_from_fh(fh) == 0xFFFFu);
    }
    {
        uint64_t fh = encode_frame_fh(42, 7);
        assert(is_framed_fh(fh));
        assert(frame_idx_from_fh(fh) == 42);
        assert(framed_file_idx_from_fh(fh) == 7);
    }

    // ── Root and framed handles never collide ─────────────────────────────────
    for (size_t i = 0; i < 1000; ++i) {
        assert(!is_framed_fh(encode_root_fh(i)));
        assert( is_framed_fh(encode_frame_fh(i, i)));
    }

    std::puts("fh encoding: all tests passed");
    return 0;
}
