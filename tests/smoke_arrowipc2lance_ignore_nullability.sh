#!/usr/bin/env bash
# Nullability: what is accepted, and what is refused.
#
#   1. A nullable-flagged schema with NO nulls writes normally, WITHOUT --ignore-nullability, and
#      round-trips through stock Lance. pyarrow marks essentially every field nullable, so this is
#      the overwhelmingly common shape; requiring a flag for it rejected almost every real table.
#   2. --ignore-nullability is still accepted (now a no-op) so existing scripts keep working.
#   3. A batch containing a REAL null is refused, with or without the flag, and says which column,
#      which row and what to do. This assertion used to run the other way: the test pinned the old
#      behaviour of copying the null slot's raw bytes, asserting [10, None, 30, 40] came back as
#      [10, 0, 30, 40]. That was silent data loss, and stock Lance read the wrong values back
#      without complaint.
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

# 1. No flag, nullable schema, no null values -> writes. This is the pyarrow default shape.
"$bin" -o "$dataset_path" -c -l 3 < "$no_nulls"

# 2. The legacy flag is still accepted (no-op) so existing scripts do not break.
"$bin" --ignore-nullability -o "$tmpdir/legacy_flag.lance" -c -l 3 < "$no_nulls"

"$python_bin" - <<PY
import lance

table = lance.dataset("$dataset_path").to_table()
assert table.num_rows == 4, table.num_rows
assert not table.schema.field("frame_ts").nullable
assert not table.schema.field("score").nullable
assert table.column("frame_ts").to_pylist() == [10, 20, 30, 40]
assert table.column("score").to_pylist() == [1.0, 2.0, 3.0, 4.0]
PY

# 3. An actual null -> refused, whether or not the legacy flag is passed. Accepting a nullable
#    *schema* must never mean silently dropping a null *value*.
for flag in "" "--ignore-nullability"; do
    if "$bin" $flag -o "$tmpdir/nulls.lance" -c -l 3 < "$with_nulls" 2>"$tmpdir/nulls.err"; then
        echo "batch containing real nulls unexpectedly succeeded (flag='$flag')" >&2
        exit 1
    fi
done
# The message must name the offending column, the row, and the remedy.
grep -q "frame_ts" "$tmpdir/nulls.err"
grep -q "null at row 1" "$tmpdir/nulls.err"
grep -q "fill_null" "$tmpdir/nulls.err"
# And nothing may have been left behind for a reader to pick up.
if [ -e "$tmpdir/nulls.lance" ]; then
    echo "a rejected write left a dataset behind at $tmpdir/nulls.lance" >&2
    exit 1
fi

echo "nullability smoke: nullable schema accepted by default, null values refused"
