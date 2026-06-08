#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ArrowArray;
struct ArrowSchema;

enum NanoLanceStatus {
    NANO_LANCE_OK = 0,
    NANO_LANCE_INVALID_ARGUMENT = 1,
    NANO_LANCE_INVALID_STATE = 2,
    NANO_LANCE_IO_ERROR = 3,
    NANO_LANCE_UNSUPPORTED = 4
};

typedef struct NanoLanceWriter {
    void* private_data;
    char last_error[512];
} NanoLanceWriter;

int nano_lance_writer_init(NanoLanceWriter* writer, const char* path, int compression_level);
/// Open an existing dataset for more fragments (reloads schema from latest manifest; commits must use `is_append=true`).
int nano_lance_writer_init_append(NanoLanceWriter* writer, const char* path, int compression_level);
int nano_lance_writer_set_ignore_nullability(NanoLanceWriter* writer, bool ignore_nullability);
int nano_lance_write_batch(NanoLanceWriter* writer, struct ArrowArray* batch, struct ArrowSchema* schema);
int nano_lance_writer_commit(NanoLanceWriter* writer, bool is_append);
int nano_lance_writer_close(NanoLanceWriter* writer);

const char* nano_lance_writer_last_error(const NanoLanceWriter* writer);
uint64_t nano_lance_writer_pending_batches(const NanoLanceWriter* writer);

#ifdef __cplusplus
}
#endif
