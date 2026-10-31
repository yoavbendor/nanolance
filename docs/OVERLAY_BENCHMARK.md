# Data overlays: cost against update() and with overlays stacked

`tools/bench_overlay.py` (run 2026-10-31, the dev container, 4 cores; not part of the CI matrix). It patches one
int64 column of a 1M-row, one-fragment dataset either as a DataOverlay (a small data file plus a commit; the
fragment's files stay) or with `update()`, then reads with 0, 1 and 10 overlays stacked. A runnable example of
writing and reading an overlay is `bindings/python/examples/data_overlay.py`.

### Patch 1,000,000 rows, one int64 column (best of 3)

| rows patched | engine | overlay | update() | bytes added: overlay / update |
|---|---|---|---|---|
| 1,000 (0.1%) | nanolance | 3.9 ms | 21.2 ms | 10,270 / 32,859 |
| 1,000 (0.1%) | pylance | 3.8 ms | 27.2 ms | 12,386 / 32,271 |
| 10,000 (1.0%) | nanolance | 6.6 ms | 25.4 ms | 92,487 / 201,915 |
| 10,000 (1.0%) | pylance | 6.1 ms | 34.2 ms | 112,609 / 183,453 |
| 100,000 (10.0%) | nanolance | 23.9 ms | 53.0 ms | 757,562 / 1,965,503 |
| 100,000 (10.0%) | pylance | 17.2 ms | 47.5 ms | 884,052 / 1,694,862 |

### Read with overlays stacked (best of 3; 1% of rows patched by each overlay)

| overlays | engine | full scan | `val` scan | take 1,000 rows |
|---|---|---|---|---|
| 0 | nanolance | 7.3 ms | 4.0 ms | 1.2 ms |
| 0 | pylance | 27.6 ms | 10.4 ms | 15.6 ms |
| 1 | nanolance | 9.9 ms | 11.2 ms | 1.3 ms |
| 1 | pylance | 33.3 ms | 24.2 ms | 13.7 ms |
| 10 | nanolance | 12.0 ms | 18.1 ms | 2.7 ms |
| 10 | pylance | 79.8 ms | 76.7 ms | 19.1 ms |

What this run changed: a projected scan with 10 overlays took 168 ms (one binary search per row and overlay; now one
merge per overlay), committing a 100,000-row overlay took 82 ms (its offsets were encoded and decoded value by value
in Python; now numpy), and `update(where="id % 100 = 0")` took 86 ms (the filter's arithmetic ran row by row; now a
column at a time, `test_arith_filters.py`). Overlay to overlay merging is not done, so reads slow as overlays stack;
compaction folds them into the base.

Rerun: `python tools/bench_overlay.py --rows 1000000 --repeat 3` (needs pylance 12; sets
`LANCE_ENABLE_UNSTABLE_DATA_OVERLAY_FILES=1` for it).
