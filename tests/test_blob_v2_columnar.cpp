// Phase 2: the columnar blob.v2 encapsulation. The producer describes a reference row as
// BlobRef{position,size}, fills a soatins soa<BlobRef,N>, and hands the two columns + the shared window
// URI to nanolance's build_blob_v2_external_array — never touching the lance.blob.v2 struct shape, child
// order, names, or the `data`/`kind` conventions. The BlobV2ColumnView reads it back the same way.
// This test proves that round-trip in-process (no Lance file): build from soa columns -> view -> values.

#include "nanolance/blob_builder.hpp"
#include "nanolance/blob_v2_external.hpp"

#include "soatins/endian.hpp"
#include "soatins/reflect.hpp"

#include <nanoarrow/nanoarrow.h>

#include <boost/describe.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// The producer's semantic reference row: pure fixed-width columns, no var-width uri (that's shared,
// out-of-band) — exactly the form that fits a fixed-N SoA and the GPU.
struct BlobRef {
    std::uint64_t position;
    std::uint64_t size;
};
BOOST_DESCRIBE_STRUCT(BlobRef, (), (position, size))

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

}  // namespace

int main() {
    constexpr std::size_t N = 64;
    constexpr std::size_t K = 25;
    const char* kUri = "file:///data/window_0007.raw";

    // Producer side: fill the reference SoA. Nothing here knows about lance.blob.v2.
    soatins::soa<BlobRef, N> refs;
    for (std::size_t i = 0; i < K; ++i) {
        refs.append(BlobRef{/*position=*/i * 9000ull + 64ull, /*size=*/i * 13ull + 1ull});
    }
    CHECK(refs.size() == K);

    // Hand the two columns + the shared URI to nanolance; it owns the Arrow envelope.
    ArrowArray array{};
    std::string err;
    CHECK(nano_lance::build_blob_v2_external_array(refs.column<0>().data(),  // positions
                                                   refs.column<1>().data(),  // sizes
                                                   refs.size(), kUri, array, err));

    // Read it back through the view: resolve children once, then read values.
    ArrowSchema schema{};
    CHECK(nano_lance::build_blob_v2_payload_schema(schema, err));
    ArrowArrayView view{};
    ArrowError ae;
    CHECK(ArrowArrayViewInitFromSchema(&view, &schema, &ae) == NANOARROW_OK);
    CHECK(ArrowArrayViewSetArray(&view, &array, &ae) == NANOARROW_OK);

    nano_lance::BlobV2ColumnView blob;
    CHECK(blob.init(schema, view, err));
    CHECK(blob.size() == static_cast<std::int64_t>(K));

    for (std::size_t i = 0; i < K; ++i) {
        CHECK(blob.position(static_cast<std::int64_t>(i)) == i * 9000ull + 64ull);
        CHECK(blob.byte_size(static_cast<std::int64_t>(i)) == i * 13ull + 1ull);
        const char* udata = nullptr;
        std::int64_t usize = 0;
        blob.uri(static_cast<std::int64_t>(i), &udata, &usize);
        CHECK(std::string(udata, static_cast<std::size_t>(usize)) == kUri);
    }

    ArrowArrayViewReset(&view);
    if (schema.release) schema.release(&schema);
    if (array.release) array.release(&array);

    std::printf("blob_v2_columnar: ok (soa<BlobRef,%zu> -> build_blob_v2_external_array -> view, %zu rows)\n",
                N, K);
    return 0;
}
