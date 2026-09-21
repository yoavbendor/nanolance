#!/usr/bin/env bash
# What --ignore-nullability does, and what it deliberately no longer does.
#
#   1. Without the flag, a nullable-flagged schema is refused (unchanged).
#   2. With the flag, a nullable-flagged schema whose columns contain NO nulls writes normally and
#      round-trips through stock Lance. This is the case the flag exists for -- pyarrow marks
#      essentially every field nullable.
#   3. With the flag, a batch that contains a REAL null is refused with an actionable message.
#      This assertion used to run the other way: the test pinned the old behaviour of copying the
#      null slot's raw bytes, asserting [10, None, 30, 40] came back as [10, 0, 30, 40]. That was
#      silent data loss, and stock Lance read the wrong values back without complaint.
set -euo pipefail

bin="${1:?arrowipc2lance binary path required}"
python_bin="${2:?python interpreter path required}"
tmpdir="$(mktemp -d)"
# A non-MSYS (Windows) python/exe needs a native path; mktemp yields a POSIX /tmp path. cygpath bridges it
# on Git-Bash/MSYS, and is absent on Linux (so this is a no-op there).
command -v cygpath >/dev/null 2>&1 && tmpdir="$(cygpath -m "$tmpdir")"
trap 'rm -rf "$tmpdir"' EXIT

with_nulls="$tmpdir/with_nulls.arrow"
no_nulls="$tmpdir/no_nulls.arrow"
dataset_path="$tmpdir/out.lance"

"$python_bin" - <<PY
import pyarrow as pa
import pyarrow.ipc as ipc

# Nullable-flagged schema, used for both files. Only the DATA differs.
schema = pa.schema([
    pa.field("frame_ts", pa.int64(), nullable=True),
    pa.field("score", pa.float64(), nullable=True),
])

with_nulls = pa.Table.from_arrays(
    [
        pa.array([10, None, 30, 40], type=pa.int64()),
        pa.array([1.0, 2.0, None, 4.0], type=pa.float64()),
    ],
    schema=schema,
)
no_nulls = pa.Table.from_arrays(
    [
        pa.array([10, 20, 30, 40], type=pa.int64()),
        pa.array([1.0, 2.0, 3.0, 4.0], type=pa.float64()),
    ],
    schema=schema,
)
for path, table in (("$with_nulls", with_nulls), ("$no_nulls", no_nulls)):
    with open(path, "wb") as sink:
        with ipc.new_stream(sink, schema) as writer:
            writer.write_table(table)
PY

# 1. No flag -> refused, and the message names the flag.
if "$bin" -o "$tmpdir/should_fail.lance" -c -l 3 < "$no_nulls" 2>"$tmpdir/no_flag.err"; then
    echo "nullable schema unexpectedly succeeded without --ignore-nullability" >&2
    exit 1
fi
grep -q -- "--ignore-nullability" "$tmpdir/no_flag.err"

# 2. Flag + no actual nulls -> writes, and stock Lance reads the real values back.
"$bin" --ignore-nullability -o "$dataset_path" -c -l 3 < "$no_nulls"

"$python_bin" - <<PY
import lance

table = lance.dataset("$dataset_path").to_table()
assert table.num_rows == 4, table.num_rows
assert not table.schema.field("frame_ts").nullable
assert not table.schema.field("score").nullable
assert table.column("frame_ts").to_pylist() == [10, 20, 30, 40]
assert table.column("score").to_pylist() == [1.0, 2.0, 3.0, 4.0]
PY

# 3. Flag + an actual null -> refused. The flag accepts a nullable *schema*; it must never silently
#    drop a null *value*.
if "$bin" --ignore-nullability -o "$tmpdir/nulls.lance" -c -l 3 < "$with_nulls" \
        2>"$tmpdir/nulls.err"; then
    echo "batch containing real nulls unexpectedly succeeded" >&2
    exit 1
fi
# The message must name the offending column, the row, and the remedy.
grep -q "frame_ts" "$tmpdir/nulls.err"
grep -q "null at row 1" "$tmpdir/nulls.err"
grep -q "fill_null" "$tmpdir/nulls.err"
# And nothing may have been left behind for a reader to pick up.
if [ -e "$tmpdir/nulls.lance" ]; then
    echo "a rejected write left a dataset behind at $tmpdir/nulls.lance" >&2
    exit 1
fi

echo "ignore_nullability smoke: schema accepted, null values refused"
