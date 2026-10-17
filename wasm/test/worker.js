// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Runs weblance in a Web Worker: opens the dataset the page names, takes rows, and reports each step's
// wall time and the request/byte counters the module keeps. Message in: {ds, rows, columns}.
// Message out: {ok, error, steps, info, rows, image, imageType, imageSha256, imageLen}.

importScripts('weblance.js');

const DEFAULT_COLUMNS = 'image_id,file_name,caption,coco_url,split,width,height';

function utf8(M, s) {
  const n = M.lengthBytesUTF8(s) + 1;
  const p = M._malloc(n);
  M.stringToUTF8(s, p, n);
  return p;
}

function readJson(M, ptr) {
  if (!ptr) return null;
  const text = M.UTF8ToString(ptr);
  M._wl_free(ptr);
  return text;
}

function stats(M) {
  const ptr = M.ccall('wl_stats_json', 'number', [], []);
  return JSON.parse(readJson(M, ptr));
}

function lastError(M) {
  return M.ccall('wl_error', 'string', [], []) || 'unknown error';
}

async function sha256Hex(bytes) {
  const digest = await crypto.subtle.digest('SHA-256', bytes);
  return Array.from(new Uint8Array(digest), (b) => b.toString(16).padStart(2, '0')).join('');
}

function sniffImageType(b) {
  if (b[0] === 0xff && b[1] === 0xd8) return 'image/jpeg';
  if (b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e && b[3] === 0x47) return 'image/png';
  return 'application/octet-stream';
}

self.onmessage = async (ev) => {
  const { ds, rows, columns } = ev.data;
  const steps = [];
  const result = { ok: false, error: '', steps, info: null, rows: null, image: null };
  let M = null;

  // Runs fn, times it, and records the counters it caused.
  async function step(name, fn) {
    M.ccall('wl_reset_stats', null, [], []);
    const t0 = performance.now();
    const value = await fn();
    const wallMs = performance.now() - t0;
    steps.push({ name, wallMs, ...stats(M) });
    return value;
  }

  try {
    const t0 = performance.now();
    M = await createWeblance();
    steps.push({ name: 'load module', wallMs: performance.now() - t0, requests: 0, bytes: 0, ms: 0 });

    const rowList = rows.split(',').map((r) => Number(r.trim()));
    const h = await step('open', () => {
      const p = utf8(M, ds);
      const handle = M.ccall('wl_open', 'number', ['number'], [p]);
      M._free(p);
      if (!handle) throw new Error('wl_open: ' + lastError(M));
      return handle;
    });
    result.info = JSON.parse(await step('info', () => readJson(M, M.ccall('wl_info_json', 'number', ['number'], [h]))));

    result.rows = await step('take scalars', () => {
      const n = rowList.length;
      const arr = M._malloc(8 * n);
      new BigInt64Array(M.HEAPU8.buffer, arr, n).set(BigInt64Array.from(rowList, (r) => BigInt(r)));
      const cols = utf8(M, columns || DEFAULT_COLUMNS);
      const text = readJson(M, M.ccall('wl_take_json', 'number', ['number', 'number', 'number', 'number'], [h, arr, n, cols]));
      M._free(arr);
      M._free(cols);
      if (text === null) throw new Error('wl_take_json: ' + lastError(M));
      return JSON.parse(text);
    });

    // One image: the third requested row.
    const imageRow = rowList[2];
    const bytes = await step('take image', async () => {
      const lenPtr = M._malloc(4);
      const col = utf8(M, 'image_bytes');
      const ptr = M._wl_take_binary(h, BigInt(imageRow), col, lenPtr);
      const len = new Int32Array(M.HEAPU8.buffer, lenPtr, 1)[0];
      M._free(lenPtr);
      M._free(col);
      if (!ptr || len < 0) throw new Error('wl_take_binary: ' + lastError(M));
      const copy = M.HEAPU8.slice(ptr, ptr + len);
      M._wl_free(ptr);
      return copy;
    });
    result.imageLen = bytes.length;
    result.imageRow = imageRow;
    result.imageType = sniffImageType(bytes);
    result.imageSha256 = await sha256Hex(bytes);
    result.image = bytes.buffer;
    result.ok = true;
  } catch (e) {
    result.error = String(e && e.message ? e.message : e);
  }
  if (result.image) {
    self.postMessage(result, [result.image]);
  } else {
    self.postMessage(result);
  }
};
