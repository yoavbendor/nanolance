#!/usr/bin/env bash
set -euo pipefail

bin="${1:?arrowipc2lance binary path required}"
python_bin="${2:?python interpreter path required}"

. "$(dirname "$0")/smoke_python_deps.sh"
require_python_modules "$python_bin" pyarrow
tmpdir="$(mktemp -d)"
# A non-MSYS (Windows) python/exe needs a native path; mktemp yields a POSIX /tmp path. cygpath bridges it
# on Git-Bash/MSYS, and is absent on Linux (so this is a no-op there).
command -v cygpath >/dev/null 2>&1 && tmpdir="$(cygpath -m "$tmpdir")"
trap 'rm -rf "$tmpdir"' EXIT

ipc_a="$tmpdir/a.arrow"
ipc_b="$tmpdir/b.arrow"
dataset_path="$tmpdir/out.lance"

"$python_bin" - <<PY
import pyarrow as pa
import pyarrow.ipc as ipc

schema = pa.schema([pa.field("id", pa.int64(), nullable=False)])
t1 = pa.Table.from_arrays([pa.array([1, 2], type=pa.int64())], schema=schema)
with open("$ipc_a", "wb") as sink:
    with ipc.new_stream(sink, schema) as w:
        w.write_table(t1)
t2 = pa.Table.from_arrays([pa.array([3], type=pa.int64())], schema=schema)
with open("$ipc_b", "wb") as sink:
    with ipc.new_stream(sink, schema) as w:
        w.write_table(t2)
PY

"$bin" -o "$dataset_path" -c -l 3 < "$ipc_a"
"$bin" -o "$dataset_path" -a -l 3 < "$ipc_b"

"$python_bin" - <<PY
import lance

ds = lance.dataset("$dataset_path")
t = ds.to_table()
rows = t.column("id").to_pylist()
assert rows == [1, 2, 3], rows
PY

echo "smoke_arrowipc2lance_append: OK"
