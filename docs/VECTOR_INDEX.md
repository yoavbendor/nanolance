# Vector indexes: Lance 12's IVF_FLAT, IVF_PQ and IVF_HNSW_SQ

What nanolance reads, searches and (Phase 2c) writes, established against pylance 12.0.0 and the
`lance` / `lance-index` 12.0.0 sources. A numpy model of everything below reproduces pylance's
`nearest` results from the index files alone: the same rows, order and distances for IVF_FLAT and
8-bit IVF_PQ under L2, cosine and dot, for default probing, `nprobes`, `refine_factor` and k from 1
to 150; and for 4-bit IVF_PQ the same distances, with rows of equal (quantized) distance in another
order.

## Files

An index is a directory `_indices/<uuid>/` with two Lance files (format 2.2 when the dataset is).

**`index.idx`** has no rows (one non-nullable `__flat_marker: uint64` column) and the schema metadata

| key | value |
|---|---|
| `lance:index` | `{"type":"IVF_PQ","distance_type":"l2"}` (or `IVF_FLAT`; `l2` / `cosine` / `dot`) |
| `lance:ivf` | `"1"`: the global buffer holding the IVF model |
| `lance:flat` | one `""` per partition |

Global buffer 1 is a protobuf `IVF` (`lance-index/protos/index.proto`) whose `centroids_tensor` (field
4) is a `Tensor { data_type = FLOAT32, shape = [num_partitions, dim], data }`; its `offsets` and
`lengths` are zeros there.

**`auxiliary.idx`** holds every indexed row, grouped by partition, with the schema metadata
`lance:ivf = "1"`, `distance_type`, and `storage_metadata`, a JSON list of one JSON string:

- IVF_FLAT: `{"dim":32}`; columns `_rowid: uint64`, `flat: fixed_size_list<float>[dim]` (the vector;
  normalized for cosine).
- IVF_PQ: `{"codebook_position":2,"nbits":8,"num_sub_vectors":4,"dimension":32,"codebook_tensor":[],"transposed":true}`;
  columns `_rowid: uint64`, `__pq_code: fixed_size_list<uint8>[m]` (`[m/2]` for 4 bits).

Global buffer 1 is again an `IVF`, now with the partitions' row `offsets` (cumulative) and `lengths`
(packed varints) and no centroids. For IVF_PQ, global buffer `codebook_position` is a `Tensor`
`FLOAT32 [2^nbits, dim]` whose data is laid out sub-vector major: `[m][2^nbits][dim/m]`.

`_rowid` is the row address (`fragment << 32 | offset`) unless the dataset has stable row ids.

**Codes are transposed per partition**: for a partition of `n` rows the `__pq_code` values are the
bytes `[m][n]`, so byte `s*n + r` is row `r`'s code for sub-vector `s` -- the column's per-row lists
are not the rows' codes. With 4 bits, byte `j` packs sub-vector `2j` in its low nibble and `2j+1` in
the high one.

**Residuals.** For L2 and cosine, PQ encodes `vector - centroid` of the row's partition; dot encodes
the vector. Cosine indexes normalize vectors (and queries) first and then work as L2; the centroids
are not unit vectors.

The manifest entry's `index_details` is a `/lance.table.VectorIndexDetails` (`metric_type`,
`target_partition_size`, `compression` = `pq { num_bits, num_sub_vectors }` or `flat {}`, optional
`hnsw_index_config`). pylance names the index from it: `IVF_PQ`, `IVF_FLAT`, `IVF_SQ`, `IVF_HNSW_*`.

## Search

1. **Query.** Cosine: normalize it.
2. **Partitions.** Distances from the query to every centroid -- squared L2 (L2 and cosine) or
   `1 - q.c` (dot) -- sorted ascending (stable).
3. **How many to probe.** `nprobes = n` fixes it at `min(n, P)`. Otherwise (`minimum_nprobes` 1 and
   `maximum_nprobes` all, unless given):
   - *Adaptive* -- IVF_FLAT, float32, L2 or cosine, `k <= 100`, no `refine_factor > 1`, no
     `maximum_nprobes`: take the partitions with `d - d0 <= margin * d0` (`d0` the nearest; f64
     arithmetic), at least `floor` and at most `cap`, by k bucket (k<=1, k<=10, more):

     | metric | margin | floor | cap |
     |---|---|---|---|
     | L2 | 0.2175 / 0.265 / 0.33 | 5 / 6 / 11 | 19 / 24 / 38 |
     | cosine | 0.235 / 0.2875 / 0.38 | 3 / 8 / 7 | 50 / 77 / 106 |

   - *Legacy* -- everything else (every IVF_PQ, every dot): the partitions with
     `d <= d0 * f` in f32, `f` = 0.6 (k<=1), 7 (k<=10), 81 (more); at least `minimum_nprobes`.

   Then, if the probed partitions gave fewer than k rows (a prefilter, deletions), more partitions
   are searched one at a time, in order, until k rows or `maximum_nprobes`.
4. **Within a partition** each row's distance, keeping the best `k * refine_factor` overall:
   - IVF_FLAT: the metric on the stored vector -- squared L2; cosine `1 - x.q/(|x||q|)`; dot `1 - x.q`.
   - IVF_PQ, 8 bits: a table `[m][256]` from the (residual) query's sub-vectors to the codebook's --
     squared L2, or `1 - q_s.c` for dot -- summed over the row's codes in f32, sub-vector order;
     dot then subtracts `m - 1`.
   - IVF_PQ, 4 bits: the same sums for the partition's first `max(200, k)` rows and its last
     `n % 16`; the rest from a u8 table, `round((t - tmin) * 255 / (qmax - tmin))` with `qmax` the
     largest of those first sums and `tmin` the table's smallest entry, summed with saturation and
     mapped back as `q * (qmax - tmin) / 255 + tmin`.
5. **Refine** (`refine_factor`): the `k * refine_factor` candidates are re-scored exactly from the
   dataset's vectors, and the best k returned with those distances.

`_distance` is that distance: squared L2, `1 - cos`, or `1 - dot` (PQ: their approximations).

## Building (nanolance)

`create_index(column, "IVF_FLAT" | "IVF_PQ", ...)` (`src/vector_index_build.cpp`) writes the files
and manifest entry above with Lance 12's defaults:

1. **Vectors.** The column (a fixed-size list of float32) is scanned with row addresses; null
   vectors and vectors with a NaN or infinite value are left out (Lance's `filter_nan`). Cosine
   normalizes them (and leaves out zero vectors).
2. **Partitions.** `num_partitions`, or rows / `target_partition_size` (4096 for IVF_FLAT, 8192 for
   IVF_PQ), between 1 and 4096.
3. **IVF.** k-means on at most `sample_rate` (256) vectors a partition: random initial centroids,
   at most `max_iters` (50) Lloyd iterations, stopping when the loss moves by less than 1e-4 of
   itself, an empty cluster splitting the largest; past 256 partitions, hierarchical -- 16 clusters,
   each given partitions in proportion to its vectors, recursively. Squared L2 (cosine: of the
   normalized vectors); dot assigns by the largest product. Every vector then goes to its nearest
   centroid.
4. **PQ.** Residuals (vector - its centroid; dot: the vector) of at most 256 * 2^nbits vectors train
   one codebook per sub-vector by L2 k-means, whatever the metric -- Lance's v3 builder does the
   same, and dot only scores the codes. Every row's codes are the nearest entries by L2. The
   sub-vectors train side by side, each on its own seed.
5. **Files.** auxiliary.idx holds the rows partition by partition (ascending row address within
   one); codes are transposed per partition and, with 4 bits, packed two to a byte. index.idx
   holds the centroids and the k-means loss. The manifest entry is a
   `/lance.index.pb.VectorIndexDetails` (metric, target partition size when it decided the count,
   `pq { num_bits, num_sub_vectors }` or `flat {}`, Lance's runtime hints), index version 1.

pylance lists such an index, reports its stats, searches it with nanolance's results, and its
`optimize_indices` adds appended rows to it.

## IVF_HNSW_SQ

An IVF index whose partitions each hold an HNSW graph over 8-bit scalar-quantized (SQ) vectors.
nanolance searches pylance's IVF_HNSW_SQ indexes with pylance's answers (rows, order and distances
for L2 and cosine, with and without deletions, for k, `nprobes`, `refine_factor`, `ef`,
`distance_range`, and filters before or after the search), and builds ones pylance searches as its
own.

### Files

**`auxiliary.idx`**: `_rowid: uint64` and `__sq_code: fixed_size_list<uint8>[dim]`, one row per
indexed vector (not transposed), grouped by partition; schema metadata as above with
`storage_metadata = ["{\"dim\":16,\"num_bits\":8,\"bounds\":{\"start\":-4.49,\"end\":4.73}}"]` --
one pair of bounds for the whole index. Global buffer 1: the `IVF` with the partitions' row offsets
and lengths. All fields are nullable.

**`index.idx`**: the graphs, `__vector_id: uint32`, `__neighbors: list<uint32>`,
`_distance: list<float>` (the neighbours' distances), partition after partition; within a partition,
level 0 (every node, by id) then each level above (its nodes, by id). Node ids are positions in the
partition's rows of `auxiliary.idx`. Schema metadata:

| key | value |
|---|---|
| `lance:index` | `{"type":"IVF_HNSW_SQ","distance_type":"l2"}` |
| `lance:ivf` | `"1"`; global buffer 1 is the `IVF` with the centroids, and the graph rows' offsets and lengths per partition |
| `lance:hnsw` | a JSON list of one JSON string per partition: `{"entry_point":364,"params":{"max_level":7,"m":20,"ef_construction":150,"prefetch_distance":2},"level_offsets":[0,707,736,737,737,737,737,737]}` |

`level_offsets` has `max_level + 1` entries: level `l`'s rows are `[level_offsets[l],
level_offsets[l+1])` of the partition's rows. The manifest details carry `hnsw_index_config
{ max_connections, construction_ef, max_level }` and `sq { num_bits: 8 }`.

pylance writes the neighbour lists with 0-bit bit-packed definition levels and no level buffer (all
valid); nanolance's decoder reads those as all-zero levels.

### Quantization and distances

`code = (v - start) * 255 / (end - start)` in f64, cast to u8 saturating (NaN to 0); all zeros when
`start == end`. Cosine indexes quantize normalized vectors; SQ never uses residuals. With
`value_scale = (end - start) as f32 / 255` and `scale = value_scale^2`:

- L2 and cosine: the query is quantized too; `distance = sum((a - b)^2) as f32 * scale` over codes.
- Dot: the query stays float; `distance = 1 - (start * sum(q) + value_scale * sum(code * q))`.
- Between two stored vectors (building), dot: `1 - (dim * start^2 + start * value_scale *
  (sum(a) + sum(b)) + scale * sum(a * b))`.

`refine_factor` re-scores with the original vectors, as for the other indexes.

### Search

Partitions are chosen as for IVF_PQ (the adaptive probing of IVF_FLAT does not apply). In a
partition (lance-index `HNSW::search`):

1. Greedy descent from the entry point through levels `max_level - 1` down to 1: move to the closest
   neighbour while one is closer.
2. A beam search on level 0 with `ef` (the query's, else `k + k / 2`, where k is `k * refine_factor`;
   `ef < k` is an error), and the best k of it.
3. A prefilter: when every row passes, the plain search; when under 10% pass, an exact scan of the
   passing rows; otherwise the beam only admits passing rows. A `distance_range` admits only results
   in range.

Lance keeps both heaps in Rust's `BinaryHeap`, compared by distance alone, so which of equally distant
rows win depends on the heaps' exact sift order; nanolance's heap reproduces it.

After the first `nprobes` partitions (searched together), Lance's HNSW path searches the rest
`query_parallelism` at a time -- by default `min(compute CPUs, partitions)` -- while fewer than k rows
were found, and keeps whatever those return. nanolance takes the partitions in that order, as if each
finished in turn; when partitions race (a filtered search where several hold candidates) pylance itself
can answer differently from run to run. When fewer rows pass the prefilter than k, Lance stops after the
first probes.

### Building (nanolance)

`create_index(column, "IVF_HNSW_SQ", num_partitions=..., m=20, ef_construction=150, max_level=7)` --
pylance's options and defaults; without `num_partitions`, one per 2^20 rows. lance-c:
`LANCE_INDEX_IVF_HNSW_SQ` with `hnsw_m` (required) and `hnsw_ef_construction`.

1. IVF as for the other indexes (k-means on a sample).
2. SQ bounds: the minimum and maximum value of a sample of `sample_rate * 256` vectors (the model's
   bounds when `optimize_indices` reuses it).
3. Each partition's graph, after lance-index's `HnswBuilder`:
   - node levels drawn as Lance draws them -- `min(-ln(u) / ln(m), max_level - 1)` with `u` from
     rand 0.9's `SmallRng` (Xoshiro256++) seeded with 42 -- so the levels and the entry point (the
     first node of the highest level) are pylance's own;
   - every other node inserted: a greedy descent to its level, then on each of its levels a beam of
     `ef_construction`, Algorithm 4's neighbour heuristic (closest first; keep a candidate closer to
     the node than to every kept one; refill from the rest) down to `m`, and reciprocal edges, each
     neighbour pruned the same way to `2m` on level 0 and `m` above;
   - nodes that level 0 cannot reach from the entry point linked from a nearby reachable node.

Nodes are inserted on nanolance's threads, as Lance inserts them on its own, so the edges differ from
one build to the next unless `NANOLANCE_THREADS=1`; pylance's do too. On 20,000 random 64-dimensional
vectors the recall@10 of a nanolance-built index is that of pylance's own (0.65 / 0.57 / 0.59 for L2 /
cosine / dot against pylance's 0.64 / 0.58 / 0.62), in about the same time.
