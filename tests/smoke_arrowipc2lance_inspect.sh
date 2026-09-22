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

ipc_path="$tmpdir/input.arrow"
dataset_path="$tmpdir/out.lance"

"$python_bin" - <<PY
import pyarrow as pa
import pyarrow.ipc as ipc

schema = pa.schema([pa.field("id", pa.int64(), nullable=False)])
table = pa.Table.from_arrays([pa.array([1, 2], type=pa.int64())], schema=schema)
with open("$ipc_path", "wb") as sink:
    with ipc.new_stream(sink, schema) as writer:
        writer.write_table(table)
PY

"$bin" -o "$dataset_path" -c -l 3 < "$ipc_path"

out="$("$bin" --inspect "$dataset_path")"
echo "$out" | grep -q '"manifest_version": 1' || {
  echo "inspect output missing manifest_version" >&2
  echo "$out" >&2
  exit 1
}
echo "smoke_arrowipc2lance_inspect: OK"
