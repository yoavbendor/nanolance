// M0 gate: a described struct -> soa -> to_arrow -> nano_lance write -> read back, bit-exact.
// Exercises the genuinely novel parts: the all-scalar path (what the packet row uses), be<>/le<>
// wire scalars, and bits<> multi-column expansion over a big-endian word. The fixed-size-binary
// (byte-array) path is write-only here — nanolance's writer-parity reader can't recover
// fixed_size_binary yet, so that column's values are checked by the pylance interop test instead.

#include "nanotins/arrow_glue.hpp"
#include "nanotins/bits.hpp"
#include "nanotins/endian.hpp"

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/nano_lance_writer.h"

#include <boost/describe.hpp>
#include <nanoarrow/nanoarrow.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "roundtrip failed: %s\n", msg);
        std::exit(1);
    }
}

struct Scalars {
    std::uint32_t a;
    std::uint64_t b;
    std::uint16_t c;
    std::int32_t d;
};
BOOST_DESCRIBE_STRUCT(Scalars, (), (a, b, c, d))

struct WireInt {
    nanotins::be<std::uint16_t> len;  // big-endian on the wire
    nanotins::be<std::uint32_t> id;
    nanotins::le<std::uint32_t> tag;  // little-endian on the wire
};
BOOST_DESCRIBE_STRUCT(WireInt, (), (len, id, tag))

struct Flags {
    nanotins::bits<nanotins::be<std::uint16_t>, nanotins::field<"pcp", 3>, nanotins::field<"dei", 1>,
                   nanotins::field<"vid", 12>>
        tci;
    std::uint8_t trailer;
};
BOOST_DESCRIBE_STRUCT(Flags, (), (tci, trailer))

struct Bytes {
    std::array<std::uint8_t, 4> addr;
    std::uint16_t port;
};
BOOST_DESCRIBE_STRUCT(Bytes, (), (addr, port))

// Pack a host value as big-endian wire bytes.
template <class T>
nanotins::be<T> mk_be(T v) {
    nanotins::be<T> b{};
    for (std::size_t k = sizeof(T); k-- > 0;) {
        b.raw[k] = static_cast<unsigned char>(v & 0xFF);
        v = static_cast<T>(v >> 8);
    }
    return b;
}
// Pack a host value as little-endian wire bytes.
template <class T>
nanotins::le<T> mk_le(T v) {
    nanotins::le<T> b{};
    for (std::size_t k = 0; k < sizeof(T); ++k) {
        b.raw[k] = static_cast<unsigned char>(v & 0xFF);
        v = static_cast<T>(v >> 8);
    }
    return b;
}

template <class T>
void write_dataset(const nanotins::soa<T>& s, const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    ArrowSchema schema{};
    ArrowArray batch{};
    std::string err;
    require(nanotins::arrow_schema<T>(schema, err), ("arrow_schema: " + err).c_str());
    require(nanotins::to_arrow<T>(s, batch, err), ("to_arrow: " + err).c_str());

    NanoLanceWriter writer{};
    require(nano_lance_writer_init(&writer, dir.string().c_str(), 0) == NANO_LANCE_OK, "writer init");
    require(nano_lance_write_batch(&writer, &batch, &schema) == NANO_LANCE_OK,
            (std::string("write_batch: ") + nano_lance_writer_last_error(&writer)).c_str());
    require(nano_lance_writer_commit(&writer, false) == NANO_LANCE_OK,
            (std::string("commit: ") + nano_lance_writer_last_error(&writer)).c_str());
    require(nano_lance_writer_close(&writer) == NANO_LANCE_OK, "writer close");

    batch.release(&batch);
    schema.release(&schema);
}

struct ReadBack {
    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    ArrowArrayView view{};

    ~ReadBack() {
        ArrowArrayViewReset(&view);
        for (auto& b : batches) {
            if (b.release) {
                b.release(&b);
            }
        }
        if (schema.release) {
            schema.release(&schema);
        }
    }
};

void read_dataset(const std::filesystem::path& dir, ReadBack& rb) {
    std::string err;
    require(nano_lance::lance_table_read_dataset(dir, rb.schema, rb.batches, err),
            ("read_dataset: " + err).c_str());
    require(rb.batches.size() == 1, "expected exactly one batch");
    ArrowError ae;
    require(ArrowArrayViewInitFromSchema(&rb.view, &rb.schema, &ae) == NANOARROW_OK, "view init");
    require(ArrowArrayViewSetArray(&rb.view, &rb.batches[0], &ae) == NANOARROW_OK, "view set array");
}

}  // namespace

int main() {
    const auto base = std::filesystem::temp_directory_path() / "nanotins_roundtrip";

    // --- 1. all-scalar (host order) — the M1 packet-row path ---
    {
        nanotins::soa<Scalars> s;
        s.resize(3);
        const Scalars rows[3] = {{1, 100, 7, -5},
                                 {0xAABBCCDD, 0x1122334455667788ULL, 0xFFFF, -2000000},
                                 {42, 0, 65535, 2000000}};
        for (std::size_t i = 0; i < 3; ++i) {
            s.store(i, rows[i]);
        }
        const auto dir = base / "scalars";
        write_dataset(s, dir);
        ReadBack rb;
        read_dataset(dir, rb);
        require(rb.view.n_children == 4, "scalars: 4 columns");
        for (int i = 0; i < 3; ++i) {
            require(ArrowArrayViewGetUIntUnsafe(rb.view.children[0], i) == rows[i].a, "scalars.a");
            require(ArrowArrayViewGetUIntUnsafe(rb.view.children[1], i) == rows[i].b, "scalars.b");
            require(ArrowArrayViewGetUIntUnsafe(rb.view.children[2], i) == rows[i].c, "scalars.c");
            require(ArrowArrayViewGetIntUnsafe(rb.view.children[3], i) == rows[i].d, "scalars.d");
        }
    }

    // --- 2. be<>/le<> wire scalars (swap happens in host()) ---
    {
        nanotins::soa<WireInt> s;
        s.resize(2);
        s.store(0, WireInt{mk_be<std::uint16_t>(0x0102), mk_be<std::uint32_t>(0xDEADBEEF), mk_le<std::uint32_t>(7)});
        s.store(1, WireInt{mk_be<std::uint16_t>(1500), mk_be<std::uint32_t>(1), mk_le<std::uint32_t>(0xCAFEBABE)});
        const auto dir = base / "wireint";
        write_dataset(s, dir);
        ReadBack rb;
        read_dataset(dir, rb);
        require(rb.view.n_children == 3, "wireint: 3 columns");
        require(ArrowArrayViewGetUIntUnsafe(rb.view.children[0], 0) == 0x0102, "wireint.len 0");
        require(ArrowArrayViewGetUIntUnsafe(rb.view.children[1], 0) == 0xDEADBEEF, "wireint.id 0");
        require(ArrowArrayViewGetUIntUnsafe(rb.view.children[2], 0) == 7, "wireint.tag 0");
        require(ArrowArrayViewGetUIntUnsafe(rb.view.children[0], 1) == 1500, "wireint.len 1");
        require(ArrowArrayViewGetUIntUnsafe(rb.view.children[2], 1) == 0xCAFEBABE, "wireint.tag 1");
    }

    // --- 3. bits<> multi-column expansion (MSB-first over a big-endian word) ---
    {
        nanotins::soa<Flags> s;
        s.resize(1);
        // pcp=5 (101), dei=1, vid=2730 (0xAAA) -> word = 101 1 101010101010 = 0xBAAA
        Flags f{{mk_be<std::uint16_t>(0xBAAA)}, 0x7F};
        s.store(0, f);
        const auto dir = base / "flags";
        write_dataset(s, dir);
        ReadBack rb;
        read_dataset(dir, rb);
        require(rb.view.n_children == 4, "flags: 3 subfields + trailer");
        require(ArrowArrayViewGetUIntUnsafe(rb.view.children[0], 0) == 5, "flags.pcp");
        require(ArrowArrayViewGetUIntUnsafe(rb.view.children[1], 0) == 1, "flags.dei");
        require(ArrowArrayViewGetUIntUnsafe(rb.view.children[2], 0) == 0xAAA, "flags.vid");
        require(ArrowArrayViewGetUIntUnsafe(rb.view.children[3], 0) == 0x7F, "flags.trailer");
    }

    // --- 4. fixed-size binary (byte array) — write-only; values checked by pylance interop ---
    {
        nanotins::soa<Bytes> s;
        s.resize(2);
        s.store(0, Bytes{{10, 0, 0, 1}, 80});
        s.store(1, Bytes{{192, 168, 1, 254}, 443});
        write_dataset(s, base / "bytes");  // write must succeed (valid Lance fixed_size_binary)
    }

    std::puts("nanotins roundtrip ok");
    return 0;
}
