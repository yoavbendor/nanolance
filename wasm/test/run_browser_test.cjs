// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Browser test for weblance. Serves wasm/test and the built module with range_server.py, opens
// test.html in headless Chromium, and checks the worker's report.
//
//   local: a small dataset written by make_local_dataset.py, served by the Range server (no proxy).
//   hf:    the public Hugging Face dataset, reached through HTTPS_PROXY. The proxy re-signs TLS, so
//          this test (only) ignores certificate errors.
//
// Usage: NODE_PATH=<global node_modules> PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers \
//        node run_browser_test.cjs local|hf WORKDIR WASM_DIR
// Writes WORKDIR/report-<mode>.json and prints the numbers. Exit status 1 on any failed check.

const assert = require('assert');
const { spawn, execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const readline = require('readline');
const { chromium } = require('playwright');

const [mode, workdir, wasmDir] = process.argv.slice(2);
if (!['local', 'hf'].includes(mode) || !workdir || !wasmDir) {
  console.error('usage: run_browser_test.cjs local|hf WORKDIR WASM_DIR');
  process.exit(2);
}
const testDir = __dirname;
const site = path.join(workdir, `site-${mode}`);
const HF_DATASET =
  'https://huggingface.co/datasets/jrmiller/coco-2017-siglip2-embeddings/resolve/main/lancedb/coco_clip_embeddings.lance';
const KIB = 1024;

function stageSite() {
  fs.rmSync(site, { recursive: true, force: true });
  fs.mkdirSync(site, { recursive: true });
  for (const f of ['test.html', 'worker.js']) fs.copyFileSync(path.join(testDir, f), path.join(site, f));
  for (const f of ['weblance.js', 'weblance.wasm']) fs.copyFileSync(path.join(wasmDir, f), path.join(site, f));
}

function startRangeServer() {
  const log = fs.createWriteStream(path.join(workdir, `range-${mode}.log`));
  const child = spawn('python3', [path.join(testDir, 'range_server.py'), site, '0'], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  child.stderr.pipe(log);
  return new Promise((resolve, reject) => {
    const rl = readline.createInterface({ input: child.stdout });
    rl.once('line', (line) => {
      const m = /^PORT (\d+)$/.exec(line);
      if (!m) return reject(new Error('range server said: ' + line));
      resolve({ child, port: Number(m[1]), logPath: path.join(workdir, `range-${mode}.log`) });
    });
    child.once('exit', (code) => reject(new Error('range server exited ' + code)));
  });
}

function mib(n) {
  return (n / (1024 * 1024)).toFixed(3) + ' MiB';
}

async function main() {
  stageSite();
  let expected = null;
  let dataset;
  let rows = '0,1,100000,287359';
  if (mode === 'local') {
    const localDir = path.join(workdir, 'local');
    if (!fs.existsSync(path.join(localDir, 'expected.json'))) {
      execFileSync('python3', [path.join(testDir, 'make_local_dataset.py'), localDir], { stdio: 'inherit' });
    }
    fs.symlinkSync(localDir, path.join(site, 'ds'));
    expected = JSON.parse(fs.readFileSync(path.join(localDir, 'expected.json'), 'utf8'));
    rows = '0,1,250,499';
  }
  const server = await startRangeServer();
  if (mode === 'local') {
    dataset = `http://127.0.0.1:${server.port}/ds/coco_small.lance`;
  } else {
    dataset = HF_DATASET;
  }

  const launchOpts = { headless: true };
  const contextOpts = {};
  if (mode === 'hf') {
    // Set on the context, not the launch: Chromium then honors the bypass, and the page and the
    // Range server (loopback) stay off the proxy.
    contextOpts.proxy = { server: process.env.HTTPS_PROXY, bypass: '127.0.0.1,localhost' };
    contextOpts.ignoreHTTPSErrors = true;
  }
  const browser = await chromium.launch(launchOpts);
  const failures = [];
  let report;
  let pageSeconds;
  try {
    const context = await browser.newContext(contextOpts);
    const page = await context.newPage();
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    page.on('console', (m) => {
      if (m.type() === 'error') pageErrors.push('console: ' + m.text());
    });
    const query = new URLSearchParams({ ds: dataset, rows });
    const t0 = Date.now();
    await page.goto(`http://127.0.0.1:${server.port}/test.html?${query}`);
    await page.waitForFunction(() => window.__result !== undefined, null, { timeout: 900000, polling: 200 });
    pageSeconds = (Date.now() - t0) / 1000;
    report = await page.evaluate(() => window.__result);
    if (pageErrors.length) console.log('page errors:', pageErrors.slice(0, 5));
  } finally {
    await browser.close();
    server.child.kill();
  }

  // Checks. Each failure is collected so one run shows all of them.
  const check = (name, fn) => {
    try {
      fn();
    } catch (e) {
      failures.push(`${name}: ${e.message.split('\n')[0]}`);
    }
  };
  check('ok', () => assert.strictEqual(report.ok, true, report.error));
  check('image decodes in the page', () => assert.strictEqual(report.imageDecoded, true));
  check('step list', () =>
    assert.deepStrictEqual(report.steps.map((s) => s.name), ['load module', 'open', 'info', 'take scalars', 'take image']));
  const step = (name) => report.steps.find((s) => s.name === name);

  if (mode === 'local') {
    check('local info', () => {
      assert.strictEqual(report.info.rows, expected.rows.length);
      assert.strictEqual(report.info.version, expected.version);
    });
    check('local scalar rows', () => {
      assert.strictEqual(report.rows.length, 4);
      rows.split(',').map(Number).forEach((r, i) => {
        const want = expected.rows[r];
        const got = report.rows[i];
        for (const k of ['image_id', 'file_name', 'caption', 'coco_url', 'split', 'width', 'height']) {
          assert.deepStrictEqual(got[k], want[k], `row ${r} ${k}`);
        }
      });
    });
    check('local image bytes', () => {
      const r = Number(rows.split(',')[2]);
      assert.strictEqual(report.imageRow, r);
      assert.strictEqual(report.imageLen, expected.rows[r].image_len);
      assert.strictEqual(report.imageSha256, expected.rows[r].image_sha256);
    });
  } else {
    check('hf info', () => {
      assert.strictEqual(report.info.rows, 287360);
      const names = report.info.fields.map((f) => f.name);
      for (const n of ['image_id', 'file_name', 'caption', 'coco_url', 'split', 'width', 'height', 'vector', 'image_bytes']) {
        assert.ok(names.includes(n), 'missing column ' + n);
      }
    });
    check('hf scalar rows', () => {
      assert.strictEqual(report.rows.length, 4);
      for (const r of report.rows) {
        assert.strictEqual(typeof r.image_id, 'number');
        assert.strictEqual(typeof r.caption, 'string');
        // COCO's unlabeled split has no captions: an empty caption is correct there, not a failure.
        if (r.split !== 'unlabeled') assert.ok(r.caption.length > 0, 'empty caption for ' + r.split);
      }
    });
    check('hf image is a JPEG', () => assert.strictEqual(report.imageType, 'image/jpeg'));
  }

  // The numbers.
  const summary = {
    mode,
    dataset,
    pageSeconds,
    imageRow: report.imageRow,
    imageLen: report.imageLen,
    info: report.info,
    steps: report.steps.map((s) => ({
      name: s.name,
      wallMs: Math.round(s.wallMs),
      libMs: s.ms,
      requests: s.requests,
      bytes: s.bytes,
      listings: s.listings,
      fallbacks: s.fallbacks,
    })),
    failures,
  };
  if (mode === 'local') {
    const lines = fs.readFileSync(server.logPath, 'utf8').trim().split('\n');
    summary.serverRequests = lines.filter((l) => l.startsWith('GET ')).length;
    summary.serverBytes = lines.reduce((a, l) => a + Number(l.split(' ').pop() || 0), 0);
  }
  fs.writeFileSync(path.join(workdir, `report-${mode}.json`), JSON.stringify({ ...summary, report: { ...report, rows: report.rows } }, null, 2));

  console.log(`mode ${mode}: page ${pageSeconds.toFixed(2)} s, image row ${report.imageRow}, image ${report.imageLen} bytes`);
  for (const s of summary.steps) {
    console.log(
      `  ${s.name.padEnd(13)} wall ${String(s.wallMs).padStart(6)} ms  lib ${String(s.libMs).padStart(7)} ms  ` +
        `requests ${String(s.requests).padStart(3)}  bytes ${String(s.bytes).padStart(9)} (${mib(s.bytes)})` +
        (s.listings ? `  listings ${s.listings}` : '') +
        (s.fallbacks ? `  fallbacks ${s.fallbacks}` : '')
    );
  }
  if (mode === 'local') console.log(`  range server: ${summary.serverRequests} GETs, ${mib(summary.serverBytes)} sent`);
  if (failures.length) {
    console.log('FAILED:\n  ' + failures.join('\n  '));
    process.exit(1);
  }
  console.log('all checks passed');
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
