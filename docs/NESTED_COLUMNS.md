# Nested columns: how list data flows from page to Arrow

_Design note for roadmap Phase C (C0). It fixes the data model before any list code lands, because
the part most likely to go wrong — slicing and compacting nested columns for row ranges and
deletions — goes wrong silently._

## What a list page holds

Verified against pylance 2.x pages (`nlance-pagelayout -v`) and `rust/lance-encoding/src/repdef.rs`:

- **One physical column per leaf.** `list<int64>` is one column (`c.item`); `list<struct<a, b>>` is two,
  each carrying the full repetition and definition levels of every layer above it.
- **Levels, not offsets.** Each MiniBlock chunk stores a repetition-level buffer (`rep_compression`,
  e.g. `Bitpacked{16, Flat(1)}`) and, when any layer can be null or empty, a definition-level buffer.
  Both have `num_levels` entries — one per item **plus one per empty or null list**.
- **Values only for item slots.** An empty or null list has a level but no value; a null *item* has
  both (a garbage value slot, as in a flat nullable column). Checked: `[[1,2],[],None,[None,4]]` is
  4 values, 6 levels.
- **A chunk's value count comes from its metadata word**, `log_num_values` (non-final chunks hold
  `2^log` values, the last one the remainder of `num_items`), not from its level count. The current
  splitter derives counts from levels, which is only right when there is no repetition.
- **`layers` is innermost first** (`DefinitionInterpretation`): item layer, then one entry per list
  or struct above it. Level numbering is per page: a page with no nulls may use a different layer
  set from its neighbour.
- **Rows never span pages**, so each page unravels on its own.
- Page buffer 2 is the repetition index (random access); a full scan ignores it.

## Unravelling (C1)

A port of `RepDefUnraveler`, innermost layer first:

1. **Item validity** — levels visible at repetition 0 (`levels_to_rep[def] == 0`) are item slots;
   `def == 0` is a valid item.
2. **Each list layer** — walk the levels: `rep != 0` starts a list (valid, null, empty, or invisible
   at this depth, by `def`); `rep == 0` continues one. Emit that layer's offsets and validity, then
   compact the stream to list starts only, with `rep - 1`: that is the next layer's input.
3. **Struct layers** (`NullableItem` above a list) unravel as validity at their own depth.

It takes `(rep, def, layers, num_items)` and returns offsets and validity per layer, with no I/O —
table-driven tests use Lance's own `repdef.rs` vectors, and it gets a fuzz target.

## Data model

`ColumnValues` keeps describing the **leaf items**: `fixed` / `variable` hold the values and
`validity` holds item validity, exactly as today. A list column adds:

```cpp
struct NestedLayer {                   // one per list/struct layer, OUTERMOST first
    bool is_list;                      // false: a struct -- validity only, one child per entry
    std::vector<std::int64_t> offsets; // lists only: length + 1 entries, starting at 0
    std::vector<std::uint8_t> validity; // empty = all valid
    std::uint64_t null_count = 0;
};
std::vector<NestedLayer> layers;       // empty for a non-nested column: no behaviour change
```

A column takes this path when any page has repetition levels **or a nullable layer above the item**
— a struct that can be null. Everything else, by far the common case, keeps the flat decoder.
Whether a list is `large_list` is the schema's business, decided when the Arrow array is built.

Offsets are held as `int64` in memory and narrowed to Arrow's `int32` only when the array is
built, with a checked failure past `INT32_MAX`. Row count = `layers[0]`'s entry count for a list,
as it is today for a flat column.

**Pages concatenate layer by layer.** Page N's offsets are rebased by the current item (or
child-list) count and appended without their leading 0; validity bits append at the running count.

## Slicing and compaction

The existing row-range and deletion paths operate on one leaf's `ColumnValues`. For nested columns
they walk the layers from the outside in:

- **Row range `[first, first + count)`**: layer 0 selects offsets `first .. first + count`; the
  children it covers are `[off[first], off[first + count])`. Rebase to start at 0 and carry that
  child range into the next layer. The last range selects leaf items; slice values and item
  validity as today.
- **Deletions (kept-row list)**: layer 0 turns kept rows into **kept child ranges** (runs, not
  per-item indices — a deletion vector can drop one row of a 10,000-item list), builds new offsets
  by concatenating range lengths, and passes the ranges down. The leaf compacts by ranges.
- Validity at each layer is sliced or compacted with the same selection as that layer's offsets.

Both are pure functions of `(layers, selection)` and get property tests: for random nested Arrow
arrays, `slice(read(file))` must equal `read(file).slice(...)` from pyarrow, and a deletion must equal
pylance's `delete` then `to_table()`. The FSL element-bitmap bug class (a second bitmap not cut with
the first) is exactly what this guards against.

## Assembling Arrow

The table reader builds a list array per `List` layer (`+l`, or `+L` for `large_list`) around the
next layer, and a struct array per `Struct` layer. For `list<struct<a, b>>` the offsets and validity
of the shared layers come from the first leaf; every other leaf's copy must match exactly, or the
read is refused by name. They are written together, so disagreement means a corrupt file.

## What this does not cover yet

FullZip with `bits_rep > 0` (C8), and `fixed_size_list` inside or around a list.

## The write side (Phase D)

The inverse, piece by piece:

- **Ingest** walks each leaf's path from its top-level Arrow column, recording one layer per list
  or struct: validity bits, and for a list its offsets rebased to this column's running child count.
  Physical indices compose as Arrow defines them: a list's offsets are logical indices into its
  child (plus the child's own `offset`); a struct's children are not sliced with it (child physical
  index = child offset + the struct's physical index). The leaf values are appended for the
  children the batch's rows span, garbage under null lists included.
- **`repdef::serialize`** turns a run of rows into levels, choosing each layer's kind from what
  those rows contain, and lists the leaf entries that get a value slot. A null or empty list gets a
  level and no slot, and its children are never visited.
- **Pages** are cut at row boundaries (up to 32,768 rows, ~8 MiB of values) and split into
  mini-block chunks of 1,024 values (`log_num_values = 10`; the last chunk holds the rest). Levels go
  to chunks by Lance's slicer rule: a chunk takes levels until it has covered its values, and any
  trailing levels with no value slot (empty or null lists) go to the next. So a row can span
  chunks; the depth-1 repetition index records, per chunk, the rows that finish in it and the
  levels left over from the row still open at its end.
- **Levels** are `Bitpacked{16, Flat(w)}` at the page's widest level, per chunk: whole 1,024-level
  blocks plus a tail kept raw (`u16`) when that is cheaper than padding it to a block. A chunk
  holds at most 65,535 levels (a `u16` count); a single row that needs more in one chunk is refused
  by name.
- **Items** reuse the flat encodings: `InlineBitpacking` for integers, bits for booleans, `Flat` for
  other fixed widths, and for strings either `Variable` chunks (`u32` offsets, padded to 8) or a
  per-page dictionary (`InlineBitpacking(32)` indices, the dictionary as one `Variable` block) when
  it is at most half the items and under 80% of the plain size.
- A page with no values at all is a `ConstantLayout` with raw `[rep, def]` buffers, exactly as
  pylance writes one.
- A leaf under structs that are never null keeps the flat encodings: nested pages are written only
  for a list or an actual struct null (`ColumnValues::needs_nested_pages`).
