#!/usr/bin/env bash
set -euo pipefail

bin="${1:?arrowipc2lance binary path required}"
python_bin="${2:?python interpreter path required}"
tmpdir="$(mktemp -d)"
# A non-MSYS (Windows) python/exe needs a native path; mktemp yields a POSIX /tmp path. cygpath bridges it
# on Git-Bash/MSYS, and is absent on Linux (so this is a no-op there).
command -v cygpath >/dev/null 2>&1 && tmpdir="$(cygpath -m "$tmpdir")"
trap 'rm -rf "$tmpdir"' EXIT

ipc_path="$tmpdir/input.arrow"
dataset_path="$tmpdir/out.lance"

"$python_bin" - <<PY
import pyarrow as pa
import pyarrow.ipc as ipc

schema = pa.schema([
    pa.field("frame_ts", pa.int64(), nullable=True),
    pa.field("score", pa.float64(), nullable=True),
])
table = pa.Table.from_arrays(
    [
        pa.array([10, None, 30, 40], type=pa.int64()),
        pa.array([1.0, 2.0, None, 4.0], type=pa.float64()),
    ],
    schema=schema,
)
with open("$ipc_path", "wb") as sink:
    with ipc.new_stream(sink, schema) as writer:
        writer.write_table(table)
PY

if "$bin" -o "$tmpdir/should_fail.lance" -c -l 3 < "$ipc_path" 2>"$tmpdir/no_flag.err"; then
    echo "nullable input unexpectedly succeeded without --ignore-nullability" >&2
    exit 1
fi
grep -q -- "--ignore-nullability" "$tmpdir/no_flag.err"

"$bin" --ignore-nullability -o "$dataset_path" -c -l 3 < "$ipc_path"

"$python_bin" - <<PY
import lance

table = lance.dataset("$dataset_path").to_table()
assert table.num_rows == 4
assert not table.schema.field("frame_ts").nullable
assert not table.schema.field("score").nullable
# Values are copied from Arrow value buffers (null slots keep prior bytes).
assert table.column("frame_ts").to_pylist() == [10, 0, 30, 40]
assert table.column("score").to_pylist() == [1.0, 2.0, 0.0, 4.0]
PY
