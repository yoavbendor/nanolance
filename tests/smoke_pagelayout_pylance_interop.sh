#!/usr/bin/env bash
# The PageLayout parser must handle descriptors written by the Rust lance crate, not just our own.
#
# This is the evidence behind docs/OPTIMIZATION_PLAN.md section 6: nanolance cannot currently DECODE
# most stock-Lance datasets, but that is a dispatch gap, not an encoding gap. If every descriptor
# here parses, and the encodings they name are ones nanolance already implements, then re-rooting
# decode onto the descriptor is the whole job.
#
# Skips when pylance is absent.
set -euo pipefail

tool="${1:?nlance-pagelayout binary path required}"
python_bin="${2:?python interpreter path required}"

# exit 77, not 0: ctest reports 77 as SKIPPED. Exiting 0 made this test report PASS while having
# checked nothing, which is the one outcome worse than a failure.
. "$(dirname "$0")/smoke_python_deps.sh"
require_python_modules "$python_bin" pyarrow lance

tmpdir="$(mktemp -d)"
command -v cygpath >/dev/null 2>&1 && tmpdir="$(cygpath -m "$tmpdir")"
trap 'rm -rf "$tmpdir"' EXIT

"$python_bin" - <<PY
import lance, pyarrow as pa, random
random.seed(7)
n = 5000
cases = {
    "plain_int":  pa.table({"v": pa.array(range(n), type=pa.int64())}),
    "nullable":   pa.table({"v": pa.array([None if i % 3 == 0 else i for i in range(n)], type=pa.int64())}),
    "strings":    pa.table({"v": pa.array(["v%d" % random.randrange(10**6) for _ in range(n)])}),
    "low_card":   pa.table({"v": pa.array(["alpha", "beta"] * (n // 2))}),
    "floats":     pa.table({"v": pa.array([i * 0.5 for i in range(n)], type=pa.float64())}),
    "bools":      pa.table({"v": pa.array([i % 2 == 0 for i in range(n)])}),
    "timestamps": pa.table({"v": pa.array([1700000000000 + i for i in range(n)], type=pa.timestamp("ms"))}),
    "lists":      pa.table({"v": pa.array([[1, 2], [3]] * (n // 2), type=pa.list_(pa.int32()))}),
    "structs":    pa.table({"v": pa.array([{"a": i, "b": "x"} for i in range(n)])}),
    "large_str":  pa.table({"v": pa.array(["v%d" % i for i in range(n)], type=pa.large_string())}),
}
for name, table in cases.items():
    lance.write_dataset(table, "$tmpdir/" + name + ".lance", mode="overwrite")
PY

echo "--- PageLayout descriptors written by pylance ---"
# Non-zero exit means a descriptor was missing or failed to parse. Naming an encoding we cannot yet
# decode is FINE and expected here -- it prints as Unknown(field N) and is not a failure. What must
# not happen is failing to read the descriptor at all.
"$tool" "$tmpdir"/*.lance

echo "pagelayout pylance interop: every stock-Lance descriptor parsed"
