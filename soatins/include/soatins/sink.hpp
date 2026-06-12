#pragma once

// column_sink<T, N>: the auto-flushing front end of a fixed-capacity soa<T, N>. push() a row at a time;
// when the chunk fills (the occupied counter reaches N) it calls the bound flush callback with the full
// chunk and resets the counter (storage reused — no realloc, no re-zero). finish() drains the partial
// tail at end of stream. This is the single place the "fill a fixed SoA, flush when full" loop lives, so
// every producer (L1 packets, the remainder, a sensor's PDUs) is one push()/finish() pair instead of a
// hand-written chunk-and-flush.
//
// The flush callback is `bool(soa<T, N>& chunk, std::string& error)` — it owns *how* a full chunk drains
// (to_arrow -> nano_lance_write_batch, a GPU H2D, an ndjson line, ...). It is templated, not std::function,
// so it inlines and allocates nothing; it fires only once per N rows. Because the flush is SYNCHRONOUS and
// the chunk is reused only after it returns, the chunk provably outlives anything the flush hands out of
// it — which is exactly the lifetime guarantee a zero-copy borrow to_arrow needs.

#include "soatins/reflect.hpp"

#include <cstddef>
#include <string>
#include <utility>

namespace soatins {

template <class T, std::size_t N, class Flush>
class column_sink {
public:
    explicit column_sink(Flush flush) : flush_(std::move(flush)) {}

    // Append one row; auto-flushes the chunk when it just filled. Returns false (and sets error) only if a
    // flush failed.
    bool push(const T& row, std::string& error) {
        if (chunk_.append(row)) {  // append() returns true once the chunk is full
            return flush_chunk(error);
        }
        return true;
    }

    // Drain the partial tail (call once at end of stream). A no-op when the chunk is empty, so it is safe
    // to call unconditionally.
    bool finish(std::string& error) {
        if (chunk_.size() == 0) {
            return true;
        }
        return flush_chunk(error);
    }

    std::size_t pending() const { return chunk_.size(); }  // rows buffered but not yet flushed
    static constexpr std::size_t capacity = N;

private:
    bool flush_chunk(std::string& error) {
        if (!flush_(chunk_, error)) {
            return false;
        }
        chunk_.clear();  // reset the occupied counter; the array storage is reused as-is
        return true;
    }

    soa<T, N> chunk_;
    Flush flush_;
};

// Deduce the flush type while you pick T and N explicitly: make_column_sink<Row, 1024>(flush).
template <class T, std::size_t N, class Flush>
column_sink<T, N, Flush> make_column_sink(Flush flush) {
    return column_sink<T, N, Flush>(std::move(flush));
}

}  // namespace soatins
