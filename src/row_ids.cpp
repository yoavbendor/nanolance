// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/row_ids.hpp"

#include "nanolance/deletion_vector.hpp"
#include "nanolance/manifest_reader.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_set>

namespace nano_lance {

namespace {

// ── protobuf wire helpers (the messages here are tiny and fixed) ─────────────────────────────────

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_key(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint32_t wire) {
    put_varint(out, (static_cast<std::uint64_t>(field) << 3U) | wire);
}

void put_bytes_field(std::vector<std::uint8_t>& out, std::uint32_t field, const std::vector<std::uint8_t>& bytes) {
    put_key(out, field, 2);
    put_varint(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void put_u64_field(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint64_t v) {
    if (v == 0) {
        return;  // proto3 default
    }
    put_key(out, field, 0);
    put_varint(out, v);
}

struct Field {
    std::uint32_t number = 0;
    std::uint32_t wire = 0;
    std::uint64_t value = 0;                  // wire 0
    const std::uint8_t* data = nullptr;       // wire 2
    std::size_t size = 0;
    const std::uint8_t* begin = nullptr;      // the whole field, key included
    const std::uint8_t* end = nullptr;
};

/// Iterates the fields of one message.
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : at_(data), end_(data + size) {}

    /// Next field; false at the end. `ok()` tells a clean end from a malformed message.
    bool next(Field& f) {
        if (at_ >= end_ || !ok_) {
            return false;
        }
        f.begin = at_;
        std::uint64_t key = 0;
        if (!varint(key)) {
            return false;
        }
        f.number = static_cast<std::uint32_t>(key >> 3U);
        f.wire = static_cast<std::uint32_t>(key & 7U);
        switch (f.wire) {
            case 0:
                if (!varint(f.value)) {
                    return false;
                }
                break;
            case 1:
                if (end_ - at_ < 8) {
                    return fail();
                }
                f.data = at_;
                f.size = 8;
                at_ += 8;
                break;
            case 2: {
                std::uint64_t n = 0;
                if (!varint(n) || n > static_cast<std::uint64_t>(end_ - at_)) {
                    return fail();
                }
                f.data = at_;
                f.size = static_cast<std::size_t>(n);
                at_ += n;
                break;
            }
            case 5:
                if (end_ - at_ < 4) {
                    return fail();
                }
                f.data = at_;
                f.size = 4;
                at_ += 4;
                break;
            default:
                return fail();
        }
        f.end = at_;
        return true;
    }
    bool ok() const { return ok_; }

private:
    bool fail() {
        ok_ = false;
        return false;
    }
    bool varint(std::uint64_t& out) {
        out = 0;
        for (unsigned shift = 0; shift < 70; shift += 7) {
            if (at_ >= end_) {
                return fail();
            }
            const std::uint8_t b = *at_++;
            out |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
            if ((b & 0x80U) == 0U) {
                return true;
            }
        }
        return fail();
    }
    const std::uint8_t* at_;
    const std::uint8_t* end_;
    bool ok_ = true;
};

// ── EncodedU64Array ─────────────────────────────────────────────────────────────────────────────

bool decode_encoded_array(const std::uint8_t* data, std::size_t size, std::vector<std::uint64_t>& out,
                          std::string& error) {
    Reader r(data, size);
    Field f;
    bool seen = false;
    while (r.next(f)) {
        if (f.wire != 2 || f.number < 1 || f.number > 3) {
            continue;
        }
        seen = true;
        Reader inner(f.data, f.size);
        Field g;
        std::uint64_t base = 0;
        const std::uint8_t* offsets = nullptr;
        std::size_t offsets_size = 0;
        while (inner.next(g)) {
            if (g.number == 1 && g.wire == 0) {
                base = g.value;
            } else if (g.number == 2 && g.wire == 2) {
                offsets = g.data;
                offsets_size = g.size;
            }
        }
        if (!inner.ok()) {
            error = "malformed row id array";
            return false;
        }
        const std::size_t width = f.number == 1 ? 2 : (f.number == 2 ? 4 : 8);
        if (offsets_size % width != 0U) {
            error = "row id array length is not a multiple of its element width";
            return false;
        }
        const std::size_t n = offsets_size / width;
        out.reserve(out.size() + n);
        for (std::size_t i = 0; i < n; ++i) {
            std::uint64_t v = 0;
            std::memcpy(&v, offsets + i * width, width);  // little endian hosts
            if (f.number == 3) {
                out.push_back(v);
            } else {
                if (base > std::numeric_limits<std::uint64_t>::max() - v) {
                    error = "row id array base plus offset overflows";
                    return false;
                }
                out.push_back(base + v);
            }
        }
    }
    if (!r.ok() || !seen) {
        error = "malformed row id array";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> encode_encoded_array(const std::vector<std::uint64_t>& values) {
    std::vector<std::uint8_t> out;
    std::uint64_t min = 0;
    std::uint64_t max = 0;
    if (!values.empty()) {
        const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
        min = *lo;
        max = *hi;
    }
    std::vector<std::uint8_t> inner;
    std::uint32_t field = 3;
    if (values.empty()) {
        field = 3;
        put_bytes_field(inner, 2, {});
    } else if (max - min <= 0xFFFFU) {
        field = 1;
        put_u64_field(inner, 1, min);
        std::vector<std::uint8_t> raw;
        raw.reserve(values.size() * 2);
        for (const auto v : values) {
            const auto o = static_cast<std::uint16_t>(v - min);
            raw.push_back(static_cast<std::uint8_t>(o));
            raw.push_back(static_cast<std::uint8_t>(o >> 8U));
        }
        put_bytes_field(inner, 2, raw);
    } else if (max - min <= 0xFFFFFFFFULL) {
        field = 2;
        put_u64_field(inner, 1, min);
        std::vector<std::uint8_t> raw;
        raw.reserve(values.size() * 4);
        for (const auto v : values) {
            const auto o = static_cast<std::uint32_t>(v - min);
            for (unsigned b = 0; b < 4; ++b) {
                raw.push_back(static_cast<std::uint8_t>(o >> (8U * b)));
            }
        }
        put_bytes_field(inner, 2, raw);
    } else {
        field = 3;
        std::vector<std::uint8_t> raw;
        raw.reserve(values.size() * 8);
        for (const auto v : values) {
            for (unsigned b = 0; b < 8; ++b) {
                raw.push_back(static_cast<std::uint8_t>(v >> (8U * b)));
            }
        }
        put_bytes_field(inner, 2, raw);
    }
    put_bytes_field(out, field, inner);
    return out;
}

// ── U64Segment ──────────────────────────────────────────────────────────────────────────────────

/// One U64Segment message, appended to `out`'s values.
bool decode_segment(const std::uint8_t* data, std::size_t size, std::vector<std::uint64_t>& out, std::string& error) {
    Reader r(data, size);
    Field f;
    if (!r.next(f) || f.wire != 2) {
        error = "missing row id segment type";
        return false;
    }
    Reader inner(f.data, f.size);
    Field g;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    const std::uint8_t* extra = nullptr;
    std::size_t extra_size = 0;
    while (inner.next(g)) {
        if (g.number == 1 && g.wire == 0) {
            start = g.value;
        } else if (g.number == 2 && g.wire == 0) {
            end = g.value;
        } else if (g.number == 3 && g.wire == 2) {
            extra = g.data;
            extra_size = g.size;
        }
    }
    if (!inner.ok()) {
        error = "malformed row id segment";
        return false;
    }
    switch (f.number) {
        case 1:  // Range
        case 2:  // RangeWithHoles
        case 3: {  // RangeWithBitmap
            if (end < start) {
                error = "row id segment range start exceeds end";
                return false;
            }
            const std::uint64_t span = end - start;
            if (span > (1ULL << 40U)) {
                error = "row id segment range is too long";
                return false;
            }
            if (f.number == 1) {
                for (std::uint64_t v = start; v < end; ++v) {
                    out.push_back(v);
                }
            } else if (f.number == 2) {
                std::vector<std::uint64_t> holes;
                if (extra == nullptr || !decode_encoded_array(extra, extra_size, holes, error)) {
                    if (error.empty()) {
                        error = "RangeWithHoles is missing its holes array";
                    }
                    return false;
                }
                std::size_t h = 0;
                for (std::uint64_t v = start; v < end; ++v) {
                    if (h < holes.size() && holes[h] == v) {
                        ++h;
                        continue;
                    }
                    out.push_back(v);
                }
            } else {
                const std::uint64_t need = (span + 7U) / 8U;
                if (extra == nullptr || extra_size < need) {
                    error = "RangeWithBitmap bitmap is too short";
                    return false;
                }
                for (std::uint64_t i = 0; i < span; ++i) {
                    if ((extra[i / 8U] >> (i % 8U)) & 1U) {  // least significant bit first
                        out.push_back(start + i);
                    }
                }
            }
            return true;
        }
        case 4:  // SortedArray
        case 5:  // Array
            return decode_encoded_array(f.data, f.size, out, error);
        default:
            error = "unknown row id segment type";
            return false;
    }
}

std::vector<std::uint8_t> segment_range(std::uint64_t start, std::uint64_t end) {
    std::vector<std::uint8_t> inner;
    put_u64_field(inner, 1, start);
    put_u64_field(inner, 2, end);
    std::vector<std::uint8_t> out;
    put_bytes_field(out, 1, inner);
    return out;
}

/// The segment Lance would choose for `values` (U64Segment::from_slice).
std::vector<std::uint8_t> encode_segment(const std::vector<std::uint64_t>& values) {
    bool sorted = true;
    std::uint64_t min = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max = 0;
    std::uint64_t count = 0;
    for (const auto v : values) {
        ++count;
        min = std::min(min, v);
        max = std::max(max, v);
        if (sorted && count > 1 && v < max) {
            sorted = false;
        }
    }
    if (count == 0) {
        return segment_range(0, 0);
    }
    std::vector<std::uint8_t> out;
    if (!sorted) {
        put_bytes_field(out, 5, encode_encoded_array(values));
        return out;
    }
    if (max == std::numeric_limits<std::uint64_t>::max()) {
        put_bytes_field(out, 4, encode_encoded_array(values));
        return out;
    }
    const std::uint64_t total = max - min + 1;
    const std::uint64_t holes = total - count;
    if (holes == 0) {
        return segment_range(min, max + 1);
    }
    const unsigned __int128 holes_size = 24U + 4U * static_cast<unsigned __int128>(holes);
    const unsigned __int128 bitmap_size = 24U + (static_cast<unsigned __int128>(total) + 7U) / 8U;
    const unsigned __int128 array_size = 24U + 2U * static_cast<unsigned __int128>(count);
    if (holes_size <= bitmap_size && holes_size <= array_size) {
        std::vector<std::uint64_t> hole_values;
        hole_values.reserve(static_cast<std::size_t>(holes));
        std::size_t i = 0;
        for (std::uint64_t v = min; v <= max; ++v) {
            if (i < values.size() && values[i] == v) {
                ++i;
            } else {
                hole_values.push_back(v);
            }
        }
        std::vector<std::uint8_t> inner;
        put_u64_field(inner, 1, min);
        put_u64_field(inner, 2, max + 1);
        put_bytes_field(inner, 3, encode_encoded_array(hole_values));
        put_bytes_field(out, 2, inner);
    } else if (bitmap_size <= array_size) {
        std::vector<std::uint8_t> bitmap(static_cast<std::size_t>((total + 7U) / 8U), 0xFF);
        if (total % 8U != 0U) {
            bitmap.back() = static_cast<std::uint8_t>((1U << (total % 8U)) - 1U);
        }
        std::size_t i = 0;
        for (std::uint64_t v = min; v <= max; ++v) {
            if (i < values.size() && values[i] == v) {
                ++i;
            } else {
                const auto bit = v - min;
                bitmap[static_cast<std::size_t>(bit / 8U)] &= static_cast<std::uint8_t>(~(1U << (bit % 8U)));
            }
        }
        std::vector<std::uint8_t> inner;
        put_u64_field(inner, 1, min);
        put_u64_field(inner, 2, max + 1);
        put_bytes_field(inner, 3, bitmap);
        put_bytes_field(out, 3, inner);
    } else {
        put_bytes_field(out, 4, encode_encoded_array(values));
    }
    return out;
}

}  // namespace

// ── RowIdSequence ───────────────────────────────────────────────────────────────────────────────

RowIdSequence RowIdSequence::range(std::uint64_t start, std::uint64_t count) {
    RowIdSequence s;
    s.compact_ = true;
    s.start_ = start;
    s.count_ = count;
    return s;
}

RowIdSequence RowIdSequence::from_values(std::vector<std::uint64_t> values) {
    RowIdSequence s;
    bool consecutive = true;
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (values[i] != values[i - 1] + 1U) {
            consecutive = false;
            break;
        }
    }
    if (consecutive) {
        s.compact_ = true;
        s.start_ = values.empty() ? 0 : values.front();
        s.count_ = values.size();
        return s;
    }
    s.compact_ = false;
    s.values_ = std::move(values);
    return s;
}

bool RowIdSequence::decode(const std::uint8_t* data, std::size_t size, RowIdSequence& out, std::string& error) {
    Reader r(data, size);
    Field f;
    std::vector<std::uint64_t> all;
    while (r.next(f)) {
        if (f.number == 1 && f.wire == 2) {
            if (!decode_segment(f.data, f.size, all, error)) {
                return false;
            }
        }
    }
    if (!r.ok()) {
        error = "malformed row id sequence";
        return false;
    }
    out = from_values(std::move(all));
    return true;
}

std::vector<std::uint8_t> RowIdSequence::encode() const {
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> seg;
    if (compact_) {
        seg = segment_range(start_, start_ + count_);
    } else {
        seg = encode_segment(values_);
    }
    put_bytes_field(out, 1, seg);
    return out;
}

bool RowIdSequence::is_range(std::uint64_t& start) const {
    if (compact_) {
        start = start_;
        return true;
    }
    return false;
}

bool RowIdSequence::select(const std::vector<std::uint64_t>& offsets, std::vector<std::uint64_t>& out) const {
    out.clear();
    out.reserve(offsets.size());
    const auto n = size();
    for (const auto o : offsets) {
        if (o >= n) {
            return false;
        }
        out.push_back(at(o));
    }
    return true;
}

std::vector<std::uint64_t> RowIdSequence::to_vector() const {
    if (!compact_) {
        return values_;
    }
    std::vector<std::uint64_t> v(static_cast<std::size_t>(count_));
    for (std::size_t i = 0; i < v.size(); ++i) {
        v[i] = start_ + i;
    }
    return v;
}

bool RowIdSequence::max_id(std::uint64_t& out) const {
    if (empty()) {
        return false;
    }
    if (compact_) {
        out = start_ + count_ - 1U;
        return true;
    }
    out = *std::max_element(values_.begin(), values_.end());
    return true;
}

// ── RowVersionSequence ──────────────────────────────────────────────────────────────────────────

RowVersionSequence RowVersionSequence::uniform(std::uint64_t rows, std::uint64_t version) {
    RowVersionSequence s;
    if (rows != 0U) {
        s.runs_.push_back({rows, version});
    }
    return s;
}

bool RowVersionSequence::decode(const std::uint8_t* data, std::size_t size, RowVersionSequence& out,
                                std::string& error) {
    out.runs_.clear();
    Reader r(data, size);
    Field f;
    while (r.next(f)) {
        if (f.number != 1 || f.wire != 2) {
            continue;
        }
        Reader inner(f.data, f.size);
        Field g;
        std::vector<std::uint64_t> span;
        std::uint64_t version = 0;
        bool have_span = false;
        while (inner.next(g)) {
            if (g.number == 1 && g.wire == 2) {
                if (!decode_segment(g.data, g.size, span, error)) {
                    return false;
                }
                have_span = true;
            } else if (g.number == 2 && g.wire == 0) {
                version = g.value;
            }
        }
        if (!inner.ok() || !have_span) {
            error = "malformed row version run";
            return false;
        }
        out.runs_.push_back({span.size(), version});
    }
    if (!r.ok()) {
        error = "malformed row version sequence";
        return false;
    }
    return true;
}

std::vector<std::uint8_t> RowVersionSequence::encode() const {
    std::vector<std::uint8_t> out;
    std::uint64_t at = 0;
    for (const auto& run : runs_) {
        std::vector<std::uint8_t> inner;
        put_bytes_field(inner, 1, segment_range(at, at + run.length));
        put_u64_field(inner, 2, run.version);
        put_bytes_field(out, 1, inner);
        at += run.length;
    }
    return out;
}

std::uint64_t RowVersionSequence::size() const {
    std::uint64_t n = 0;
    for (const auto& run : runs_) {
        n += run.length;
    }
    return n;
}

std::uint64_t RowVersionSequence::at(std::uint64_t index) const {
    for (const auto& run : runs_) {
        if (index < run.length) {
            return run.version;
        }
        index -= run.length;
    }
    return 0;
}

std::vector<std::uint64_t> RowVersionSequence::to_vector() const {
    std::vector<std::uint64_t> v;
    v.reserve(static_cast<std::size_t>(size()));
    for (const auto& run : runs_) {
        v.insert(v.end(), static_cast<std::size_t>(run.length), run.version);
    }
    return v;
}

RowVersionSequence RowVersionSequence::from_values(const std::vector<std::uint64_t>& values) {
    RowVersionSequence s;
    for (const auto v : values) {
        if (!s.runs_.empty() && s.runs_.back().version == v) {
            ++s.runs_.back().length;
        } else {
            s.runs_.push_back({1, v});
        }
    }
    return s;
}

// ── fragment fields ─────────────────────────────────────────────────────────────────────────────

bool read_fragment_row_meta(const pb::DataFragment& fragment, FragmentRowMeta& out, std::string& error) {
    out = FragmentRowMeta{};
    Reader r(fragment.unknown.data(), fragment.unknown.size());
    Field f;
    while (r.next(f)) {
        switch (f.number) {
            case 5:
                out.has_row_ids = true;
                out.row_ids.assign(f.data, f.data + f.size);
                break;
            case 7:
                out.has_last_updated = true;
                out.last_updated.assign(f.data, f.data + f.size);
                break;
            case 9:
                out.has_created = true;
                out.created.assign(f.data, f.data + f.size);
                break;
            case 6:
            case 8:
            case 10:
                out.external = true;
                break;
            default:
                break;
        }
    }
    if (!r.ok()) {
        error = "malformed fragment row metadata";
        return false;
    }
    return true;
}

namespace {

/// `unknown` without the fields in `drop`.
std::vector<std::uint8_t> without_fields(const std::vector<std::uint8_t>& unknown, std::initializer_list<std::uint32_t> drop) {
    std::vector<std::uint8_t> out;
    Reader r(unknown.data(), unknown.size());
    Field f;
    while (r.next(f)) {
        if (std::find(drop.begin(), drop.end(), f.number) == drop.end()) {
            out.insert(out.end(), f.begin, f.end);
        }
    }
    return out;
}

}  // namespace

void clear_fragment_row_meta(pb::DataFragment& fragment) {
    fragment.unknown = without_fields(fragment.unknown, {5, 6, 7, 8, 9, 10});
}

void write_fragment_row_meta(pb::DataFragment& fragment, const FragmentRowMeta& meta) {
    clear_fragment_row_meta(fragment);
    if (meta.has_row_ids) {
        put_bytes_field(fragment.unknown, 5, meta.row_ids);
    }
    if (meta.has_last_updated) {
        put_bytes_field(fragment.unknown, 7, meta.last_updated);
    }
    if (meta.has_created) {
        put_bytes_field(fragment.unknown, 9, meta.created);
    }
}

bool fragment_row_ids(const pb::DataFragment& fragment, RowIdSequence& out, std::string& error) {
    FragmentRowMeta meta;
    if (!read_fragment_row_meta(fragment, meta, error)) {
        return false;
    }
    if (meta.external) {
        error = "row ids stored in an external file are not supported";
        return false;
    }
    if (!meta.has_row_ids) {
        error = "fragment " + std::to_string(fragment.id) + " has no row ids";
        return false;
    }
    if (!RowIdSequence::decode(meta.row_ids.data(), meta.row_ids.size(), out, error)) {
        return false;
    }
    if (out.size() != fragment.physical_rows) {
        error = "fragment " + std::to_string(fragment.id) + " has " + std::to_string(fragment.physical_rows) +
                " rows but " + std::to_string(out.size()) + " row ids";
        return false;
    }
    return true;
}

// ── RowIdIndex ──────────────────────────────────────────────────────────────────────────────────

bool RowIdIndex::build(const std::filesystem::path& dataset_path, const pb::Manifest& manifest, RowIdIndex& out,
                       std::string& error) {
    out = RowIdIndex{};
    for (const auto& fragment : manifest.fragments) {
        RowIdSequence ids;
        if (!fragment_row_ids(fragment, ids, error)) {
            return false;
        }
        std::vector<std::uint32_t> deleted;
        if (fragment.deletion_file.present &&
            !read_deletion_vector(dataset_path, fragment.id, fragment.deletion_file, deleted, error)) {
            return false;
        }
        std::uint64_t start = 0;
        if (ids.is_range(start)) {
            // The live stretches between deleted offsets, each a run.
            std::uint64_t from = 0;
            const auto total = ids.size();
            const auto flush = [&](std::uint64_t to) {
                if (to > from) {
                    out.runs_.push_back({start + from, to - from, fragment.id, from});
                }
            };
            for (const auto d : deleted) {
                if (d >= total) {
                    continue;
                }
                flush(d);
                from = static_cast<std::uint64_t>(d) + 1U;
            }
            flush(total);
        } else {
            std::size_t next_deleted = 0;
            for (std::uint64_t i = 0; i < ids.size(); ++i) {
                while (next_deleted < deleted.size() && deleted[next_deleted] < i) {
                    ++next_deleted;
                }
                if (next_deleted < deleted.size() && deleted[next_deleted] == i) {
                    continue;
                }
                out.by_id_[ids.at(i)] = (fragment.id << 32U) | i;
            }
        }
    }
    std::sort(out.runs_.begin(), out.runs_.end(), [](const Run& a, const Run& b) { return a.first_id < b.first_id; });
    out.sorted_ = true;
    return true;
}

bool RowIdIndex::find(std::uint64_t row_id, std::uint64_t& address) const {
    auto it = std::upper_bound(runs_.begin(), runs_.end(), row_id,
                               [](std::uint64_t id, const Run& run) { return id < run.first_id; });
    if (it != runs_.begin()) {
        --it;
        if (row_id - it->first_id < it->count) {
            address = (it->fragment_id << 32U) | (it->first_offset + (row_id - it->first_id));
            return true;
        }
    }
    const auto found = by_id_.find(row_id);
    if (found != by_id_.end()) {
        address = found->second;
        return true;
    }
    return false;
}


bool resolve_row_ids(const std::filesystem::path& dataset_path, std::optional<std::uint64_t> version,
                     const std::vector<std::uint64_t>& ids, std::vector<std::uint64_t>& addresses,
                     std::string& error, bool skip_missing) {
    pb::Manifest manifest;
    if (version) {
        if (!load_manifest_version(dataset_path, *version, manifest, error)) {
            return false;
        }
    } else {
        std::uint64_t latest = 0;
        if (!load_latest_manifest(dataset_path, manifest, latest, error)) {
            return false;
        }
    }
    const bool stable = (manifest.reader_feature_flags & pb::kFlagStableRowIds) != 0U;
    addresses.clear();
    addresses.reserve(ids.size());
    RowIdIndex index;
    if (stable && !RowIdIndex::build(dataset_path, manifest, index, error)) {
        return false;
    }
    std::map<std::uint64_t, const pb::DataFragment*> fragments;
    for (const auto& f : manifest.fragments) {
        fragments.emplace(f.id, &f);
    }
    std::map<std::uint64_t, std::unordered_set<std::uint32_t>> deleted;
    for (const auto id : ids) {
        std::uint64_t address = id;
        if (stable && !index.find(id, address)) {
            if (skip_missing) {
                continue;
            }
            error = "row id " + std::to_string(id) + " is not in the dataset";
            return false;
        }
        const auto fragment = fragments.find(address >> 32U);
        if (fragment == fragments.end() || (address & 0xFFFFFFFFULL) >= fragment->second->physical_rows) {
            if (skip_missing) {
                continue;
            }
            error = "row id " + std::to_string(id) + " is not in the dataset";
            return false;
        }
        if (fragment->second->deletion_file.present) {
            auto slot = deleted.find(fragment->first);
            if (slot == deleted.end()) {
                std::vector<std::uint32_t> offsets;
                if (!read_deletion_vector(dataset_path, fragment->first, fragment->second->deletion_file, offsets,
                                          error)) {
                    return false;
                }
                slot = deleted.emplace(fragment->first, std::unordered_set<std::uint32_t>(offsets.begin(),
                                                                                           offsets.end()))
                           .first;
            }
            if (slot->second.count(static_cast<std::uint32_t>(address & 0xFFFFFFFFULL)) != 0U) {
                if (skip_missing) {
                    continue;
                }
                error = "row id " + std::to_string(id) + " is not in the dataset (its row was deleted)";
                return false;
            }
        }
        addresses.push_back(address);
    }
    return true;
}

bool row_versions_at(const std::filesystem::path& dataset_path, std::optional<std::uint64_t> version,
                    const std::vector<std::uint64_t>& addresses, std::vector<std::uint64_t>& created,
                    std::vector<std::uint64_t>& last_updated, std::string& error) {
    pb::Manifest manifest;
    if (version) {
        if (!load_manifest_version(dataset_path, *version, manifest, error)) {
            return false;
        }
    } else {
        std::uint64_t latest = 0;
        if (!load_latest_manifest(dataset_path, manifest, latest, error)) {
            return false;
        }
    }
    std::map<std::uint64_t, const pb::DataFragment*> fragments;
    for (const auto& f : manifest.fragments) {
        fragments.emplace(f.id, &f);
    }
    struct Versions {
        std::vector<std::uint64_t> created;
        std::vector<std::uint64_t> updated;
    };
    std::map<std::uint64_t, Versions> cache;
    created.clear();
    last_updated.clear();
    for (const auto address : addresses) {
        const auto fragment_id = address >> 32U;
        auto slot = cache.find(fragment_id);
        if (slot == cache.end()) {
            Versions v;
            const auto it = fragments.find(fragment_id);
            if (it != fragments.end()) {
                FragmentRowMeta meta;
                if (!read_fragment_row_meta(*it->second, meta, error)) {
                    return false;
                }
                RowVersionSequence seq;
                if (meta.has_created) {
                    if (!RowVersionSequence::decode(meta.created.data(), meta.created.size(), seq, error)) {
                        return false;
                    }
                    v.created = seq.to_vector();
                }
                if (meta.has_last_updated) {
                    if (!RowVersionSequence::decode(meta.last_updated.data(), meta.last_updated.size(), seq, error)) {
                        return false;
                    }
                    v.updated = seq.to_vector();
                }
            }
            slot = cache.emplace(fragment_id, std::move(v)).first;
        }
        const auto offset = static_cast<std::size_t>(address & 0xFFFFFFFFULL);
        created.push_back(offset < slot->second.created.size() ? slot->second.created[offset] : 1U);
        last_updated.push_back(offset < slot->second.updated.size() ? slot->second.updated[offset] : 1U);
    }
    return true;
}

bool AddressToRowId::build(const pb::Manifest& manifest, AddressToRowId& out, std::string& error) {
    out = AddressToRowId{};
    out.stable_ = (manifest.reader_feature_flags & pb::kFlagStableRowIds) != 0U;
    if (!out.stable_) {
        return true;
    }
    for (const auto& fragment : manifest.fragments) {
        RowIdSequence ids;
        if (!fragment_row_ids(fragment, ids, error)) {
            return false;
        }
        out.by_fragment_.emplace(fragment.id, std::move(ids));
    }
    return true;
}

std::uint64_t AddressToRowId::operator()(std::uint64_t address) const {
    if (!stable_) {
        return address;
    }
    const auto it = by_fragment_.find(address >> 32U);
    const auto offset = address & 0xFFFFFFFFULL;
    return it != by_fragment_.end() && offset < it->second.size() ? it->second.at(offset) : address;
}

bool RowIdToAddress::build(const std::filesystem::path& dataset_path, const pb::Manifest& manifest,
                           RowIdToAddress& out, std::string& error) {
    out = RowIdToAddress{};
    out.stable_ = (manifest.reader_feature_flags & pb::kFlagStableRowIds) != 0U;
    return !out.stable_ || RowIdIndex::build(dataset_path, manifest, out.index_, error);
}

bool RowIdToAddress::find(std::uint64_t row_id, std::uint64_t& address) const {
    if (!stable_) {
        address = row_id;
        return true;
    }
    return index_.find(row_id, address);
}

}  // namespace nano_lance
