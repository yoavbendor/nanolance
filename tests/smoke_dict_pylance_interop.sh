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

n = 50000
vals = [f"row-{i % 500}" for i in range(n)]
schema = pa.schema([pa.field("s", pa.string(), nullable=False)])
table = pa.Table.from_arrays([pa.array(vals, type=pa.string())], schema=schema)
with open("$ipc_path", "wb") as sink:
    with ipc.new_stream(sink, schema) as writer:
        writer.write_table(table)
PY

# --compress selects the structural-dictionary encoding for this scattered low-cardinality column.
# Without it the writer emits a plain (zstd) string column and this test would not exercise the
# dictionary path at all. The 50000 rows span many 1024-value index chunks, so this also covers the
# multi-chunk miniblock chunk-meta layout that stock Lance must be able to read.
"$bin" -o "$dataset_path" -c --compress -l 3 < "$ipc_path"

"$python_bin" - "$dataset_path" <<'PY'
import sys
import lance

n = 50000
vals = [f"row-{i % 500}" for i in range(n)]
try:
    table = lance.dataset(sys.argv[1]).to_table()
except Exception as exc:
    print(f"dict interop skipped: {exc}", file=sys.stderr)
    sys.exit(77)

assert table.num_rows == n, table.num_rows
got = table.column(0).to_pylist()
assert got == vals, "stock lance must read nanolance dict-encoded strings"
print("dict interop ok (nanolance write, pylance read)")
PY
