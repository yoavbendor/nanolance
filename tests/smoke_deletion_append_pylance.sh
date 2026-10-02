#!/usr/bin/env bash
# Rows deleted with pylance stay deleted after nanolance appends to the dataset.
#
# An append rewrites the manifest, fragments and all. The first hand-written protobuf encoder never
# wrote a fragment's deletion_file, so every append silently resurrected the rows pylance had
# deleted. This checks the whole path against pylance, not just the codec.
set -euo pipefail

bin="${1:?arrowipc2lance binary path required}"
python_bin="${2:?python interpreter path required}"

. "$(dirname "$0")/smoke_python_deps.sh"
require_python_modules "$python_bin" pyarrow lance

tmpdir="$(mktemp -d)"
command -v cygpath >/dev/null 2>&1 && tmpdir="$(cygpath -m "$tmpdir")"
trap 'rm -rf "$tmpdir"' EXIT

dataset_path="$tmpdir/out.lance"
ipc_path="$tmpdir/more.arrow"

"$python_bin" - <<PY
import lance
import pyarrow as pa
import pyarrow.ipc as ipc

schema = pa.schema([pa.field("id", pa.int64(), nullable=False)])
lance.write_dataset(pa.Table.from_arrays([pa.array(list(range(1, 11)), type=pa.int64())], schema=schema),
                    "$dataset_path")
lance.dataset("$dataset_path").delete("id = 2 OR id = 7")
with open("$ipc_path", "wb") as sink:
    with ipc.new_stream(sink, schema) as w:
        w.write_table(pa.Table.from_arrays([pa.array([11, 12], type=pa.int64())], schema=schema))
PY

"$bin" -o "$dataset_path" -a -l 3 < "$ipc_path"

"$python_bin" - <<PY
import lance

ds = lance.dataset("$dataset_path")
rows = sorted(ds.to_table().column("id").to_pylist())
expected = [1, 3, 4, 5, 6, 8, 9, 10, 11, 12]
assert rows == expected, f"deleted rows came back after the append: {rows}"
assert ds.count_rows() == len(expected), ds.count_rows()
assert any(f.deletion_file is not None for f in ds.get_fragments()), "the deletion file was dropped"
PY

echo "smoke_deletion_append_pylance: OK"
