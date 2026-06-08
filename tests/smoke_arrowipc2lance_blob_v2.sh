#!/usr/bin/env bash
set -euo pipefail

ARROWIPC2LANCE="${1:?arrowipc2lance path}"
PYTHON="${2:?python path}"
GOLDEN_DIR="${3:?golden dir}"

if [[ ! -f "${GOLDEN_DIR}/input.arrow" ]]; then
  echo "missing golden blob v2 IPC fixture: ${GOLDEN_DIR}/input.arrow" >&2
  exit 1
fi

OUT_DIR="$(mktemp -d)"
trap 'rm -rf "${OUT_DIR}"' EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEN_SCRIPT="${SCRIPT_DIR}/generate_golden_blob_v2.py"

# Blob v2 is written entirely by nano_lance_writer (no IPC2Lance / lance-c fallback).

set +e
"${ARROWIPC2LANCE}" --ignore-nullability -c -o "${OUT_DIR}/dataset.lance" < "${GOLDEN_DIR}/input.arrow" 2>"${OUT_DIR}/arrowipc2lance.err"
STATUS=$?
set -e

if [[ "${STATUS}" -ne 0 ]]; then
  echo "arrowipc2lance blob v2 smoke failed:" >&2
  cat "${OUT_DIR}/arrowipc2lance.err" >&2
  exit "${STATUS}"
fi

if [[ ! -d "${OUT_DIR}/dataset.lance" ]]; then
  echo "expected Lance dataset directory at ${OUT_DIR}/dataset.lance" >&2
  exit 1
fi

"${PYTHON}" - <<'PY' "${OUT_DIR}/dataset.lance"
import sys
import lance

ds = lance.dataset(sys.argv[1])
blobs = ds.take_blobs("payload_ref", indices=[0])
for i, blob in enumerate(blobs):
    with blob as f:
        data = f.read(64)
    if len(data) == 0:
        raise SystemExit(f"empty blob read at index {i}")
print("blob v2 take_blobs ok", len(blobs), "rows")
PY

if [[ -f "${GEN_SCRIPT}" ]]; then
  "${PYTHON}" "${GEN_SCRIPT}" --output-dir "${GOLDEN_DIR}" --validate
fi

echo "blob v2 native external-reference smoke ok"
