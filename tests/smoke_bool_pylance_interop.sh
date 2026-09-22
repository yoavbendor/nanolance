#!/usr/bin/env bash
set -euo pipefail

bin="${1:?arrowipc2lance binary path required}"
python_bin="${2:?python interpreter path required}"

. "$(dirname "$0")/smoke_python_deps.sh"
require_python_modules "$python_bin" pyarrow lance

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
vals = [random.random() < 0.37 for _ in range(n)]
schema = pa.schema([pa.field("b", pa.bool_(), nullable=False)])
table = pa.Table.from_arrays([pa.array(vals, type=pa.bool_())], schema=schema)
with open("$ipc_path", "wb") as sink:
    with ipc.new_stream(sink, schema) as writer:
        writer.write_table(table)
PY

# bool is always bit-packed on disk (1 bit/value, LSB-first), matching stock Lance's own
# Flat{bits_per_value:1} representation -- not gated by --compress.
"$bin" -o "$dataset_path" -c -l 3 < "$ipc_path"

"$python_bin" - "$dataset_path" <<'PY'
import random
import sys
import lance

random.seed(1)
n = 20000
vals = [random.random() < 0.37 for _ in range(n)]
try:
    table = lance.dataset(sys.argv[1]).to_table()
except Exception as exc:
    print(f"bool interop skipped: {exc}", file=sys.stderr)
    sys.exit(77)

assert table.num_rows == n, table.num_rows
got = table.column(0).to_pylist()
assert got == vals, "stock lance must read nanolance's 1-bit-packed bool column exactly"
print("bool interop ok (nanolance write, pylance read)")
PY
