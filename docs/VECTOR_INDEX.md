# Vector indexes: Lance 12's IVF_FLAT and IVF_PQ

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
