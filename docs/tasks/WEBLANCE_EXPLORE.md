# WEBLANCE_EXPLORE: open any Hugging Face Lance image dataset in the browser

Status: done 2026-10-18 (nanolance 6fe2cd0, weblance 9b2b930). Outcome: all local checks pass;
Hugging Face MNIST 186 requests / 10.2 MB / 10 classes, Food-101 168 requests / 10.0 MB / 14 of 101
classes. Review: a 50 x 100-row sample was tried and reverted (each window costs ~16 requests, so
~800 requests); class names shown with spaces; one retry on the tree-API fetch (one MNIST run
failed on a single "Failed to fetch").

## Goal

A second page in the weblance demo (`/home/user/weblance`, public repo, GitHub Pages from `main`):
the viewer pastes a Hugging Face dataset URL (or clicks a preset), the page finds the Lance
table, reads a sample of its embedding vectors, lays them out with PCA, colors and labels the map
by class, and shows the image of any point clicked. Everything runs in the browser through the
existing nanolance WebAssembly module; nothing is precomputed.

Scope is deliberately narrow: Hugging Face URLs only, image datasets only (no text panels, no
audio), PCA only, no index use.

## Facts already measured (do not re-measure)

The current module (`/home/user/weblance/lib/weblance.{js,wasm}`, built by
`wasm/build_weblance.sh` from `wasm/weblance_glue.cpp`) already opens these six datasets over
`https://huggingface.co/datasets/<id>/resolve/main/<path>` in headless Chromium, and every scalar
value of rows 0, 1, n/2, n-1 and the image bytes (SHA-256) of row n/2 match pylance:

| id | table path | rows | columns |
|---|---|---|---|
| lance-format/mnist-lance | data/test.lance | 10,000 | id:int64, image:large_binary, label:int32, label_name:string, image_emb:fsl<float>[512] |
| lance-format/cifar10-lance | data/test.lance | 10,000 | same as mnist |
| lance-format/oxford-pets-lance | data/train.lance | 7,390 | id, image, label_name:string, is_dog:bool, path:string, image_emb[512] |
| lance-format/food101-lance | data/train.lance | 75,750 | same as mnist |
| lance-format/imagenet-1k-val-lance | data/validation.lance | 50,000 | same as mnist |
| lance-format/laion-1m | data/train.lance | 1,162,252 | caption:string, url, width, height, ..., img_emb:fsl<float>[768], image:binary |
| jrmiller/coco-2017-siglip2-embeddings | lancedb/coco_clip_embeddings.lance | 287,360 | caption, vector:fsl<float>[1152], image_bytes:binary, ... (the current index.html map) |

- Storage versions 2.0 (laion) and 2.1 (the rest) read fine. Their index types are irrelevant
  (the page uses none).
- `wl_take_json` refuses binary and nested columns; `wl_take_binary` takes one binary/string
  value. **There is no way to read a vector column yet** -- that is part 1.
- Hugging Face's tree API (`https://huggingface.co/api/datasets/<id>/tree/main/<dir>`) is
  CORS-enabled; the module already uses it to list Lance directories.
- The survey that produced the table: `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/survey/`
  (`expected.py` records pylance's values, `run.cjs` drives Chromium, `site/` is the staged page).
  Reuse it for the final check.
- Network through this sandbox's proxy costs seconds per request: one dataset takes 3-6 minutes.
  Timings here are not representative; request and byte counts are.

## Part 1: nanolance glue (`/home/user/nanolance/wasm/weblance_glue.cpp`)

Add one export:

```
// Rows `rows[0..n)` of a fixed_size_list<float> column as n*dim float32s (row-major, in the given
// order), malloc'ed; *out_dim gets dim. NULL and wl_error() on failure. Caller frees with wl_free.
float* wl_take_vectors(int handle, const int64_t* rows, int n, const char* column, int* out_dim);
```

- Follow `wl_take_json`'s structure (the `Take` helper). Null vectors: fill with NaN and let JS
  skip them. Also accept fsl<double> / fsl<half> if trivial (convert to float); otherwise error.
- Add it to the export list in `wasm/build_weblance.sh`. Rebuild; keep `-Oz`. Report the new
  wasm size (raw and gzip) -- it should grow by well under 10 KB.
- **Sampling and request count matter more than anything else.** A take of 5,000 rows scattered
  across a file can mean thousands of HTTP requests. Measure with `wl_stats_json` (local Range
  server: `wasm/test/range_server.py`) for three row patterns on a dataset of ~50k rows: (a)
  5,000 contiguous rows, (b) 10 windows of 500 contiguous rows spread evenly, (c) 5,000 rows at
  a fixed stride. Report requests and bytes for each. The page uses whatever pattern stays under
  ~60 requests and ~2x the vector bytes; (b) is the expected choice (datasets like food101 may be
  sorted by class, so (a) can show only a few classes). Do not change nanolance's take code to
  improve this -- if every pattern is bad, stop and report the numbers.

## Part 2: the page (`/home/user/weblance`)

New files `explore.html`, `explore.js`, `explore-worker.js` (reuse `style.css`, `lib/`, and the
patterns of `app.js` / `worker.js`; do not change `index.html`'s behaviour beyond one link to the
new page, and add a link back).

1. **Input.** A text box taking `https://huggingface.co/datasets/<owner>/<name>` (optionally with
   `/tree/main/...` or a path to a `.lance` directory), plus preset buttons: MNIST, CIFAR-10,
   Oxford Pets, Food-101 and COCO (SigLIP2). Find `.lance` directories with the tree API (top
   level and one level down); if there are several (train/test), show a selector, default to
   the first. URL query `?ds=<owner>/<name>&table=<path>` reproduces a view (shareable links).
2. **Columns.** From `wl_info_json`: image = a binary column (prefer the names image,
   image_bytes, img), confirmed by JPEG/PNG/GIF/WebP magic bytes on one row; vector = a
   fixed_size_list<float> with dim >= 8 (prefer names containing emb or vector); label = a
   string column named label_name/label/class/category/caption-free first, else an int column
   named label/class, else a string `caption` column, else none. Show which columns were picked.
   A dataset with no image column or no vector column gets a clear message (and, with images but
   no vectors, a simple grid of the first 24 images instead of a map).
3. **Sample + PCA.** Read up to 5,000 rows with the pattern from part 1 (all rows if fewer), in
   one worker. L2-normalize, center, top-2 principal components by power iteration with
   deflation (deterministic seed), in the worker, off the main thread. Show progress (rows read,
   MB fetched, requests).
4. **Map.** Canvas scatter like `index.html`'s (reuse its drawing/zoom/pan code where possible).
   - String class column with <= 200 distinct values in the sample: color by class (a fixed
     categorical palette, cycling past its length), label each class at its points' median with
     the collision avoidance `app.js` already has (largest classes first).
   - Int class column: same, labels "class N".
   - Caption column: k-means (k=16) on the 2D layout, each region named by its two most
     distinctive caption words (port the scoring in `tools/make_map.py`: frequency in the region
     times log(N / documents containing the word), with its stop list).
   - None: k-means colors, no labels.
5. **Click.** Nearest sampled points to the click (12), their images fetched with
   `wl_take_binary` across a few workers as `index.html` does, shown with the class label (no
   other text). Show the page's live counters (requests, bytes) as `index.html` does.
6. **Explain in one short line** what is happening (the Lance file is read in place with HTTP
   Range requests; nothing was precomputed) and include the 3-line pylance snippet for making a
   dataset, in a collapsed `<details>`.

Keep it plain JavaScript, no build step, no new dependencies. Phone-width layout must not scroll
sideways. Light and dark themes as the existing page does.

## Tests and checks

- A local, offline Chromium test (extend `wasm/test/`, model `run_browser_test.cjs`): write a
  small Lance dataset with pylance (e.g. 3,000 rows: PNG images, a 32-dim float vector with 5
  well-separated clusters, a label_name column), serve it with `range_server.py`, and check:
  `wl_take_vectors` values equal pylance's exactly for rows 0, 1, 1500, 2999; the explore page's
  detected columns; PCA separates the 5 clusters (e.g. each label's points have a small spread
  relative to the distance between label medians); a click returns 12 images that decode.
  The local Range server must answer the module's listing for the page's local path, as the
  existing local test does; if the page's tree-API discovery cannot work locally, give the page a
  test-only override (`?table_url=...`).
- At the very end, **once**, the Hugging Face check in the background: `run.cjs`'s datasets with
  the page's real flow for mnist and food101 only (food101 to see class diversity of the sample),
  reporting requests, bytes, detected columns and number of distinct labels in the sample. Do not
  repeat it.
- ctest is not affected (glue is WASM-only); still run `ctest --test-dir build -R byte_source`
  if you touch anything outside `wasm/`.

## Stop rules

- If `wl_take_vectors` needs changes to nanolance's reader (`src/`), stop and ask.
- If every sampling pattern needs more than ~200 requests for 5,000 rows, stop and report.
- If PCA on the real datasets is visibly a single blob with all classes mixed (check mnist), say
  so with a screenshot (Playwright `page.screenshot`) and stop; we may switch to umap-js.

## Rules

- No commits, no pushes, no `--update` of anything. The reviewer commits both repos.
- Scratch files only under `/tmp/claude-0/-home-user/8df192dc-e0a5-50ec-ba29-4d1c2c18b24f/scratchpad/explore/`.
  Nothing new in either repository root except the named files.
- Never read big files whole (app.js, weblance_glue.cpp: use `grep -n` and read windows).
- Long runs in the background; wait for the completion notification; no sleep or polling loops.
  Never two Chromium HF runs at once. Kill processes by PID, never with `pkill -f`.
- Do not use any Hugging Face account or token; public datasets only.
- Copy the rebuilt `weblance.{js,wasm}` into `/home/user/weblance/lib/`.

## Report (in this order)

1. Questions and anything you stopped on.
2. Files changed / added, both repos.
3. wasm size before/after; the three sampling patterns' requests and bytes.
4. Local test result; HF result (mnist, food101): requests, MB, seconds, columns picked, distinct
   labels in the sample; paths of two screenshots (mnist and food101 maps).
5. What is left or rough.
