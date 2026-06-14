// Phase 2: the columnar blob.v2 encapsulation. The producer supplies two fixed-width reference columns
// (position, size) + the shared window URI to nanolance's build_blob_v2_external_array — never touching the
// lance.blob.v2 struct shape, child order, names, or the `data`/`kind` conventions. The BlobV2ColumnView
// reads it back the same way. This test proves that round-trip in-process (no Lance file): build from the
// columns -> view -> values. (The columns are plain vectors here; a producer like the pcapng2lance example
// fills them from a soatins soa<BlobRef,N>, but nanolance itself depends only on nanoarrow + local code.)

#include "nanolance/blob_builder.hpp"
#include "nanolance/blob_v2_external.hpp"

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

}  // namespace

int main() {
    constexpr std::size_t K = 25;
    const char* kUri = "file:///data/window_0007.raw";

    // Producer side: two fixed-width reference columns (position, size). No var-width uri (that's shared,
    // out-of-band) — exactly the form that fits a fixed-N SoA and the GPU.
    std::vector<std::uint64_t> positions(K), sizes(K);
    for (std::size_t i = 0; i < K; ++i) {
        positions[i] = i * 9000ull + 64ull;
        sizes[i] = i * 13ull + 1ull;
    }

    // Hand the two columns + the shared URI to nanolance; it owns the Arrow envelope.
    ArrowArray array{};
    std::string err;
    CHECK(nano_lance::build_blob_v2_external_array(positions.data(), sizes.data(), positions.size(), kUri,
                                                   array, err));

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

    std::printf("blob_v2_columnar: ok (position/size columns -> build_blob_v2_external_array -> view, %zu rows)\n",
                K);
    return 0;
}
