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
    enum class Kind { List, LargeList, Struct } kind;
    std::vector<std::int64_t> offsets; // List only: parent_count + 1 entries, starting at 0
    std::vector<std::uint8_t> validity; // empty = all valid
    std::uint64_t null_count = 0;
};
std::vector<NestedLayer> layers;       // empty for a non-nested column: no behaviour change
```

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

FullZip with `bits_rep > 0` (C8), `fixed_size_list` of lists (Lance itself does not support it:
`decimate` is `todo!()` there), maps (C6: `list<struct<key, value>>` with Arrow `+m`), and the write
side (Phase D).
