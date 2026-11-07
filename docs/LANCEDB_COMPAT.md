# LanceDB compatibility (`nanolance.lancedb`)

`nanolance.lancedb` is LanceDB's Python API (lancedb 0.39) over nanolance's Lance datasets: no Rust core, no Tokio.
A database is a directory, local or an object-store prefix (`s3://...` with `storage_options`), with one
`<name>.lance` dataset per table. LanceDB and pylance open those tables, and nanolance opens theirs.

```python
import nanolance.lancedb as lancedb
from nanolance.lancedb.pydantic import LanceModel, Vector

class Doc(LanceModel):
    id: str
    vector: Vector(384)

db = await lancedb.connect_async("./db")              # or lancedb.connect("./db") for the sync API
t = await db.create_table("docs", schema=Doc, exist_ok=True)
await t.merge_insert("id").when_matched_update_all().when_not_matched_insert_all().execute(rows)
hits = await t.vector_search(q).distance_type("cosine").where("id != 'x'").limit(10).to_list()
```

You can also run unmodified LanceDB code. `nanolance.lancedb.install_as_lancedb()` makes `import lancedb` load
nanolance, and `import lance` too, because `Table.to_lance()` returns a nanolance dataset. Two ways to apply it:
- **A test suite:** `pytest -p nanolance.lancedb._pytest_plugin`.
- **Processes the code spawns** (multiprocessing "spawn" workers): put a `sitecustomize.py` that calls
  `install_as_lancedb()` on their `PYTHONPATH`.

## What is covered

| | |
|---|---|
| Connection | `connect`, `connect_async`, `table_names`, `list_tables`, `create_table` (data and/or schema, a pyarrow schema or a `LanceModel`; `mode` create/overwrite, `exist_ok`), `open_table`, `drop_table` (`ignore_missing`), `drop_all_tables` |
| Data in | lists of dicts or models, dicts, pyarrow tables/batches/readers, pandas and polars frames. Columns are reordered to the table's, absent nullable columns become null, and a list<float> column of one length is inferred as a vector. `on_bad_vectors` (error/drop/fill/null). An unknown field, nested ones included, raises lancedb's own error text |
| Table | `add`, `delete`, `update` (values or SQL), `merge_insert` (update all, insert all, delete by source), `count_rows`, `head`, `to_arrow`/`to_pandas`/`to_polars`, `schema`, `version`, `list_versions`, `checkout` (a checked-out table refuses writes), `checkout_latest`, `restore`, `optimize`, `stats`, `add/alter/drop_columns`, `to_lance` |
| Indexes | `create_index` with `IvfFlat`, `IvfPq`, `IvfSq`, `IvfRq`, `HnswPq`/`HnswSq`/`HnswFlat`, `BTree`, `Bitmap`, `LabelList`, `FTS`; the sync API's `create_index(metric=, num_partitions=, index_type=)`, `create_scalar_index`, `create_fts_index`; `list_indices`, `index_stats`, `drop_index` |
| Queries | `query()` / `search()` / `vector_search()` / `nearest_to`; `where` (prefilter by default) / `postfilter`, `select` (names or expressions), `limit`, `offset`, `distance_type` (squared L2 by default), `nprobes`, `refine_factor`, `ef`, `bypass_vector_index`, `distance_range`, `with_row_id`, several query vectors (`query_index`), full-text queries; `to_list` / `to_arrow` / `to_pandas` / `to_polars` / `to_pydantic` / `to_batches` |

The defaults are lancedb's:
- **Limits:** a vector query returns 10 rows; an async `query()` without `limit` returns every row.
- **Errors:** "Table 'x' already exists", "Table 'x' was not found".
- **Result objects:** `AddResult`, `DeleteResult`, `UpdateResult` and `MergeResult`, with the same fields.

Consistency: a table that isn't checked out reads the latest version on every call, so it sees other writers'
commits at once.

The async API runs each native call on a worker thread (`asyncio.to_thread`). nanolance releases the GIL while it
reads, searches and commits, so concurrent queries overlap. Eight concurrent flat searches over 200k × 384 vectors
took 1.99 s with `asyncio.gather` against 4.89 s one after another, on 4 cores.

**Not implemented:** LanceDB Cloud (`db://`), embedding functions (callers embed and pass vectors), rerankers and
hybrid search, namespaces, materialized views, and `rename_table` (LanceDB OSS refuses it too).

## Proof: cognee

[cognee](https://github.com/topoteretes/cognee) (commit 525b1d1) uses LanceDB as its default vector store, through
lancedb's async API and, for bounded compaction, pylance's step-by-step `lance.optimize.Compaction`. With nanolance
installed as `lancedb` and `lance`, cognee's own tests run unchanged:

| Suite | lancedb 0.39 + pylance 12 | nanolance |
|---|---|---|
| LanceDB adapter unit tests (19 files) | 108 passed, 16 skipped, 26.7 s | 108 passed, 16 skipped (the same skips: Postgres, Turso), 12.5 s |
| Journeys (remember → cognify → recall, update, forget, sessions, HTTP API, 30/30 gold questions; mock LLM) | 14 passed, 3 skipped, 5:45 | 14 passed, 3 skipped, 5:22 |

`bindings/python/tests/test_cognee_workload.py` keeps cognee's workload in nanolance's CI:
- exact cosine search with prefilters on `array_has_any(payload.belongs_to_set, ...)`
- `merge_insert` upserts
- bounded compaction, and version cleanup by number

It requires the same answers as pylance.

`bindings/python/tests/test_lancedb_api.py` pins the lancedb behaviour this layer reproduces.
