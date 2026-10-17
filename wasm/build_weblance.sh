#!/bin/sh
# Builds wasm/weblance_glue.cpp (weblance: nanolance's Lance reader for a Web Worker) against the
# single-threaded Emscripten build of nanolance in build-wasm/ (see the wasm notes in AGENTS.md).
# Usage: wasm/build_weblance.sh OUTDIR
#   writes OUTDIR/weblance.js and OUTDIR/weblance.wasm; load in a worker with importScripts and
#   createWeblance().
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$1
[ -n "$OUT" ] || { echo "usage: $0 OUTDIR" >&2; exit 2; }
mkdir -p "$OUT"
B="${WASM_BUILD:-$ROOT/build-wasm}"
NA="$ROOT/.deps/nanoarrow-src/src"
[ -f "$B/libnanolance.a" ] || { echo "no $B/libnanolance.a: configure and build build-wasm first" >&2; exit 1; }

EXPORTS='_wl_open,_wl_error,_wl_info_json,_wl_take_json,_wl_take_binary,_wl_stats_json,_wl_reset_stats,_wl_free,_malloc,_free'
RUNTIME='ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8'

em++ -std=c++20 -Oz -DNDEBUG ${WL_CXXFLAGS:-} -I"$ROOT/include" -I"$ROOT/generated" -I"$B/include" -I"$NA" -I"$B/_nanolance_nanoarrow_build/src" \
  "$ROOT/wasm/weblance_glue.cpp" -o "$OUT/weblance.js" \
  -Wl,--start-group "$B/libnanolance.a" "$B/libnanolance_reader.a" "$B/libnanolance_proto.a" \
  "$B/_nanolance_nanoarrow_build/libnanoarrow_ipc_static.a" "$B/_nanolance_nanoarrow_build/libnanoarrow_static.a" \
  "$B/_nanolance_nanoarrow_build/libflatccrt.a" "$B/_nanolance_zstd_build/lib/libzstd.a" -Wl,--end-group \
  -sMODULARIZE=1 -sEXPORT_NAME=createWeblance -sENVIRONMENT=worker -sALLOW_MEMORY_GROWTH=1 \
  -sSTACK_SIZE=1048576 -sINVOKE_RUN=0 \
  "-sEXPORTED_FUNCTIONS=[$EXPORTS]" "-sEXPORTED_RUNTIME_METHODS=[$RUNTIME]"
ls -l "$OUT/weblance.js" "$OUT/weblance.wasm" | awk '{print $5, $9}'
