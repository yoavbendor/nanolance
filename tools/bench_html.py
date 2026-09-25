#!/usr/bin/env python3
"""Render bench/results/matrix.json as a self-contained HTML report (charts + full tables).

    python tools/bench_html.py [matrix.json] [report.html]
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

TEMPLATE = r"""<title>nanolance Benchmarks</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Archivo:wght@500;700;800&family=IBM+Plex+Mono:wght@400;500&family=Public+Sans:wght@400;500;600&display=swap">
<style>
:root {
  --bg: #f6f7f9; --surface: #ffffff; --ink: #14171c; --ink-2: #4b5361; --ink-3: #6b7482;
  --rule: #e2e6ec; --grid: #eceff3; --tint: #eef3fa;
  --nl: #2a78d6; --rust: #eb6834; --rust1: #1baf7a; --parity: #9aa3b1;
  --good: #0ca30c; --bad: #d03b3b;
  --display: "Archivo", "Helvetica Neue", Arial, sans-serif;
  --body: "Public Sans", "Segoe UI", system-ui, sans-serif;
  --mono: "IBM Plex Mono", ui-monospace, "SFMono-Regular", Menlo, monospace;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    color-scheme: dark;
    --bg: #111418; --surface: #171b21; --ink: #eef1f5; --ink-2: #b4bcc8; --ink-3: #8b94a2;
    --rule: #2a313b; --grid: #232932; --tint: #1c2530;
    --nl: #3987e5; --rust: #d95926; --rust1: #199e70; --parity: #6b7482;
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --bg: #111418; --surface: #171b21; --ink: #eef1f5; --ink-2: #b4bcc8; --ink-3: #8b94a2;
  --rule: #2a313b; --grid: #232932; --tint: #1c2530;
  --nl: #3987e5; --rust: #d95926; --rust1: #199e70; --parity: #6b7482;
}
* { box-sizing: border-box; }
body { background: var(--bg); color: var(--ink); font: 15px/1.55 var(--body); }
.wrap { max-width: 1120px; margin: 0 auto; padding-inline: 20px; padding-block: 28px 64px; }
h1, h2, h3 { font-family: var(--display); text-wrap: balance; margin: 0; letter-spacing: -0.01em; }
h1 { font-size: clamp(30px, 5vw, 46px); font-weight: 800; line-height: 1.05; }
h2 { font-size: 24px; font-weight: 700; margin-bottom: 6px; }
h3 { font-size: 16px; font-weight: 700; }
p { margin: 0; max-width: 68ch; }
.muted { color: var(--ink-2); }
.small { font-size: 13px; color: var(--ink-3); }
.mono { font-family: var(--mono); font-variant-numeric: tabular-nums; }
header { display: grid; gap: 14px; padding-bottom: 22px; border-bottom: 1px solid var(--rule); }
.eyebrow { font: 500 12px/1 var(--mono); letter-spacing: .08em; text-transform: uppercase; color: var(--ink-3); }
.env { display: flex; flex-wrap: wrap; gap: 6px 14px; font: 12.5px/1.4 var(--mono); color: var(--ink-2); }
.env span b { color: var(--ink); font-weight: 500; }
section { padding-block: 34px 6px; display: grid; gap: 14px; }
.tiles { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 12px; }
.tile { background: var(--surface); border: 1px solid var(--rule); border-radius: 10px; padding: 16px 18px; display: grid; gap: 4px; }
.tile .label { font-size: 13px; color: var(--ink-2); }
.tile .value { font: 600 34px/1.1 var(--body); font-variant-numeric: proportional-nums; }
.tile .note { font-size: 12.5px; color: var(--ink-3); }
.card { background: var(--surface); border: 1px solid var(--rule); border-radius: 10px; padding: 16px; }
.legend { display: flex; flex-wrap: wrap; gap: 6px 18px; font-size: 13px; color: var(--ink-2); align-items: center; }
.key { display: inline-flex; align-items: center; gap: 7px; }
.dot { width: 10px; height: 10px; border-radius: 50%; display: inline-block; }
.chart-scroll { overflow-x: auto; }
svg text { font-family: var(--body); fill: var(--ink-2); font-size: 12px; }
svg .cat { font: 700 11px var(--mono); letter-spacing: .06em; fill: var(--ink-3); text-transform: uppercase; }
svg .ds { fill: var(--ink); font-size: 12.5px; }
svg .axis { fill: var(--ink-3); font: 11px var(--mono); }
.tip { position: fixed; pointer-events: none; background: var(--surface); color: var(--ink); border: 1px solid var(--rule);
  border-radius: 8px; padding: 8px 10px; font-size: 12.5px; box-shadow: 0 6px 20px rgba(0,0,0,.12); display: none; z-index: 5; max-width: 300px; }
.tip .v { font: 600 14px var(--mono); }
.tip .row { display: flex; gap: 8px; align-items: center; }
.tip .lk { width: 12px; height: 2px; display: inline-block; }
.tabs { display: flex; flex-wrap: wrap; gap: 6px; }
.tabs button { font: 500 13px var(--body); color: var(--ink-2); background: transparent; border: 1px solid var(--rule);
  border-radius: 999px; padding: 5px 12px; cursor: pointer; }
.tabs button[aria-pressed="true"] { background: var(--ink); color: var(--bg); border-color: var(--ink); }
.tabs button:focus-visible { outline: 2px solid var(--nl); outline-offset: 2px; }
.table-scroll { overflow-x: auto; border: 1px solid var(--rule); border-radius: 10px; background: var(--surface); }
table { border-collapse: collapse; width: 100%; font-size: 13px; }
th, td { padding: 7px 10px; text-align: right; white-space: nowrap; border-bottom: 1px solid var(--grid); }
th { font: 600 12px var(--body); color: var(--ink-2); background: var(--tint); position: sticky; top: 0; }
th:first-child, td:first-child { text-align: left; }
td { font-family: var(--mono); font-variant-numeric: tabular-nums; }
td:first-child { font-family: var(--body); }
td .desc { display: block; color: var(--ink-3); font-size: 12px; }
td.win { color: var(--good); font-weight: 500; }
tr:last-child td { border-bottom: 0; }
.grid2 { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 14px; }
ul { margin: 0; padding-left: 20px; max-width: 72ch; }
li + li { margin-top: 6px; }
code { font-family: var(--mono); font-size: .92em; background: var(--tint); padding: 1px 5px; border-radius: 4px; }
pre { font: 12.5px/1.5 var(--mono); background: var(--surface); border: 1px solid var(--rule); border-radius: 10px; padding: 12px 14px; overflow-x: auto; margin: 0; }
.check { color: var(--good); font-weight: 600; }
.fail { color: var(--bad); font-weight: 600; }
@media (prefers-reduced-motion: no-preference) { .tabs button { transition: background .12s; } }
</style>

<div class="wrap">
  <header>
    <div class="eyebrow">Lance file format 2.2 · C++ and Python · measured, not claimed</div>
    <h1>nanolance benchmarks</h1>
    <p class="muted" id="lede"></p>
    <div class="env" id="env"></div>
  </header>

  <section aria-labelledby="h-summary">
    <h2 id="h-summary">At a glance</h2>
    <div class="tiles" id="tiles"></div>
    <p class="small" id="tiles-note"></p>
  </section>

  <section aria-labelledby="h-read">
    <h2 id="h-read">Read speed, by data type</h2>
    <p class="muted">How many times faster nanolance's C++ reader is than Rust Lance, each reading the file its own writer made. Right of the line, nanolance is faster. Hover a dot for the milliseconds.</p>
    <div class="legend" id="legend-read"></div>
    <div class="card chart-scroll"><div id="chart-read"></div></div>
  </section>

  <section aria-labelledby="h-write">
    <h2 id="h-write">Write speed, by data type</h2>
    <p class="muted">The same comparison for writing: nanolance's C API (write_batch + commit) against <code>lance.write_dataset</code>.</p>
    <div class="legend" id="legend-write"></div>
    <div class="card chart-scroll"><div id="chart-write"></div></div>
  </section>

  <section aria-labelledby="h-size">
    <h2 id="h-size">File size</h2>
    <p class="muted">Bytes on disk, nanolance's file against Rust Lance's for the same data. Left of the line, nanolance's file is smaller.</p>
    <div class="card chart-scroll"><div id="chart-size"></div></div>
  </section>

  <section aria-labelledby="h-interop">
    <h2 id="h-interop">They read each other's files</h2>
    <p class="muted">Every file nanolance wrote was read by Rust Lance, and every file Rust Lance wrote was read by nanolance, from C++ and from Python. Each read was checked value by value against the source table before it was timed.</p>
    <div class="table-scroll"><table id="interop"></table></div>
  </section>

  <section aria-labelledby="h-tables">
    <h2 id="h-tables">Every number</h2>
    <p class="muted">Medians in milliseconds; sizes in MB. The faster of nanolance C++ and Rust Lance (1 core) is marked.</p>
    <div class="tabs" id="tabs" role="group" aria-label="Data type group"></div>
    <div class="table-scroll"><table id="full"></table></div>
  </section>

  <section aria-labelledby="h-reading">
    <h2 id="h-reading">Reading the results</h2>
    <div class="grid2" id="notes"></div>
  </section>

  <section aria-labelledby="h-method">
    <h2 id="h-method">How it was measured</h2>
    <ul id="method"></ul>
    <p class="muted">To reproduce on your own hardware:</p>
    <pre>cmake -S . -B build -DCMAKE_BUILD_TYPE=Release &amp;&amp; cmake --build build -j
pip install ./bindings/python pylance pyarrow numpy
python tools/bench_matrix.py        # bench/results/matrix.json
python tools/bench_report.py        # docs/BENCHMARKS.md
python tools/bench_html.py          # this page</pre>
  </section>
</div>
<div class="tip" id="tip" role="tooltip"></div>

<script id="data" type="application/json">__DATA__</script>
<script id="notes-data" type="application/json">__NOTES__</script>
<script>
(function () {
  const DATA = JSON.parse(document.getElementById("data").textContent);
  const NOTES = JSON.parse(document.getElementById("notes-data").textContent);
  const D = DATA.datasets, env = DATA.environment;
  const CATS = [["numeric","Numbers"],["temporal","Dates, times, decimals"],["string","Strings and binary"],
                ["nullable","Nulls"],["nested","Vectors, structs, lists, maps"],["mixed","A realistic table"]];
  const names = CATS.flatMap(([c]) => Object.keys(D).filter(n => D[n].category === c));
  const el = (tag, attrs = {}, text) => { const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v); if (text != null) e.textContent = text; return e; };
  const svgEl = (tag, attrs = {}) => { const e = document.createElementNS("http://www.w3.org/2000/svg", tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v); return e; };
  const fmtMs = v => v == null ? "—" : (v < 10 ? v.toFixed(2) : v < 100 ? v.toFixed(1) : Math.round(v).toLocaleString());
  const fmtX = v => v == null ? "—" : (v >= 10 ? v.toFixed(0) : v.toFixed(v >= 2 ? 1 : 2)) + "×";
  const geo = a => { a = a.filter(v => v > 0); return a.length ? Math.exp(a.reduce((s, v) => s + Math.log(v), 0) / a.length) : null; };
  const R = (n, k) => D[n].read_ms[k], W = (n, k) => D[n].write_ms[k];
  const ratio = (a, b) => (a && b) ? a / b : null;

  // Header.
  document.getElementById("lede").textContent =
    `${names.length} data types — numbers, dates, strings, nulls, vectors, structs, lists and maps — each written and read by nanolance (C++ and Python) and by Rust Lance, with Parquet alongside as a yardstick. Same machine, same data, every read verified.`;
  const envEl = document.getElementById("env");
  [["CPU", `${env.cpu}, ${env.cores} cores`], ["RAM", `${env.memory_gb} GB`], ["Rust Lance", `pylance ${env.pylance}`],
   ["pyarrow", env.pyarrow], ["nanolance", env.nanolance_commit], ["Measured", env.date]].forEach(([k, v]) => {
    const s = el("span"); s.append(k + " "); s.append(el("b", {}, v)); envEl.append(s); });

  // Tiles.
  const read1 = geo(names.map(n => ratio(R(n, "rust-lance-1c <- rust-lance"), R(n, "nanolance-cpp <- nanolance"))));
  const read4 = geo(names.map(n => ratio(R(n, "rust-lance <- rust-lance"), R(n, "nanolance-cpp <- nanolance"))));
  const write4 = geo(names.map(n => ratio(W(n, "rust-lance"), W(n, "nanolance-cpp"))));
  const write1 = geo(names.map(n => ratio(W(n, "rust-lance-1c"), W(n, "nanolance-cpp"))));
  const size = geo(names.map(n => ratio(D[n].size["nanolance"], D[n].size["rust-lance"])));
  const failures = names.reduce((s, n) => s + Object.keys(D[n].errors).length, 0);
  const interopReads = names.length * 6;
  const tiles = [
    ["Read, per core", fmtX(read1), "nanolance C++ vs Rust Lance on one core (geometric mean)"],
    ["Read, out of the box", fmtX(read4), "nanolance on one core vs Rust Lance on all " + env.cores],
    ["Write", fmtX(write4), `vs Rust Lance on all cores; ${fmtX(write1)} vs one core`],
    ["File size", (size * 100).toFixed(1) + "%", "of Rust Lance's, same data"],
    ["Cross-reads verified", `${interopReads - failures} / ${interopReads}`, "Rust ⇄ nanolance, C++ and Python"],
  ];
  const tilesEl = document.getElementById("tiles");
  tiles.forEach(([l, v, n]) => { const t = el("div", {class: "tile"}); t.append(el("div", {class: "label"}, l), el("div", {class: "value"}, v), el("div", {class: "note"}, n)); tilesEl.append(t); });
  document.getElementById("tiles-note").textContent = "Speed ratios are Rust Lance's time divided by nanolance's: above 1× means nanolance is faster. Averages are geometric means across all data types.";

  // Tooltip.
  const tip = document.getElementById("tip");
  function showTip(evt, rows, title) {
    tip.replaceChildren();
    tip.append(el("div", {class: "small"}, title));
    rows.forEach(([color, value, label]) => { const r = el("div", {class: "row"});
      const k = el("span", {class: "lk"}); k.style.background = color; r.append(k, el("span", {class: "v"}, value), el("span", {class: "small"}, label)); tip.append(r); });
    tip.style.display = "block";
    const x = Math.min(evt.clientX + 14, window.innerWidth - tip.offsetWidth - 8);
    const y = Math.min(evt.clientY + 14, window.innerHeight - tip.offsetHeight - 8);
    tip.style.left = x + "px"; tip.style.top = y + "px";
  }
  const hideTip = () => { tip.style.display = "none"; };

  function legend(id, series) {
    const l = document.getElementById(id);
    series.forEach(s => { const k = el("span", {class: "key"}); const d = el("span", {class: "dot"}); d.style.background = `var(${s.color})`; k.append(d, s.label); l.append(k); });
  }

  // Ratio dot plot: one row per dataset, log x axis, parity line at 1.
  function dotPlot(targetId, series, opts) {
    const rowH = 24, catH = 26, left = 250, right = 70, top = 34, width = 900;
    const rows = [];
    CATS.forEach(([c, title]) => { const ns = names.filter(n => D[n].category === c); if (!ns.length) return; rows.push({cat: title}); ns.forEach(n => rows.push({n})); });
    const height = top + rows.reduce((h, r) => h + (r.cat ? catH : rowH), 0) + 30;
    const vals = [];
    rows.forEach(r => { if (r.n) series.forEach(s => { const v = s.value(r.n); if (v) vals.push(v); }); });
    const lo = Math.min(opts.min, ...vals) * 0.85, hi = Math.max(opts.max, ...vals) * 1.15;
    const x = v => left + (Math.log(v) - Math.log(lo)) / (Math.log(hi) - Math.log(lo)) * (width - left - right);
    const svg = svgEl("svg", {viewBox: `0 0 ${width} ${height}`, width: "100%", role: "img", "aria-label": opts.aria, style: `min-width:${720}px`});
    const ticks = opts.ticks.filter(t => t >= lo && t <= hi);
    ticks.forEach(t => {
      svg.append(svgEl("line", {x1: x(t), x2: x(t), y1: top - 8, y2: height - 26, stroke: t === 1 ? "var(--parity)" : "var(--grid)", "stroke-width": t === 1 ? 1.5 : 1}));
      const tx = svgEl("text", {x: x(t), y: height - 10, "text-anchor": "middle", class: "axis"}); tx.textContent = opts.tickLabel(t); svg.append(tx);
    });
    const hl = svgEl("text", {x: x(1) - 8, y: top - 16, "text-anchor": "end", class: "axis"}); hl.textContent = opts.leftLabel; svg.append(hl);
    const hr = svgEl("text", {x: x(1) + 8, y: top - 16, "text-anchor": "start", class: "axis"}); hr.textContent = opts.rightLabel; svg.append(hr);
    let y = top;
    rows.forEach(r => {
      if (r.cat) { const t = svgEl("text", {x: 0, y: y + 17, class: "cat"}); t.textContent = r.cat; svg.append(t); y += catH; return; }
      const cy = y + rowH / 2;
      svg.append(svgEl("line", {x1: left, x2: width - right, y1: cy, y2: cy, stroke: "var(--grid)", "stroke-width": 1}));
      const lab = svgEl("text", {x: 0, y: cy + 4, class: "ds"}); lab.textContent = r.n; svg.append(lab);
      const pts = series.map(s => ({s, v: s.value(r.n)})).filter(p => p.v);
      if (pts.length === 2) svg.append(svgEl("line", {x1: x(pts[0].v), x2: x(pts[1].v), y1: cy, y2: cy, stroke: "var(--ink-3)", "stroke-width": 1, opacity: .5}));
      pts.forEach(p => {
        const c = svgEl("circle", {cx: x(p.v), cy, r: 5, fill: `var(${p.s.color})`, stroke: "var(--surface)", "stroke-width": 2});
        svg.append(c);
      });
      // Label the primary series value at the row's end.
      const primary = pts[0];
      if (primary) { const t = svgEl("text", {x: width - right + 8, y: cy + 4, class: "axis"}); t.textContent = opts.valueLabel(primary.v); svg.append(t); }
      const hit = svgEl("rect", {x: left, y, width: width - left - right, height: rowH, fill: "transparent", tabindex: 0, "aria-label": r.n});
      const show = evt => showTip(evt, opts.tip(r.n), `${r.n} — ${D[r.n].description}`);
      hit.addEventListener("pointermove", show); hit.addEventListener("pointerleave", hideTip);
      hit.addEventListener("focus", e => { const b = e.target.getBoundingClientRect(); show({clientX: b.left + 40, clientY: b.top}); });
      hit.addEventListener("blur", hideTip);
      svg.append(hit);
      y += rowH;
    });
    document.getElementById(targetId).append(svg);
  }

  const TICKS = [0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32];
  const READ_SERIES = [
    {label: "vs Rust Lance, 1 core", color: "--rust1", value: n => ratio(R(n, "rust-lance-1c <- rust-lance"), R(n, "nanolance-cpp <- nanolance"))},
    {label: "vs Rust Lance, all cores", color: "--rust", value: n => ratio(R(n, "rust-lance <- rust-lance"), R(n, "nanolance-cpp <- nanolance"))},
  ];
  legend("legend-read", READ_SERIES);
  dotPlot("chart-read", READ_SERIES, {min: 0.5, max: 2, ticks: TICKS, tickLabel: t => t + "×", valueLabel: fmtX,
    leftLabel: "← Rust Lance faster", rightLabel: "nanolance faster →", aria: "Read speed ratio per data type",
    tip: n => [["var(--nl)", fmtMs(R(n, "nanolance-cpp <- nanolance")) + " ms", "nanolance C++"],
               ["var(--rust1)", fmtMs(R(n, "rust-lance-1c <- rust-lance")) + " ms", "Rust Lance, 1 core"],
               ["var(--rust)", fmtMs(R(n, "rust-lance <- rust-lance")) + " ms", "Rust Lance, all cores"]]});
  const WRITE_SERIES = [
    {label: "vs Rust Lance, 1 core", color: "--rust1", value: n => ratio(W(n, "rust-lance-1c"), W(n, "nanolance-cpp"))},
    {label: "vs Rust Lance, all cores", color: "--rust", value: n => ratio(W(n, "rust-lance"), W(n, "nanolance-cpp"))},
  ];
  legend("legend-write", WRITE_SERIES);
  dotPlot("chart-write", WRITE_SERIES, {min: 0.5, max: 2, ticks: TICKS, tickLabel: t => t + "×", valueLabel: fmtX,
    leftLabel: "← Rust Lance faster", rightLabel: "nanolance faster →", aria: "Write speed ratio per data type",
    tip: n => [["var(--nl)", fmtMs(W(n, "nanolance-cpp")) + " ms", "nanolance C++"],
               ["var(--rust1)", fmtMs(W(n, "rust-lance-1c")) + " ms", "Rust Lance, 1 core"],
               ["var(--rust)", fmtMs(W(n, "rust-lance")) + " ms", "Rust Lance, all cores"]]});
  const SIZE_SERIES = [{label: "nanolance / Rust Lance", color: "--nl", value: n => ratio(D[n].size["nanolance"], D[n].size["rust-lance"])}];
  dotPlot("chart-size", SIZE_SERIES, {min: 0.8, max: 1.25, ticks: [0.5, 0.75, 0.9, 1, 1.1, 1.25, 1.5, 2], tickLabel: t => Math.round(t * 100) + "%",
    valueLabel: v => (v * 100).toFixed(0) + "%", leftLabel: "← nanolance smaller", rightLabel: "nanolance larger →", aria: "File size ratio per data type",
    tip: n => [["var(--nl)", (D[n].size["nanolance"] / 1e6).toFixed(2) + " MB", "nanolance"],
               ["var(--rust)", (D[n].size["rust-lance"] / 1e6).toFixed(2) + " MB", "Rust Lance"],
               ["var(--rust1)", ((D[n].size["parquet"] || 0) / 1e6).toFixed(2) + " MB", "Parquet (zstd)"]]});

  // Interop table.
  const inter = document.getElementById("interop");
  const pairs = [["nanolance-cpp <- rust-lance", "nanolance C++ reads Rust's"], ["nanolance-py <- rust-lance", "nanolance Python reads Rust's"],
                 ["rust-lance <- nanolance", "Rust reads nanolance's"], ["nanolance-cpp <- nanolance", "nanolance C++ reads its own"],
                 ["rust-lance <- rust-lance", "Rust reads its own"], ["parquet <- parquet", "Parquet"]];
  const ih = el("tr"); ih.append(el("th", {}, "data type")); pairs.forEach(([, l]) => ih.append(el("th", {}, l))); inter.append(ih);
  names.forEach(n => { const tr = el("tr"); tr.append(el("td", {}, n));
    pairs.forEach(([k]) => { const v = R(n, k); const td = el("td"); if (v != null) { const s = el("span", {class: "check"}, "✓ "); td.append(s, fmtMs(v) + " ms"); } else { td.append(el("span", {class: "fail"}, "✗ failed")); } tr.append(td); });
    inter.append(tr); });

  // Full tables with category tabs.
  const full = document.getElementById("full");
  const COLS = [
    ["rows", n => D[n].rows.toLocaleString()],
    ["read · nanolance C++", n => fmtMs(R(n, "nanolance-cpp <- nanolance")), "rc"],
    ["read · + retaining malloc", n => fmtMs(R(n, "nanolance-cpp-retain <- nanolance"))],
    ["read · nanolance Python", n => fmtMs(R(n, "nanolance-py <- nanolance"))],
    ["read · Rust 1 core", n => fmtMs(R(n, "rust-lance-1c <- rust-lance")), "rr"],
    ["read · Rust all cores", n => fmtMs(R(n, "rust-lance <- rust-lance"))],
    ["read · Parquet", n => fmtMs(R(n, "parquet <- parquet"))],
    ["write · nanolance C++", n => fmtMs(W(n, "nanolance-cpp")), "wc"],
    ["write · nanolance Python", n => fmtMs(W(n, "nanolance-py"))],
    ["write · Rust 1 core", n => fmtMs(W(n, "rust-lance-1c")), "wr"],
    ["write · Rust all cores", n => fmtMs(W(n, "rust-lance"))],
    ["write · Parquet", n => fmtMs(W(n, "parquet"))],
    ["MB · nanolance", n => (D[n].size["nanolance"] / 1e6).toFixed(2)],
    ["MB · Rust Lance", n => (D[n].size["rust-lance"] / 1e6).toFixed(2)],
    ["MB · Parquet", n => ((D[n].size["parquet"] || 0) / 1e6).toFixed(2)],
  ];
  function renderTable(cat) {
    full.replaceChildren();
    const h = el("tr"); h.append(el("th", {}, "data type")); COLS.forEach(([t]) => h.append(el("th", {}, t))); full.append(h);
    names.filter(n => cat === "all" || D[n].category === cat).forEach(n => {
      const tr = el("tr"); const first = el("td"); first.append(el("span", {class: "mono"}, n), el("span", {class: "desc"}, D[n].description)); tr.append(first);
      const rWin = (R(n, "nanolance-cpp <- nanolance") || 1e9) <= (R(n, "rust-lance-1c <- rust-lance") || 1e9);
      const wWin = (W(n, "nanolance-cpp") || 1e9) <= (W(n, "rust-lance-1c") || 1e9);
      COLS.forEach(([, f, tag]) => { const td = el("td", {}, f(n));
        if ((tag === "rc" && rWin) || (tag === "rr" && !rWin) || (tag === "wc" && wWin) || (tag === "wr" && !wWin)) td.className = "win";
        tr.append(td); });
      full.append(tr); });
  }
  const tabs = document.getElementById("tabs");
  [["all", "All"], ...CATS].forEach(([c, t], i) => {
    if (c !== "all" && !names.some(n => D[n].category === c)) return;
    const b = el("button", {type: "button", "aria-pressed": i === 0 ? "true" : "false", id: "tab-" + c}, t);
    b.addEventListener("click", () => { tabs.querySelectorAll("button").forEach(x => x.setAttribute("aria-pressed", "false")); b.setAttribute("aria-pressed", "true"); renderTable(c); });
    tabs.append(b); });
  renderTable("all");

  // Notes and method.
  const notes = document.getElementById("notes");
  NOTES.findings.forEach(([title, body]) => { const c = el("div", {class: "card"}); c.append(el("h3", {}, title)); const p = el("p", {class: "muted"}, body); p.style.marginTop = "6px"; c.append(p); notes.append(c); });
  const method = document.getElementById("method");
  NOTES.method.forEach(t => method.append(el("li", {}, t)));
})();
</script>
"""


def main(argv):
    src = Path(argv[1]) if len(argv) > 1 else ROOT / "bench" / "results" / "matrix.json"
    dst = Path(argv[2]) if len(argv) > 2 else ROOT / "bench" / "results" / "report.html"
    notes_path = src.with_name("notes.json")
    notes = json.loads(notes_path.read_text()) if notes_path.exists() else {"findings": [], "method": []}
    data = json.loads(src.read_text())
    html = TEMPLATE.replace("__DATA__", json.dumps(data).replace("</", "<\\/")).replace(
        "__NOTES__", json.dumps(notes).replace("</", "<\\/"))
    dst.write_text(html)
    print(f"wrote {dst}")


if __name__ == "__main__":
    main(sys.argv)
