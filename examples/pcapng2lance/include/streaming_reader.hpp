#pragma once

// Bounded, forward-only windowed reader for endless / S3-backed captures. A `Source` yields bytes
// sequentially; `Window` keeps a capped buffer over them so scan + bulk parse run on the SAME resident
// bytes (never re-reading). The window always starts at a block boundary: process complete blocks, then
// `consume()` slides the unprocessed tail down and `fill()` tops up. A block larger than the buffer
// triggers `grow()`. This is the host scaffolding the CUDA `ex::bulk` path will later run per window.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace streaming {

using Bytes = std::span<const std::uint8_t>;

// Sequential file source. The same shape (read N bytes forward) is all an S3 streaming source needs.
class FileSource {
public:
    explicit FileSource(const std::filesystem::path& path) : in_(path, std::ios::binary) {}
    bool ok() const { return static_cast<bool>(in_); }
    std::size_t read(std::uint8_t* dst, std::size_t n) {
        in_.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
        return static_cast<std::size_t>(in_.gcount());
    }

private:
    std::ifstream in_;
};

// Sliding window over a source: holds the absolute byte range [base(), base()+size()).
template <class Source>
class Window {
public:
    Window(Source& source, std::size_t capacity) : source_(source) { buf_.resize(capacity == 0 ? 1 : capacity); }

    Bytes bytes() const { return Bytes(buf_.data(), valid_); }
    std::uint64_t base() const { return base_; }  // absolute offset of bytes()[0]
    std::size_t size() const { return valid_; }
    bool eof() const { return eof_; }
    bool full() const { return valid_ == buf_.size(); }

    // Top the buffer up to capacity from the source.
    void fill() {
        while (valid_ < buf_.size() && !eof_) {
            const std::size_t got = source_.read(buf_.data() + valid_, buf_.size() - valid_);
            valid_ += got;
            if (got == 0) {
                eof_ = true;
            }
        }
    }

    // Slide forward: drop the first n consumed bytes, keep the tail, advance the absolute base.
    void consume(std::size_t n) {
        if (n > valid_) {
            n = valid_;
        }
        std::memmove(buf_.data(), buf_.data() + n, valid_ - n);
        valid_ -= n;
        base_ += n;
    }

    // Enlarge the buffer when a single block can't fit (keeps existing bytes; the next fill() reads more).
    void grow() { buf_.resize(buf_.size() * 2); }

private:
    Source& source_;
    std::vector<std::uint8_t> buf_;
    std::size_t valid_ = 0;
    std::uint64_t base_ = 0;
    bool eof_ = false;
};

}  // namespace streaming
