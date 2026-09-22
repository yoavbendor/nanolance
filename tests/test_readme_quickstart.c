/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Yoav Bendor
 *
 * The C quick start from README.md / AGENTS.md / docs, compiled and run verbatim.
 *
 * Two things rot independently and both mislead a first-time user: the snippet itself (a renamed
 * function, a setter that no longer exists, an option that must move), and the claim that the C API
 * is usable from C. This file is built as C, not C++, so `nanolance/nano_lance_writer.h` has to stay
 * genuinely C-clean -- every other consumer in the tree is C++ and would not notice if it stopped
 * being so.
 *
 * Keep the block between the BEGIN/END markers identical to the documented snippet.
 */

#include "nanolance/nano_lance_writer.h"

#include <nanoarrow/nanoarrow.h>

#include <stdio.h>
#include <string.h>

static int build_batch(struct ArrowSchema* schema, struct ArrowArray* array) {
    ArrowSchemaInit(schema);
    if (ArrowSchemaSetTypeStruct(schema, 2) != NANOARROW_OK) {
        return -1;
    }
    if (ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64) != NANOARROW_OK ||
        ArrowSchemaSetName(schema->children[0], "id") != NANOARROW_OK ||
        ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_STRING) != NANOARROW_OK ||
        ArrowSchemaSetName(schema->children[1], "name") != NANOARROW_OK) {
        return -1;
    }
    if (ArrowArrayInitFromSchema(array, schema, NULL) != NANOARROW_OK ||
        ArrowArrayStartAppending(array) != NANOARROW_OK) {
        return -1;
    }
    {
        const char* names[3] = {"alpha", "beta", "gamma"};
        int i;
        for (i = 0; i < 3; ++i) {
            struct ArrowStringView sv;
            sv.data = names[i];
            sv.size_bytes = (int64_t)strlen(names[i]);
            if (ArrowArrayAppendInt(array->children[0], i + 1) != NANOARROW_OK ||
                ArrowArrayAppendString(array->children[1], sv) != NANOARROW_OK ||
                ArrowArrayFinishElement(array) != NANOARROW_OK) {
                return -1;
            }
        }
    }
    return ArrowArrayFinishBuildingDefault(array, NULL) == NANOARROW_OK ? 0 : -1;
}

int main(int argc, char** argv) {
    struct ArrowSchema arrow_schema;
    struct ArrowArray arrow_array;
    const char* out_path = argc > 1 ? argv[1] : "readme_quickstart.lance";

    if (build_batch(&arrow_schema, &arrow_array) != 0) {
        fprintf(stderr, "failed to build the sample batch\n");
        return 1;
    }

    /* ---- BEGIN documented snippet (keep in step with README.md "Quick start (C write)") ---- */
    {
        NanoLanceWriter w = {0};
        nano_lance_writer_init(&w, out_path, /*compression_level=*/3);
        nano_lance_writer_set_compression(&w, true); /* Lance-compatible compression (off by default) */
        nano_lance_write_batch(&w, &arrow_array, &arrow_schema); /* repeatable; schema locks after #1 */
        nano_lance_writer_commit(&w, /*is_append=*/false);       /* false = create, true = append */
        nano_lance_writer_close(&w);
        /* On any non-zero return: nano_lance_writer_last_error(&w). */
        /* ---- END documented snippet ---- */

        /* The snippet above ignores return codes for brevity, exactly as documented. Re-run it with
         * checking so this test actually fails when a step breaks. */
        {
            NanoLanceWriter checked = {0};
            struct ArrowSchema schema2;
            struct ArrowArray array2;
            char checked_path[512];
            snprintf(checked_path, sizeof(checked_path), "%s.checked", out_path);
            if (build_batch(&schema2, &array2) != 0) {
                fprintf(stderr, "failed to rebuild the sample batch\n");
                return 1;
            }
            if (nano_lance_writer_init(&checked, checked_path, 3) != NANO_LANCE_OK ||
                nano_lance_writer_set_compression(&checked, true) != NANO_LANCE_OK ||
                nano_lance_write_batch(&checked, &array2, &schema2) != NANO_LANCE_OK ||
                nano_lance_writer_commit(&checked, false) != NANO_LANCE_OK) {
                fprintf(stderr, "quick start failed: %s\n", nano_lance_writer_last_error(&checked));
                return 1;
            }
            nano_lance_writer_close(&checked);
            /* Both, and in this order: an ArrowArray owns its buffers and children, so releasing
             * only the schema leaks them. (The documented snippet does not build the batch, so this
             * is the test's own bookkeeping -- but LeakSanitizer is right about it.) */
            ArrowArrayRelease(&array2);
            ArrowSchemaRelease(&schema2);
        }
    }

    ArrowArrayRelease(&arrow_array);
    ArrowSchemaRelease(&arrow_schema);
    printf("README C quick start: ok\n");
    return 0;
}
