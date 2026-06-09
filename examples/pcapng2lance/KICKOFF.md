# pcapng→lance + nanotins — implementation kickoff (notes to future self)

Read first, with `DESIGN.md` (step-1 scope, locked decisions §8) and `NANOTINS_REFLECTION.md`
(the struct→SoA→Arrow→Lance machinery, endianness, bitfields). Those two are the spec; this is the
**order to build it in** and the **traps to avoid**.

## Recommended build order (and why)

**Not** nanotins-first, **not** fully parallel. Build in this sequence — it front-loads the shared,
risky C++ and keeps a working oracle at every step:

- **M0 — shared reflection core (CPU only, no pcap, no CUDA).** `be<T>`/`le<T>`, `bits<Word,
  field<…>…>`, `column_traits`, `columns_of<T>` (the flattened column list), `soa<T>` (owning) +
  `soa_view<T>` (POD), `store`/`load`, `arrow_schema<T>()`, and the boost::describe→`std` adapter
  (`nt_members`/`nt_names`). Prove it on 2–3 toy structs with a unit test: struct → `soa` →
  `to_arrow()` → `nano_lance_write_batch` → read back (nanolance + rust lance) bit-exact. **This is the
  genuinely novel/risky part and both the example and nanotins depend on it — get it right in
  isolation before any protocol code.**
- **M1 — example end-to-end, CPU reference parser.** Implement `pcap_blocks.hpp` with a plain CPU
  `pcap_blocks_ref.cpp` (L1 only: pcap + pcapng SHB/IDB/EPB + options). Wire: scan → interface table →
  bulk-parse (a plain `for` loop today) → fill the packet `soa` (external payload columns) →
  nanolance write. Ship the golden + interop + external-offset tests. **Milestone = a working,
  useful pcapng→Lance tool _and_ the acceptance test suite for the seam.**
- **M2 — extract nanotins.** Move the M0 core + the seam implementation into the nanotins repo. Add
  the **CUDA / `ex::bulk`** implementation of `parse_epbs_bulk` (one thread per `BlockRef`, the
  `store` fold writes coalesced columns). The example's tests run **unchanged** against nanotins — that
  is the proof the swap is correct. Only now introduce a GPU.
- **M3 — L2/L3 protocols.** Ethernet/VLAN/IPv4/IPv6/TCP/UDP as `be<>`/`bits<>` structs + describe +
  overlay parse fns + a registry; each PDU type → its own Lance table. Then internal PDUs in UDP.

Rationale: the example is nanotins' **test harness and golden** regardless of order; so the example's
CPU path must exist first to *be* the oracle. nanotins-first means building CUDA before anything runs
end-to-end, with nothing to validate against — bad feedback loop. Parallel-from-scratch drifts without
the shared core + golden. **Correctness on CPU first (reference impl = oracle), performance (CUDA)
second — never debug CUDA and format-correctness at the same time.**

## Principles to hold (cheap now, painful to retrofit)
- **Keep `boost` strictly host-side from day 1.** Device code sees only the distilled `std::tuple`/
  `std::array` adapters. Retrofitting CUDA-safety after boost has leaked into device TUs is misery.
- **The seam (`pcap_blocks.hpp`) is sacred.** Tests target it; it is nanotins' acceptance contract.
  The example must never reach around it into a concrete parser.
- **Start all-scalar.** The packet row is 100% fixed-width *because the payload is external*
  (`uri`/`off`/`size`). That means M1's first end-to-end has **no variable-width SoA and no
  prefix-sum** — the simplest possible path, and it maps straight onto the encoders we already
  shipped (bitpacking / RLE / dict-RLE / ConstantLayout). Add variable-width columns (comments, PDU
  byte slices) only *after* the scalar path round-trips.
- **Verify the premise every milestone:** `nano_lance_fetch_external_blob(uri, off, size)` must equal
  the source file's bytes at `[off, off+size)`. The whole design rests on those offsets being right.
- **Lance interop from M0.** The CI loop (`.github/workflows/linux-bench.yml`) and `bench.py` exist;
  reuse the "rust lance reads our file" check as a gate. Note: the workflow's path filter is
  `src|include|tools|tests|generated|CMakeLists|workflow` — **it will NOT build `examples/`**. To get
  the example into CI, add `examples/**` to the workflow `paths` and a build step that installs boost
  (`boost::describe` + `boost::mp11`, header-only).

## Gotchas already identified (don't rediscover them)
- **Overlay structs must be `standard_layout` and packed** (no padding) to match wire bytes; `be<>`/
  `bits<>` store `unsigned char[N]` to stay 1-aligned and dodge unaligned-load UB. `static_assert`
  size + `is_standard_layout`.
- **Bitfields:** byteswap the *word* (`be<>`) first, then shift+mask on the host value (§3a). Declare
  widths MSB-first; `static_assert(Σ widths == word_bits)`. Don't tag sub-fields "BE".
- **Arrow `bool` is 1 bit**, but the scalar SoA path is byte-wide → needs a small adapter when a field
  is `bool` (bit-pack on `to_arrow`). Decide early or forbid `bool` row fields in M0.
- **Variable-width on GPU needs length→scan→write** (prefix-sum for offsets). Keep it off the
  all-scalar fast path; it's only for strings/`bytes` fields. (Synergy: external payloads avoid it.)
- **`field<"name", W>`** needs the C++20 `fixed_string` NTTP (class-type non-type template param).
  Get a 5-line `fixed_string` working in M0; everything else leans on it.
- **Device `store`:** member-pointer + `index_sequence` fold; `noexcept`, allocation-free, no STL
  containers / no boost in the body. Same rules as the existing nanolance hot paths.

## Locked decisions (from DESIGN.md §8 — do not relitigate)
KV dataset metadata for SHB/IDB options · store `ts_raw` + denormalized `ts_resol` · `payload_uri`
per input file (ConstantLayout single / dict-RLE batched) · denormalize `link_type`/`ts_resol` per
row · `std::span<const std::uint8_t>` (`Bytes`) across the seam · `boost::describe`'s
`BOOST_DESCRIBE_STRUCT` as the reflection source.

## Open items to settle while implementing (not blockers)
- `column_traits` coverage: signed ints, `float`/`double` ("f"/"g"), `bool` (see gotcha), enums (store
  underlying), nested described struct → Arrow struct child.
- Nested PDUs: separate Lance table per PDU type (recommended for step 1) vs one wide table with
  struct columns. Keep separate tables until there's a reason not to.
- Whether to also keep the raw bitfield word as a column (opt-in tag); default sub-fields only.
- nanolance touchpoint: fixed-width `soa` columns can hand `column.data()` straight to an `ArrowArray`
  buffer (zero-copy); confirm lifetimes (the `soa` must outlive the `write_batch`/`commit`).

## State of the world at handoff
nanolance is feature-complete for this example: writer encodings (bitpacking, RLE, dict-RLE,
ConstantLayout, zstd) all Lance-interop bit-exact; reader optimized ~2.6×; CI bench/profile loop live;
everything on `main`. The clone of the Lance Rust source is at `C:/tmp/lance` (format reference).
Memory notes capture the on-disk encoding formats. Start a fresh session at M0.
