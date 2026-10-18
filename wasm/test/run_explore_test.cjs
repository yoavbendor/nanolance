// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor
//
// Browser test for the explore page of weblance and for wl_take_vectors. Stages the page (from
// WEB_DIR, the weblance checkout) and the built module (WASM_DIR) behind range_server.py, then:
//
//   local: a 3,000-row dataset from make_explore_dataset.py. Checks wl_take_vectors bit for bit against
//          pylance on rows 0, 1, 1500, 2999; the page's detected columns; that PCA separates the five
//          classes; that a click shows 12 decoded images; and that the phone-width page does not scroll.
//   hf:    the explore page's real flow (Hugging Face discovery, sample, map) for MNIST and Food-101,
//          through HTTPS_PROXY, with map screenshots. Run once, at the end.
//
// Usage: NODE_PATH=<global node_modules> PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers \
//        node run_explore_test.cjs local|hf WORKDIR WASM_DIR WEB_DIR
// Writes WORKDIR/explore-<mode>.json. Exit status 1 on any failed check.

const assert = require('assert');
const { spawn, execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const readline = require('readline');
const { chromium } = require('playwright');

const [mode, workdir, wasmDir, webDir] = process.argv.slice(2);
if (!['local', 'hf'].includes(mode) || !workdir || !wasmDir || !webDir) {
  console.error('usage: run_explore_test.cjs local|hf WORKDIR WASM_DIR WEB_DIR');
  process.exit(2);
}
const testDir = __dirname;
const site = path.join(workdir, `site-${mode}`);
const PAGE_FILES = ['explore.html', 'explore.js', 'explore-worker.js', 'style.css', 'index.html'];
const TEST_FILES = ['vectors.html', 'vectors-worker.js'];
const HF_RUNS = [
  { name: 'mnist', ds: 'lance-format/mnist-lance' },
  { name: 'food101', ds: 'lance-format/food101-lance' },
];

function stageSite() {
  fs.rmSync(site, { recursive: true, force: true });
  fs.mkdirSync(path.join(site, 'lib'), { recursive: true });
  for (const f of PAGE_FILES) fs.copyFileSync(path.join(webDir, f), path.join(site, f));
  for (const f of TEST_FILES) fs.copyFileSync(path.join(testDir, f), path.join(site, f));
  for (const f of ['weblance.js', 'weblance.wasm']) fs.copyFileSync(path.join(wasmDir, f), path.join(site, 'lib', f));
  // The vectors test loads the module next to itself.
  for (const f of ['weblance.js', 'weblance.wasm']) fs.copyFileSync(path.join(wasmDir, f), path.join(site, f));
}

function startRangeServer() {
  const logPath = path.join(workdir, `range-explore-${mode}.log`);
  const log = fs.openSync(logPath, 'w');
  const child = spawn('python3', [path.join(testDir, 'range_server.py'), site, '0'], {
    stdio: ['ignore', 'pipe', log],
  });
  return new Promise((resolve, reject) => {
    const rl = readline.createInterface({ input: child.stdout });
    rl.once('line', (line) => {
      const m = /^PORT (\d+)$/.exec(line);
      if (!m) return reject(new Error('range server said: ' + line));
      resolve({ child, port: Number(m[1]), logPath });
    });
    child.once('exit', (code) => reject(new Error('range server exited ' + code)));
  });
}

function expectedRows() {
  return JSON.parse(fs.readFileSync(path.join(workdir, 'local', 'expected.json'), 'utf8'));
}

function hexOfFloat32(values) {
  const buf = Buffer.alloc(values.length * 4);
  values.forEach((v, i) => buf.writeFloatLE(v, i * 4));
  return buf.toString('hex');
}

// The explore page's report, once the sample is read (or the page shows a message).
async function waitForExplore(page, timeout) {
  await page.waitForFunction(
    () => window.__explore !== undefined || (document.getElementById('message') && !document.getElementById('message').hidden),
    null,
    { timeout, polling: 200 }
  );
  const message = await page.evaluate(() => {
    const m = document.getElementById('message');
    return m && !m.hidden ? m.textContent : '';
  });
  const report = await page.evaluate(() => window.__explore || null);
  return { report, message };
}

async function main() {
  stageSite();
  const failures = [];
  const check = (name, fn) => {
    try {
      fn();
    } catch (e) {
      failures.push(`${name}: ${e.message.split('\n')[0]}`);
    }
  };
  const out = { mode, steps: {} };
  let server = null;
  let browser = null;
  try {
    let base;
    let expected = null;
    let localRows = [0, 1, 1500, 2999];
    if (mode === 'local') {
      const localDir = path.join(workdir, 'local');
      if (!fs.existsSync(path.join(localDir, 'expected.json'))) {
        execFileSync('python3', [path.join(testDir, 'make_explore_dataset.py'), localDir], { stdio: 'inherit' });
      }
      fs.symlinkSync(localDir, path.join(site, 'ds'));
      expected = expectedRows();
    }
    server = await startRangeServer();
    base = `http://127.0.0.1:${server.port}`;

    const contextOpts = {};
    if (mode === 'hf') {
      // The page and the Range server (loopback) stay off the proxy; Hugging Face goes through it.
      contextOpts.proxy = { server: process.env.HTTPS_PROXY, bypass: '127.0.0.1,localhost' };
      contextOpts.ignoreHTTPSErrors = true;
    }
    browser = await chromium.launch({ headless: true });

    if (mode === 'local') {
      // 1. wl_take_vectors, bit for bit.
      const ctx = await browser.newContext();
      const page = await ctx.newPage();
      const ds = `${base}/ds/explore_small.lance`;
      const q = new URLSearchParams({ ds, rows: localRows.join(','), column: 'image_emb' });
      await page.goto(`${base}/vectors.html?${q}`);
      await page.waitForFunction(() => window.__result !== undefined, null, { timeout: 300000, polling: 200 });
      const vr = await page.evaluate(() => window.__result);
      await ctx.close();
      out.steps.vectors = vr;
      check('wl_take_vectors ok', () => assert.strictEqual(vr.ok, true, vr.error));
      check('wl_take_vectors dim', () => assert.strictEqual(vr.dim, expected.dim));
      check('wl_take_vectors bits', () => {
        const got = vr.hex.match(/.{8}/g);
        localRows.forEach((r, i) => {
          const want = expected.rows[r].vector_hex;
          const have = got.slice(i * expected.dim, (i + 1) * expected.dim).join('');
          assert.strictEqual(have, want, `row ${r}`);
        });
      });

      // 2. The explore page on the local table: detected columns, the sample, the map.
      const ctx2 = await browser.newContext({ viewport: { width: 1280, height: 900 } });
      const page2 = await ctx2.newPage();
      const pageErrors = [];
      page2.on('pageerror', (e) => pageErrors.push(String(e)));
      page2.on('console', (m) => { if (m.type() === 'error') pageErrors.push('console: ' + m.text()); });
      const t0 = Date.now();
      await page2.goto(`${base}/explore.html?table_url=${encodeURIComponent(ds)}`);
      const { report, message } = await waitForExplore(page2, 300000);
      out.steps.explore = { seconds: (Date.now() - t0) / 1000, report, message, pageErrors: pageErrors.slice(0, 5) };
      check('explore sampled', () => {
        assert.ok(report, 'no report: ' + message);
        assert.strictEqual(report.sampled, 3000);
      });
      check('explore columns', () => {
        assert.strictEqual(report.columns.image, 'image');
        assert.strictEqual(report.columns.vector.name, 'image_emb');
        assert.strictEqual(report.columns.vector.dim, 32);
        assert.strictEqual(report.columns.label.name, 'label_name');
        assert.strictEqual(report.byClass, true);
        assert.strictEqual(report.distinct, 5);
      });
      check('explore vectors are pylance values', () => {
        // The sampled rows are all rows, in order: the x/y layout must come from them.
        assert.strictEqual(report.sampleRows.length, 3000);
        assert.strictEqual(report.sampleRows[1500], 1500);
      });
      check('PCA separates the classes', () => {
        const byLabel = new Map();
        report.labels.forEach((l, i) => {
          if (!byLabel.has(l)) byLabel.set(l, []);
          byLabel.get(l).push(i);
        });
        const med = new Map();
        const spread = new Map();
        for (const [l, idx] of byLabel) {
          const xs = idx.map((i) => report.x[i]).sort((a, b) => a - b);
          const ys = idx.map((i) => report.y[i]).sort((a, b) => a - b);
          const mx = xs[xs.length >> 1], my = ys[ys.length >> 1];
          med.set(l, [mx, my]);
          const d = idx.map((i) => Math.hypot(report.x[i] - mx, report.y[i] - my)).sort((a, b) => a - b);
          spread.set(l, d[Math.floor(d.length * 0.9)]);
        }
        let worst = Infinity;
        for (const [l, [mx, my]] of med) {
          for (const [m2, [nx, ny]] of med) {
            if (l === m2) continue;
            worst = Math.min(worst, Math.hypot(mx - nx, my - ny) / (spread.get(l) + spread.get(m2)));
          }
        }
        out.steps.explore.separation = worst;
        assert.ok(worst > 2, `closest pair of classes only ${worst.toFixed(2)} separations apart`);
      });
      await page2.locator('.map-panel').screenshot({ path: path.join(workdir, 'explore-local-map.png') });

      // 3. A click on a sampled point: 12 images, each decoded.
      await page2.locator('#map').scrollIntoViewIfNeeded();
      const clickPoint = await page2.evaluate(() => {
        const c = document.getElementById('map');
        const r = c.getBoundingClientRect();
        const i = 700;
        return { x: r.left + window.__explore.x[i] * c.clientWidth, y: r.top + window.__explore.y[i] * c.clientHeight };
      });
      await page2.mouse.click(clickPoint.x, clickPoint.y);
      await page2.waitForFunction(() => {
        const imgs = [...document.querySelectorAll('#gallery .card img')];
        return imgs.length === 12 && imgs.every((i) => i.complete && i.naturalWidth > 0);
      }, null, { timeout: 120000, polling: 200 });
      // The fade-in takes 0.25 s; give it a second, then count what is actually visible.
      await page2.waitForFunction(() => [...document.querySelectorAll('#gallery .card img')]
        .every((i) => getComputedStyle(i).opacity === '1'), null, { timeout: 1000, polling: 100 }).catch(() => {});
      const click = await page2.evaluate(() => {
        const cards = [...document.querySelectorAll('#gallery .card')];
        return {
          images: cards.length,
          decoded: cards.filter((c) => c.querySelector('img') && c.querySelector('img').naturalWidth > 0).length,
          // Seen, not only decoded: style.css keeps a card's image transparent until the card is 'loaded'.
          visible: cards.filter((c) => {
            const img = c.querySelector('img');
            const r = img ? img.getBoundingClientRect() : null;
            return img && getComputedStyle(img).opacity === '1' && r.width > 0 && r.height > 0;
          }).length,
          captions: cards.map((c) => c.querySelector('figcaption').textContent),
          requests: document.getElementById('s-click-requests').textContent,
          bytes: document.getElementById('s-click-bytes').textContent,
        };
      });
      out.steps.click = click;
      check('click 12 images decode and show', () => {
        assert.strictEqual(click.images, 12);
        assert.strictEqual(click.decoded, 12);
        assert.strictEqual(click.visible, 12, 'images decoded but not visible');
      });
      check('click labels are classes', () => {
        const classes = new Set(expected.rows.map((r) => r.label_name));
        for (const c of click.captions) assert.ok(classes.has(c), 'caption ' + c);
      });
      check('no page errors', () => assert.deepStrictEqual(pageErrors, []));
      await ctx2.close();

      // 4. Phone width: no sideways scroll.
      const ctx3 = await browser.newContext({ viewport: { width: 375, height: 800 }, isMobile: true, hasTouch: true });
      const page3 = await ctx3.newPage();
      await page3.goto(`${base}/explore.html?table_url=${encodeURIComponent(ds)}`);
      await waitForExplore(page3, 300000);
      const widths = await page3.evaluate(() => ({ scroll: document.documentElement.scrollWidth, client: document.documentElement.clientWidth }));
      out.steps.phone = widths;
      check('phone width does not scroll', () => assert.ok(widths.scroll <= widths.client, JSON.stringify(widths)));
      await ctx3.close();
    } else {
      for (const run of HF_RUNS) {
        const ctx = await browser.newContext({ viewport: { width: 1280, height: 900 }, ...contextOpts });
        const page = await ctx.newPage();
        const pageErrors = [];
        page.on('pageerror', (e) => pageErrors.push(String(e)));
        const t0 = Date.now();
        await page.goto(`${base}/explore.html?ds=${run.ds}`);
        const { report, message } = await waitForExplore(page, 1500000);
        const seconds = (Date.now() - t0) / 1000;
        const shot = path.join(workdir, `explore-hf-${run.name}-map.png`);
        if (report) await page.locator('.map-panel').screenshot({ path: shot });
        const numbers = await page.evaluate(() => ({
          requests: document.getElementById('s-requests').textContent,
          bytes: document.getElementById('s-bytes').textContent,
          status: document.getElementById('status').textContent,
        }));
        out.steps[run.name] = {
          seconds, message, numbers, pageErrors: pageErrors.slice(0, 5), screenshot: shot,
          report: report && {
            table: report.table, rows: report.rows, columns: report.columns, sampled: report.sampled,
            requested: report.requested, byClass: report.byClass, distinct: report.distinct,
            groupLabels: report.groupLabels, sampleRequests: report.sampleRequests, sampleBytes: report.sampleBytes,
            totalRequests: report.totalRequests, totalBytes: report.totalBytes,
          },
        };
        check(`${run.name} sampled`, () => assert.ok(report, 'no report: ' + message));
        await ctx.close();
      }
    }
  } catch (e) {
    failures.push('run: ' + String(e && e.message ? e.message : e).split('\n')[0]);
  } finally {
    if (browser) await browser.close();
    if (server) server.child.kill();
  }
  fs.writeFileSync(path.join(workdir, `explore-${mode}.json`), JSON.stringify({ ...out, failures }, null, 2));
  console.log(JSON.stringify({ mode, failures, summary: Object.fromEntries(Object.entries(out.steps).map(([k, v]) => [k, v && v.report ? { sampled: v.report.sampled, requests: v.report.sampleRequests, bytes: v.report.sampleBytes, distinct: v.report.distinct } : (k === 'vectors' ? { ok: v.ok, dim: v.dim, stats: v.stats } : v)])) }, null, 1));
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
