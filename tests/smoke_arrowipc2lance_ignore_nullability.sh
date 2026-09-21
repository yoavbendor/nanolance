#!/usr/bin/env bash
# Nullability, end to end.
#
#   1. A nullable-flagged schema with no nulls writes normally, WITHOUT --ignore-nullability.
#   2. --ignore-nullability is still accepted (a no-op) so existing scripts keep working.
#   3. A fixed-width column containing REAL nulls is written with Lance's definition-level layer and
#      read back by STOCK LANCE with the nulls in the right places.
#
# This file has now asserted three different behaviours for case 3, which is the whole history of
# the bug: it first pinned the silent corruption (asserting [10, None, 30, 40] came back as
# [10, 0, 30, 40]), then the refusal that replaced it, and now the real thing.
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
# The manifest's nullable flag mirrors the ARROW SCHEMA, not whether nulls are present -- which is
# what pylance does too. This schema is nullable-flagged, so the field is nullable even though every
# value is set. (It read `not ... .nullable` while the flag was hardcoded false.)
assert table.schema.field("frame_ts").nullable
assert table.schema.field("score").nullable
assert table.column("frame_ts").null_count == 0
assert table.column("frame_ts").to_pylist() == [10, 20, 30, 40]
assert table.column("score").to_pylist() == [1.0, 2.0, 3.0, 4.0]
PY

# 3. Real nulls are stored, and stock Lance reads them back in the right places.
"$bin" -o "$tmpdir/nulls.lance" -c -l 3 < "$with_nulls"

"$python_bin" - <<PY
import lance

table = lance.dataset("$tmpdir/nulls.lance").to_table()
assert table.num_rows == 4, table.num_rows
# The manifest's nullable flag now mirrors the Arrow schema, so stock Lance sees a nullable field.
assert table.schema.field("frame_ts").nullable
assert table.column("frame_ts").to_pylist() == [10, None, 30, 40]
assert table.column("score").to_pylist() == [1.0, 2.0, None, 4.0]
PY

echo "nullability smoke: nullable schema accepted, null values stored and read back by stock Lance"
