# WEBLANCE_DEBUG: a hidden debug panel that survives crashes

Status: delegated 2026-10-18.

## Goal

The user tests weblance on their own laptop and pastes results back to the reviewer. Give both
pages (`index.html`, `explore.html` in `/home/user/weblance`) a debug log of timings, counters and
errors, kept in `localStorage` as it is written, so it survives a tab crash; a hidden panel to view
it; and a **Copy debug report** button whose text is pasted into a chat. After a visit that did
not end cleanly (crash, out of memory, killed tab), the next visit shows a banner offering the copy.

Facts: a real renderer crash runs no more JavaScript, so nothing can be saved *at* the crash and
the page cannot reopen itself. Everything is therefore written as it happens, and a visit is
detected as crashed on the next load because it never marked itself ended.

## Design (follow it; ask before changing it)

1. **`debug.js`**, loaded first by both pages (plain script, no module), defines `window.wlDebug`:
   - `wlDebug.log(kind, fields)`: appends `{t: ms since page start (performance.now, 1 decimal),
     kind, ...fields}` to the current visit.
   - `wlDebug.span(kind, fields)`: returns `end(moreFields)` that logs `kind` with `ms` (duration).
   - `wlDebug.attachWorker(worker, name)`: logs the worker's `error` / `messageerror` events.
   - `wlDebug.report()`: the copy text (below).
   Every read and write of `localStorage` is in try/catch; the page works with storage blocked
   (the log then lives in memory only and the panel says so).
2. **Storage:** key `weblance-debug-v1`: `{visits: [{id, page, url, start (ISO), env, ended,
   crashed, events: [...]}]}`. Keep the last 5 visits, at most 3,000 events per visit (drop the
   oldest, count the dropped), total JSON under ~1 MB (drop the oldest visits first). Write errors
   immediately; other events at most every 300 ms and on `visibilitychange` (hidden) and `pagehide`.
3. **Unclean end:** a visit starts with `ended: false`; `pagehide` sets `ended: true` and writes
   (on `pageshow` with `persisted`, set it back to false). On load, every earlier visit still
   `ended: false` is marked `crashed: true`. If any visit is crashed and not yet acknowledged, show
   a small banner at the top: "The last visit ended unexpectedly. [Copy debug report] [Dismiss]".
   Two tabs open at once must not mark each other crashed: a visit also writes a heartbeat
   timestamp every 2 s while visible, and an unended visit whose heartbeat is newer than 6 s ago is
   left alone (not crashed).
4. **Environment (`env`):** user agent, `navigator.userAgentData` brands/platform if present,
   hardwareConcurrency, deviceMemory, screen size and devicePixelRatio, language, the page's URL,
   the module's size from `performance.getEntriesByType('resource')` (transfer and decoded size of
   `weblance.wasm`), navigation timing (DOMContentLoaded, load), and `performance.memory` when
   present (Chromium).
5. **What is logged** (both pages; add it where the pages already know the numbers):
   - module load per worker (ms), dataset open per worker (ms, requests, bytes), info;
   - explore: input/preset chosen, tree-API discovery (ms, requests, tables found), columns
     picked, each sample window (start, rows, ms, requests, bytes), sample total, PCA ms, k-means
     ms, map draw ms, distinct classes;
   - each click: the rows, and per image row: worker, ms, requests, bytes, image size in bytes,
     decode ms (img load), and failures;
   - workers include in their replies the wasm heap size (`M.HEAPU8.length`) and the module's
     `wl_stats_json`; log the heap size at open, after the sample, after each click;
   - errors: `window` `error` and `unhandledrejection`, worker `error`, every error a worker
     reports (the `wl_error()` text), and Emscripten aborts: pass `onAbort` to
     `createWeblance({...})` in each worker and post `{type: 'abort', what}` before the worker dies.
     Also wrap `console.error` to log (and still print).
   - `performance.memory` (when present) at the same milestones as the heap size.
6. **Panel:** hidden by default. Opened by a small, quiet "debug" link in each page's footer, by
   `?debug=1`, or by pressing `Shift+D` (not while typing in an input). Shows: visits (time, page,
   crashed?), the current visit's last 200 events (monospace, newest last), and buttons **Copy
   debug report**, **Clear log**, **Close**. Works at phone width (375 px) without sideways scroll;
   light and dark themes from the existing tokens. One line in the panel: "Stored only in this
   browser; nothing is sent anywhere."
7. **Report text** (what Copy puts on the clipboard): plain text, compact, readable in a chat:
   a header (`weblance debug report`, ISO time, page, the env on a few lines), then each visit
   (crashed ones and the current one; at most the last 3 visits), one event per line:
   `+12.345s sample.window start=37875 rows=500 ms=812 req=16 bytes=1.0MB`. Cap the whole report at
   ~60 KB by dropping the oldest events of the oldest visit first, saying how many were dropped.
   Copy with `navigator.clipboard.writeText`; if that fails, show the text in a selected textarea
   so the user can copy it by hand.

No new dependencies, no build step, plain JavaScript. Do not change what either page does apart
from logging and the panel/banner. Keep `debug.js` around 300-400 lines.

## Tests

Extend `/home/user/nanolance/wasm/test/run_explore_test.cjs` local mode (keep every existing
check passing):

- After the click step, open the panel (`Shift+D`) and check the current visit has events of
  kinds: open, sample window, PCA, click image; the report text contains them.
- **Crash recovery, for real:** in the same browser context, crash the tab (`page.goto('chrome://crash')`
  and catch the crash), open a fresh page on `explore.html`, and check: the banner is shown; the
  report (read it by granting `clipboard-read`/`clipboard-write` to the context and pressing Copy,
  or via `wlDebug.report()`) contains the crashed visit's sample and click events.
- Two tabs open at once: neither banner appears.
- Storage blocked: a context where `localStorage` throws (override with an init script) still
  loads the page, samples and clicks without page errors, and the panel says the log is in memory.
- A forced worker error: a test-only hook is acceptable (e.g. `?debug_fail=open` makes the worker
  throw on open); check it is logged and in the report.
- `index.html`: a small local check in the same file -- load `index.html` with requests to
  huggingface.co blocked (`context.route`), and check the page does not throw, the failures are
  logged, and the panel opens. (Its normal flow needs Hugging Face; do not run it against HF.)

No Hugging Face run is needed for this task.

## Stop rules

- If the pages' existing structure makes any logging point need more than a few lines of change,
  log what is easy and list the rest instead.
- If `chrome://crash` cannot be driven from Playwright here, use `page.close({runBeforeUnload:
  false})` after removing the `pagehide` handler is NOT acceptable as a substitute -- say so and
  propose another real crash (e.g. a CDP `Page.crash`), then use it.

## Rules

- No commits, no pushes. The reviewer commits.
- Scratch files only under `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/debug/`.
- Never read big files whole (app.js, explore.js, run_explore_test.cjs: `grep -n`, then windows).
- Long runs in the background; wait for the notification; no sleep or polling loops. Kill by PID.
- Run the test with `NODE_PATH=/opt/node22/lib/node_modules PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers
  node run_explore_test.cjs local <workdir> /home/user/weblance/lib /home/user/weblance`.

## Report (in this order)

1. Questions and anything you stopped on.
2. Files changed / added.
3. Test results, each check named.
4. A sample report text (the first ~40 lines of a real one from the local test).
5. What is left or rough.
