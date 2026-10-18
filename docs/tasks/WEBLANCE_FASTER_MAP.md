# WEBLANCE_FASTER_MAP: read the explore sample with every worker

Status: delegated 2026-10-18.

## What the user's iPhone measured (Safari, food101, debug report)

```
+2.700s input                                   (preset clicked)
+3.201s tree ms=501 req=2
+4.765s open w=1 ms=1362 req=12 bytes=4.1KB
+7.255s columns ...                             <- 2.5 s: probeImage fetches a whole image on worker 1
+9.120s sample.window start=0 rows=500 ms=1860 req=12 bytes=1020.1KB
 ... 9 more windows, each ~1.3-1.5 s, 11 requests, ~1 MB, one after another on worker 1 ...
+22.603s pca ms=741 dim=512 kept=5000 / kmeans ms=18 / sample.total ms=15346 req=123 bytes=9.8MB
+22.605s map.draw ms=2
```

Every step runs on worker 1, one after another; workers 2-4 open only at the first click. Each
window costs ~11 sequential requests (~130 ms each on the phone). The map takes 20 s from the click.

## Goal

The same map, the same sample (ten windows of 500 rows, same rows, same PCA result), in a few
seconds: spread the work over the existing pool (`POOL_SIZE = 4` in `/home/user/weblance/explore.js`).
Page-only change: do **not** change nanolance or the wasm module.

1. **Open all workers at once** when a table is chosen (today only worker 1 opens; the others open
   at the first click). Their opens run in parallel.
2. **Sample windows across the pool:** a new worker message reads given windows and returns their
   vectors (Float32Array, transferred, not copied), labels / captions and row numbers, with the
   window's ms / requests / bytes as today's progress messages carry. The page hands windows to
   whichever worker is free (a queue, like the click's images), puts results back **in window
   order**, then sends the combined vectors to one worker for PCA + k-means (or a new message that
   takes vectors and returns the layout). The rows, their order, and therefore the PCA output must
   be identical to today's.
3. **Do not block on the image probe:** run `probeImage` on a different worker at the same time as
   the sample (the map does not need the image column; only clicks do). If no image column is
   found, show the existing message when it settles.
4. Keep the debug log complete: `sample.window` lines gain `w=` (the worker); `open` for every
   worker; `probe` gets a span (ms, req, bytes). `sample.total` keeps its meaning (from the first
   window request to the last window's result). Add a `layout` span covering PCA + k-means.
5. In `debug.js`: Safari reports `transferSize` / `decodedBodySize` as 0 for the module; log them
   as absent (not `0B`) when both are 0.

## Checks

- `/home/user/nanolance/wasm/test/run_explore_test.cjs local` passes as before (all checks), plus:
  - the sample's rows and their order equal the single-worker plan (`window.__explore` already
    exposes rows; compare with `windowPlan`);
  - the PCA coordinates equal what they were before the change for the local dataset: record
    `window.__explore.x/y` from the current (unchanged) page first, save it in the scratch dir,
    then compare after (exact equality or |diff| < 1e-9);
  - the report has `sample.window` lines from at least 2 different workers;
  - worker opens: one `open` line per worker before the map appears.
  The local dataset has 3,000 rows, so it samples every row in six windows; that is enough to test
  the spreading.
- No Hugging Face run (the reviewer runs one).

## Stop rules

- If keeping the PCA output identical needs a change to PCA itself, stop and ask.

## Rules

- No commits, no pushes. Scratch files only under
  `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/fastmap/`.
- Read big files in windows (`grep -n` first). Long runs in the background; no sleep loops; kill by PID.
- Run the test with `NODE_PATH=/opt/node22/lib/node_modules PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers
  node run_explore_test.cjs local <workdir> /home/user/weblance/lib /home/user/weblance`.

## Report

1. Questions / stops. 2. Files changed. 3. Test results by name. 4. The local report's lines from
`input` to `map.draw`. 5. What is left.
