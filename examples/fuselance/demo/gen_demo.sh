#!/usr/bin/env bash
# Build the fuselance demo dataset end to end:
#   1. make_demo_arrow.py  -> three cyclic text files + an interleaved blob.v2 Arrow IPC stream
#   2. arrowipc2lance      -> a nanolance-native .lance dataset (readable by fuselance)
#
# Usage:
#   gen_demo.sh <arrowipc2lance> [output_dir]
#
# Then mount it:
#   fuselance <output_dir>/demo.lance --filename-col name
#   ls    /tmp/fuse_demo
#   cat   /tmp/fuse_demo/letters
set -euo pipefail

ARROWIPC2LANCE="${1:?path to the arrowipc2lance tool (build/arrowipc2lance)}"
OUT_DIR="${2:-/tmp/fuselance_demo}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "== generating source files + blob.v2 IPC stream =="
python3 "${SCRIPT_DIR}/make_demo_arrow.py" --output-dir "${OUT_DIR}"

echo "== converting to a nanolance-native Lance dataset =="
rm -rf "${OUT_DIR}/demo.lance"
"${ARROWIPC2LANCE}" --ignore-nullability -c -o "${OUT_DIR}/demo.lance" < "${OUT_DIR}/demo.arrow"

echo
echo "demo dataset ready: ${OUT_DIR}/demo.lance"
echo "mount it with:  fuselance ${OUT_DIR}/demo.lance --filename-col name"
