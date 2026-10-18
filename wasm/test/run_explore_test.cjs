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
const PAGE_FILES = ['explore.html', 'explore.js', 'explore-worker.js', 'style.css', 'index.html', 'app.js', 'worker.js', 'debug.js'];
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

// The rows of the sample in window order, as explore.js's windowPlan lays them out: every row of a table of
// up to 5,000 rows (windows of 500), else ten spread windows of 500.
function planRows(n) {
  const windows = [];
  if (n <= 5000) {
    for (let s = 0; s < n; s += 500) windows.push([s, Math.min(500, n - s)]);
  } else {
    for (let k = 0; k < 10; k++) windows.push([Math.round((k * (n - 500)) / 9), 500]);
  }
  const rows = [];
  for (const [s, len] of windows) for (let i = 0; i < len; i++) rows.push(s + i);
  return rows;
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

// ---- The debug log (debug.js): crash recovery, two tabs, storage blocked, a forced worker error and
// index.html with Hugging Face blocked. Each scenario has its own browser context.

const DEBUG_KEY = 'weblance-debug-v1';
const CLICK_AT = 700;  // a sampled point, as in the click step

// The image cards of a click are all decoded.
async function clickSampledPoint(page, index) {
  await page.locator('#map').scrollIntoViewIfNeeded();
  const p = await page.evaluate((i) => {
    const c = document.getElementById('map');
    const r = c.getBoundingClientRect();
    return { x: r.left + window.__explore.x[i] * c.clientWidth, y: r.top + window.__explore.y[i] * c.clientHeight };
  }, index);
  await page.mouse.click(p.x, p.y);
  await page.waitForFunction(() => {
    const imgs = [...document.querySelectorAll('#gallery .card img')];
    return imgs.length === 12 && imgs.every((i) => i.complete && i.naturalWidth > 0);
  }, null, { timeout: 120000, polling: 200 });
}

// The block of one visit in a report: from its '== visit <id>' line to the next visit.
function blockOf(report, visitId) {
  return report.split('\n== visit ').find((b) => b.startsWith(visitId + ' ')) || '';
}

// A renderer crash: chrome://crash, or CDP Page.crash when that does not crash the tab here.
async function crashTab(ctx, page) {
  const crashed = page.waitForEvent('crash', { timeout: 15000 }).then(() => true, () => false);
  page.goto('chrome://crash').catch(() => {});
  if (await crashed) return 'chrome://crash';
  const cdp = await ctx.newCDPSession(page);
  const crashed2 = page.waitForEvent('crash', { timeout: 15000 }).then(() => true, () => false);
  cdp.send('Page.crash').catch(() => {});
  if (await crashed2) return 'CDP Page.crash';
  throw new Error('the tab did not crash: chrome://crash and CDP Page.crash both failed');
}

// The crashed visit is only recognised once its heartbeat is over 6 s old: wait for that on a page of the
// same origin that has no debug log.
async function waitUntilStale(ctx, base, visitId) {
  const probe = await ctx.newPage();
  await probe.goto(`${base}/style.css`);
  await probe.waitForFunction((a) => {
    const s = JSON.parse(localStorage.getItem(a.key) || '{"visits":[]}');
    const v = s.visits.find((x) => x.id === a.id);
    return !v || Date.now() - v.hb > 6500;
  }, { key: DEBUG_KEY, id: visitId }, { timeout: 30000, polling: 250 });
  await probe.close();
}

const bannerShown = (page) => page.evaluate(() => {
  const b = document.querySelector('.wl-banner');
  return !!b && !b.hidden;
});

async function debugScenarios({ browser, base, ds, check, out }) {
  const viewport = { width: 1280, height: 900 };
  const tableUrl = `${base}/explore.html?table_url=${encodeURIComponent(ds)}`;
  out.steps.debug = out.steps.debug || {};

  // 1. Crash recovery: a visit that samples and clicks, then its tab crashes. The next visit shows the
  //    banner, and its report has the crashed visit's sample and click events; Copy puts it on the clipboard.
  {
    const ctx = await browser.newContext({ viewport });
    await ctx.grantPermissions(['clipboard-read', 'clipboard-write'], { origin: base });
    const page = await ctx.newPage();
    await page.goto(tableUrl);
    await waitForExplore(page, 300000);
    await clickSampledPoint(page, CLICK_AT);
    const crashId = await page.evaluate(() => window.wlDebug.visitId);
    // Ordinary events reach the store at most 300 ms after they happen: wait until the click is stored.
    await page.waitForFunction(([key, id]) => {
      const v = JSON.parse(localStorage.getItem(key)).visits.find((x) => x.id === id);
      return !!v && v.events.some((e) => e.kind === 'click.done');
    }, [DEBUG_KEY, crashId], { timeout: 30000, polling: 200 });
    const how = await crashTab(ctx, page);
    await waitUntilStale(ctx, base, crashId);
    const next = await ctx.newPage();
    const errors = [];
    next.on('pageerror', (e) => errors.push(String(e)));
    await next.goto(`${base}/explore.html`);
    await next.waitForFunction(() => {
      const b = document.querySelector('.wl-banner');
      return !!b && !b.hidden;
    }, null, { timeout: 30000, polling: 200 });
    const bannerText = await next.locator('.wl-banner').innerText();
    const report = await next.evaluate(() => window.wlDebug.report());
    const block = blockOf(report, crashId);
    fs.writeFileSync(path.join(workdir, 'debug-report-crash.txt'), report);
    await next.locator('.wl-banner button', { hasText: 'Copy debug report' }).click();
    await next.waitForFunction(() => /Copied|Copy is blocked/.test(document.querySelector('.wl-banner .wl-say').textContent), null, { timeout: 10000 });
    const clip = await next.evaluate(() => navigator.clipboard.readText());
    await next.locator('.wl-banner button', { hasText: 'Dismiss' }).click();
    const afterDismiss = await bannerShown(next);
    out.steps.debug.crash = { how, crashId, bannerText, blockLines: block.split('\n').length, clipStart: clip.slice(0, 60) };
    check('crash: the next visit shows the banner', () => assert.match(bannerText, /The last visit ended unexpectedly/));
    check('crash: the report has the crashed visit with its sample, PCA and click events', () => {
      assert.match(block, /\(CRASHED\)/, 'visit not marked CRASHED');
      for (const kind of ['sample.window', 'pca', 'click.image', 'click.done']) {
        assert.match(block, new RegExp('\\+\\d+\\.\\d{3}s ' + kind.replace('.', '\\.') + ' '), kind);
      }
      assert.strictEqual((block.match(/ click\.image /g) || []).length, 12, 'click.image lines');
    });
    check('crash: Copy puts the report on the clipboard', () => {
      assert.ok(clip.startsWith('weblance debug report'), clip.slice(0, 60));
      assert.ok(clip.includes('== visit ' + crashId + ' '), 'crashed visit missing from the clipboard text');
    });
    check('crash: Dismiss hides the banner', () => assert.strictEqual(afterDismiss, false));
    check('crash: no page errors on the next visit', () => assert.deepStrictEqual(errors, []));
    await ctx.close();
  }

  // 2. Two tabs open at once: neither is marked crashed, not even a third one opened after both were alive
  //    for a while (their heartbeats keep them unended).
  {
    const ctx = await browser.newContext({ viewport });
    const a = await ctx.newPage();
    await a.goto(`${base}/explore.html`);
    const b = await ctx.newPage();
    await b.goto(`${base}/explore.html`);
    await a.waitForFunction(() => performance.now() > 7000, null, { timeout: 30000, polling: 500 });
    const c = await ctx.newPage();
    await c.goto(`${base}/explore.html`);
    const shown = [];
    for (const p of [a, b, c]) shown.push(await bannerShown(p));
    out.steps.debug.twoTabs = { shown, visibility: await a.evaluate(() => document.visibilityState) };
    check('two tabs: no banner in either', () => assert.deepStrictEqual(shown, [false, false, false]));
    await ctx.close();
  }

  // 2b. A background tab: its page reports 'hidden', so it writes no heartbeat. A visit opened 7 s later must
  //     not call it crashed: the tab is alive and still holds its Web Lock.
  {
    const ctx = await browser.newContext({ viewport });
    const a = await ctx.newPage();
    await a.addInitScript(() => Object.defineProperty(document, 'visibilityState', { get: () => 'hidden' }));
    await a.goto(`${base}/explore.html`);
    await a.waitForFunction(() => performance.now() > 7000, null, { timeout: 30000, polling: 500 });
    const c = await ctx.newPage();
    await c.goto(`${base}/explore.html`);
    await c.waitForTimeout(500);  // the lock query is asynchronous
    const shown = await bannerShown(c);
    out.steps.debug.backgroundTab = { shown };
    check('background tab: a live hidden tab is not reported as crashed', () => assert.strictEqual(shown, false));
    await ctx.close();
  }

  // 3. Storage blocked: localStorage throws. The page still samples and clicks without page errors, and the
  //    panel says the log is in memory only.
  {
    const ctx = await browser.newContext({ viewport });
    await ctx.addInitScript(() => {
      try {
        Object.defineProperty(window, 'localStorage', {
          configurable: true, get() { throw new DOMException('storage blocked by the test', 'SecurityError'); },
        });
      } catch (e) {
        // the probe below reports whether the block took
      }
    });
    const page = await ctx.newPage();
    const errors = [];
    page.on('pageerror', (e) => errors.push(String(e)));
    await page.goto(tableUrl);
    const { report } = await waitForExplore(page, 300000);
    await clickSampledPoint(page, CLICK_AT);
    await page.keyboard.press('Shift+D');
    const panel = await page.evaluate(() => {
      const p = document.getElementById('wl-debug');
      const memNote = p && p.querySelector('.wl-mem');
      return { open: !!p && !p.hidden, memVisible: !!memNote && !memNote.hidden, text: memNote ? memNote.textContent : '',
        storageOk: window.wlDebug.storageOk, sampled: !!window.__explore };
    });
    out.steps.debug.blocked = panel;
    check('storage blocked: the page samples and clicks without page errors', () => {
      assert.ok(panel.sampled, 'no sample');
      assert.deepStrictEqual(errors, []);
    });
    check('storage blocked: the panel says the log is in memory', () => {
      assert.strictEqual(panel.open, true, 'panel not open');
      assert.strictEqual(panel.memVisible, true, 'memory note hidden');
      assert.strictEqual(panel.storageOk, false);
      assert.match(panel.text, /in memory only/);
    });
    await ctx.close();
  }

  // 4. A forced worker error (test hook debug_fail=open): logged, shown and in the report.
  {
    const ctx = await browser.newContext({ viewport });
    const page = await ctx.newPage();
    const errors = [];
    page.on('pageerror', (e) => errors.push(String(e)));
    await page.goto(`${tableUrl}&debug_fail=open`);
    const { message } = await waitForExplore(page, 120000);
    const report = await page.evaluate(() => window.wlDebug.report());
    out.steps.debug.forcedError = { message, lines: report.split('\n').filter((l) => /worker\.error|error /.test(l)).slice(0, 4) };
    check('forced worker error: shown on the page and logged in the report', () => {
      assert.match(message, /debug_fail=open/);
      assert.match(report, /\+\d+\.\d{3}s worker\.error w=w\d op=open error=debug_fail=open/);
    });
    check('forced worker error: no uncaught page error', () => assert.deepStrictEqual(errors, []));
    await ctx.close();
  }

  // 5. index.html with Hugging Face blocked: no page error, the failures are logged, and the panel opens from
  //    the footer link. (Not run against Hugging Face.)
  {
    const ctx = await browser.newContext({ viewport });
    let blocked = 0;
    await ctx.route(/huggingface\.co/, (route) => { blocked++; route.abort(); });
    const page = await ctx.newPage();
    const errors = [];
    page.on('pageerror', (e) => errors.push(String(e)));
    await page.goto(`${base}/index.html`);
    await page.waitForFunction(() => /could not be opened/.test(document.getElementById('status').textContent),
      null, { timeout: 180000, polling: 500 });
    await page.locator('footer button.wl-open').click();
    const report = await page.evaluate(() => window.wlDebug.report());
    const panelOpen = await page.evaluate(() => !document.getElementById('wl-debug').hidden);
    out.steps.debug.index = { blocked, panelOpen, failures: (report.match(/worker\.error|abort /g) || []).length,
      mapLine: (report.match(/map\.load.*/) || [''])[0] };
    check('index: Hugging Face requests reached the route block', () => assert.ok(blocked > 0, 'no request was routed'));
    check('index: the failures are logged and the footer link opens the panel', () => {
      assert.ok(out.steps.debug.index.failures > 0, 'no worker failure logged');
      assert.strictEqual(panelOpen, true);
    });
    check('index: no page error', () => assert.deepStrictEqual(errors, []));
    await ctx.close();
  }
  fs.writeFileSync(path.join(workdir, 'debug-scenarios.json'), JSON.stringify(out.steps.debug, null, 2));
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
      check('sample rows and their order equal the window plan', () => {
        assert.deepStrictEqual(report.sampleRows, planRows(report.rows));
      });
      check('PCA coordinates equal the ones recorded before the change', () => {
        // The layout must not change when the page's work is reorganized. EXPLORE_PCA_BASELINE names a
        // file of x and y recorded by an earlier run (each run writes WORKDIR/pca-baseline.json); without
        // it, this run records one for the next and compares nothing.
        fs.writeFileSync(path.join(workdir, 'pca-baseline.json'), JSON.stringify({ x: report.x, y: report.y }));
        if (!process.env.EXPLORE_PCA_BASELINE) {
          console.log('no EXPLORE_PCA_BASELINE: recorded ' + path.join(workdir, 'pca-baseline.json'));
          return;
        }
        const base = JSON.parse(fs.readFileSync(process.env.EXPLORE_PCA_BASELINE, 'utf8'));
        assert.strictEqual(report.x.length, base.x.length, 'point count');
        base.x.forEach((v, i) => {
          assert.ok(Math.abs(report.x[i] - v) < 1e-9 && Math.abs(report.y[i] - base.y[i]) < 1e-9,
            `point ${i}: (${report.x[i]}, ${report.y[i]}) vs (${v}, ${base.y[i]})`);
        });
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

      // 5. The debug log: Shift+D opens its panel, and the visit's report has the open, sample, PCA and click events.
      await page2.keyboard.press('Shift+D');
      const dbgReport = await page2.evaluate(() => window.wlDebug.report());
      const dbgPanel = await page2.evaluate(() => {
        const p = document.getElementById('wl-debug');
        return { open: !!p && !p.hidden, text: p ? p.textContent : '' };
      });
      fs.writeFileSync(path.join(workdir, 'debug-report-local.txt'), dbgReport);
      out.steps.debug = out.steps.debug || {};
      out.steps.debug.click = { panelOpen: dbgPanel.open, lines: dbgReport.split('\n').length };
      check('debug panel opens with Shift+D and says where the log is', () => {
        assert.strictEqual(dbgPanel.open, true);
        assert.ok(dbgPanel.text.includes('Stored only in this browser; nothing is sent anywhere.'));
      });
      check('debug report has the open, sample, PCA and click events', () => {
        for (const kind of ['open', 'sample.window', 'pca', 'click.image']) {
          assert.match(dbgReport, new RegExp('\\+\\d+\\.\\d{3}s ' + kind.replace('.', '\\.') + ' '), kind);
        }
        assert.strictEqual((dbgReport.match(/ click\.image /g) || []).length, 12, 'click.image lines');
      });
      check('sample windows come from at least two workers', () => {
        const ws = new Set([...dbgReport.matchAll(/ sample\.window w=(\d)/g)].map((m) => m[1]));
        assert.ok(ws.size >= 2, 'workers: ' + [...ws].join(','));
      });
      check('one open line per worker, all before the map is drawn', () => {
        const lines = dbgReport.split('\n');
        const drawAt = lines.findIndex((l) => / map\.draw /.test(l));
        assert.ok(drawAt > 0, 'no map.draw line');
        const opens = lines.slice(0, drawAt).map((l) => /^\+\d+\.\d{3}s open w=(\d) /.exec(l)).filter(Boolean).map((m) => m[1]);
        assert.deepStrictEqual(opens.slice().sort(), ['1', '2', '3', '4'], 'open lines before map.draw');
      });
      await ctx2.close();

      // 4. Phone width: no sideways scroll.
      const ctx3 = await browser.newContext({ viewport: { width: 375, height: 800 }, isMobile: true, hasTouch: true });
      const page3 = await ctx3.newPage();
      await page3.goto(`${base}/explore.html?table_url=${encodeURIComponent(ds)}`);
      await waitForExplore(page3, 300000);
      const widths = await page3.evaluate(() => ({ scroll: document.documentElement.scrollWidth, client: document.documentElement.clientWidth }));
      out.steps.phone = widths;
      check('phone width does not scroll', () => assert.ok(widths.scroll <= widths.client, JSON.stringify(widths)));
      // The debug panel at phone width too.
      await page3.keyboard.press('Shift+D');
      const panelWidths = await page3.evaluate(() => ({ scroll: document.documentElement.scrollWidth, client: document.documentElement.clientWidth,
        open: !document.getElementById('wl-debug').hidden }));
      out.steps.phonePanel = panelWidths;
      check('phone width does not scroll with the debug panel open', () => assert.ok(panelWidths.open && panelWidths.scroll <= panelWidths.client, JSON.stringify(panelWidths)));
      await ctx3.close();
      await debugScenarios({ browser, base, ds, check, out });
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
