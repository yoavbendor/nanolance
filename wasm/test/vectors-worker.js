// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Test worker for wl_take_vectors: opens the dataset, takes the given rows of one fixed_size_list
// column, and reports the vectors' float32 bits (hex, row-major) and the module's counters.
// Message in: {ds, rows, column}. Message out: {ok, error, dim, hex, stats}.

importScripts('weblance.js');

function utf8(M, s) {
  const n = M.lengthBytesUTF8(s) + 1;
  const p = M._malloc(n);
  M.stringToUTF8(s, p, n);
  return p;
}

self.onmessage = async (ev) => {
  const { ds, rows, column } = ev.data;
  const result = { ok: false, error: '' };
  try {
    const M = await createWeblance();
    const p = utf8(M, ds);
    const h = M.ccall('wl_open', 'number', ['number'], [p]);
    M._free(p);
    if (!h) throw new Error('wl_open: ' + M.ccall('wl_error', 'string', [], []));
    M.ccall('wl_reset_stats', null, [], []);
    const arr = M._malloc(8 * rows.length);
    new BigInt64Array(M.HEAPU8.buffer, arr, rows.length).set(BigInt64Array.from(rows, (r) => BigInt(r)));
    const col = utf8(M, column);
    const dimPtr = M._malloc(4);
    const vp = M._wl_take_vectors(h, arr, rows.length, col, dimPtr);
    const dim = new Int32Array(M.HEAPU8.buffer, dimPtr, 1)[0];
    if (!vp) throw new Error('wl_take_vectors: ' + M.ccall('wl_error', 'string', [], []));
    const bytes = M.HEAPU8.slice(vp, vp + rows.length * dim * 4);
    const hex = Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
    const sp = M.ccall('wl_stats_json', 'number', [], []);
    const stats = JSON.parse(M.UTF8ToString(sp));
    M._wl_free(sp);
    M._wl_free(vp);
    result.dim = dim;
    result.hex = hex;
    result.stats = stats;
    result.ok = true;
  } catch (e) {
    result.error = String((e && e.message) || e);
  }
  self.postMessage(result);
};
