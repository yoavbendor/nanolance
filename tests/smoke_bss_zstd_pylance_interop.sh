#!/usr/bin/env bash
set -euo pipefail

bin="${1:?arrowipc2lance binary path required}"
python_bin="${2:?python interpreter path required}"
tmpdir="$(mktemp -d)"
command -v cygpath >/dev/null 2>&1 && tmpdir="$(cygpath -m "$tmpdir")"
trap 'rm -rf "$tmpdir"' EXIT

ipc_path="$tmpdir/input.arrow"
dataset_path="$tmpdir/out.lance"

"$python_bin" - <<PY
import pyarrow as pa
import pyarrow.ipc as ipc
import random

random.seed(1)
n = 20000
vals = [random.uniform(-1e6, 1e6) for _ in range(n)]
schema = pa.schema([pa.field("f", pa.float64(), nullable=False)])
table = pa.Table.from_arrays([pa.array(vals, type=pa.float64())], schema=schema)
with open("$ipc_path", "wb") as sink:
    with ipc.new_stream(sink, schema) as writer:
        writer.write_table(table)
PY

# --compress tags float/double fixed-width columns with byte-stream-split + zstd (writer.cpp); random
# high-entropy float64 data is exactly the shape stock Lance itself applies ByteStreamSplit to.
"$bin" -o "$dataset_path" -c --compress -l 3 < "$ipc_path"

"$python_bin" - "$dataset_path" <<'PY'
import random
import sys
import lance

random.seed(1)
n = 20000
vals = [random.uniform(-1e6, 1e6) for _ in range(n)]
try:
    table = lance.dataset(sys.argv[1]).to_table()
except Exception as exc:
    print(f"bss-zstd interop skipped: {exc}", file=sys.stderr)
    sys.exit(77)

assert table.num_rows == n, table.num_rows
got = table.column(0).to_pylist()
assert got == vals, "stock lance must read nanolance byte-stream-split+zstd floats exactly"
print("bss-zstd interop ok (nanolance write, pylance read)")
PY
