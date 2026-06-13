# nanotins — reflective struct → SoA → Arrow → Lance (design notes)

**Status:** design only. Captures how a nanotins "row" struct, described once with
`BOOST_DESCRIBE_STRUCT`, yields **(1)** a Structure-of-Arrays store, **(2)** an Arrow schema, and
**(3)** a Lance table — all derived at compile time, usable from the same source on **CPU and CUDA
(`ex::bulk`)** — and how **per-field byte order** (BE-on-wire for L2/L3) is attached to the struct so
the generic machinery converts it automatically.

The thesis: a parser is **`{plain struct} + {one BOOST_DESCRIBE_STRUCT line} + {one `parse` fn}`** and
everything else (SoA, schema, scatter, table) is generated. C++26 reflection will later delete only
the describe line.

---

## 1. Reflection substrate: `boost::describe`, kept host-side

`BOOST_DESCRIBE_STRUCT(T, (), (f1, f2, …))` records each member's **name** and **constexpr
member-pointer**. We never call `boost`/`mp11` in device code (Boost isn't CUDA-annotated). Instead a
thin compile-time adapter distills the description into **`std`-only** constexpr objects that device
code consumes:

```cpp
// host, compile time — bridges boost::describe -> plain std (device-safe)
template<class T>
inline constexpr auto nt_members = [] {
    using Md = boost::describe::describe_members<T, boost::describe::mod_public>;
    return []<class... D>(boost::mp11::mp_list<D...>) {
        return std::tuple{ D::pointer... };            // std::tuple of member pointers
    }(Md{});
};
template<class T>
inline constexpr auto nt_names = [] {
    using Md = boost::describe::describe_members<T, boost::describe::mod_public>;
    return []<class... D>(boost::mp11::mp_list<D...>) {
        return std::array<const char*, sizeof...(D)>{ D::name... };
    }(Md{});
};
template<class T> inline constexpr std::size_t nt_field_count = std::tuple_size_v<decltype(nt_members<T>())>;
```

Device code (the `store` fold, below) touches only `std::tuple`/`std::array`/member-pointers — all
device-usable. `boost` stays in the schema/SoA setup (host). **This is the practical key to using
`boost::describe` with CUDA.**

When C++26 static reflection ships, `nt_members`/`nt_names` are reimplemented over `^^T` and the
`BOOST_DESCRIBE_STRUCT` lines disappear — nothing else changes.

---

## 2. Byte order attached to the field type (`be<T>` / `le<T>`)

L1 (pcap/pcapng) is host-/little-endian (we only support LE). L2/L3 headers are **big-endian on the
wire**. We attach endianness to the **field type**, so `describe` carries it and the generic store
fold converts per field — precisely (a 6-byte MAC or 16-byte IPv6 address is a byte array and is
**not** swapped; a `be<uint16_t>` length is).

```cpp
// nanotins, device-callable. Stores raw WIRE bytes (no unaligned loads, 1-byte aligned -> safe to
// overlay on packet memory and free of struct padding).
template<class T> struct be {                 // big-endian on the wire
    unsigned char raw[sizeof(T)];
    __host__ __device__ T host() const {      // assemble big-endian bytes into a host T
        T v{}; for (std::size_t k = 0; k < sizeof(T); ++k) v = (v << 8) | raw[k]; return v;
    }
    __host__ __device__ operator T() const { return host(); }   // reads like a plain T
};
template<class T> struct le { unsigned char raw[sizeof(T)];      // little-endian on the wire
    __host__ __device__ T host() const { T v{}; for (std::size_t k=sizeof(T); k--> 0;) v=(v<<8)|raw[k]; return v; }
    __host__ __device__ operator T() const { return host(); } };
```

(We roll our own instead of `boost::endian::big_uint16_t` purely so the conversion is `__device__`.
Same idea, CUDA-callable. `std::byteswap`/`__builtin_bswap*` can replace the byte loop on platforms
that have them.)

A wire header is then a **packed** aggregate that can be overlaid directly on packet bytes:

```cpp
struct Ipv4 {
    std::uint8_t       ver_ihl, dscp_ecn;
    be<std::uint16_t>  total_length, identification, flags_frag;
    std::uint8_t       ttl, protocol;
    be<std::uint16_t>  checksum;
    std::array<std::uint8_t,4> src, dst;     // addresses: byte arrays, NEVER swapped
};
BOOST_DESCRIBE_STRUCT(Ipv4, (), (ver_ihl, dscp_ecn, total_length, identification, flags_frag,
                                 ttl, protocol, checksum, src, dst))
static_assert(std::is_standard_layout_v<Ipv4>);   // overlay-safe; keep it packed (no padding)
```

Reading `hdr.total_length` yields a host `uint16_t` automatically. **The `<LE>(r)` cast you imagined
is real but per-field and implicit** — it happens inside `store` when each field is read through its
type. No cast site, and mixed structs (ints + addresses) are handled correctly.

> Optional struct-level alternative (your "cast before store" mental model): a describe-driven
> `soatins::to_host(r)` that blanket-byteswaps every multi-byte *arithmetic* member (byte arrays are
> skipped because they aren't arithmetic), enabled by a `wire_be` tag on the struct so `store` can
> auto-apply. It's less code per struct but **blanket** — it can't tell a counter from an address that
> happens to be a `uint32_t`. Prefer per-field `be<T>`; offer `to_host` only for all-scalar-BE structs.

---

## 3. `column_traits<F>` — the one type→column bridge

Maps a field type `F` (which may be `be<U>`, `le<U>`, a scalar `U`, a byte array, or a nested
described struct) to: the **SoA element type** (host), the **Arrow format**, and how to **extract**
the host value. This is the single extension point; adding a supported field type = one
specialization.

```cpp
template<class F> struct column_traits;                                   // primary

template<std::integral U> struct column_traits<U> {                       // host-order scalar
    using elem = U;  static constexpr const char* arrow = arrow_format_v<U>;   // "C","S","I","L",signed…
    static constexpr bool variable = false;
    __host__ __device__ static elem get(U v) { return v; }
};
template<class U> struct column_traits<be<U>> {                           // wire BE scalar
    using elem = U;  static constexpr const char* arrow = arrow_format_v<U>;
    static constexpr bool variable = false;
    __host__ __device__ static elem get(const be<U>& f) { return f.host(); }  // swap here
};
template<class U> struct column_traits<le<U>> { /* like be<U> but le::host() */ };
template<std::size_t N> struct column_traits<std::array<std::uint8_t,N>> { // MAC/IPv6/etc.
    static constexpr const char* arrow = fixed_size_binary_format_v<N>;   // Arrow w:N
    static constexpr bool variable = false; /* store = memcpy N bytes, no swap */ };
template<> struct column_traits<soatins::bytes> {                        // variable payload/string
    static constexpr const char* arrow = "z"; static constexpr bool variable = true; };
```

The SoA column for field `K` is `std::vector<column_traits<Field_K>::elem>`; its Arrow child uses
`column_traits<Field_K>::arrow` and `nt_names<T>[K]`.

---

## 3a. Bitfields packed into a (big-endian) word — `bits<Word, field<…>…>`

Native C++ bitfields can't overlay wire data (bit order, unit allocation, and endianness interaction
are all implementation-defined), and pulling bits straight out of raw big-endian bytes is the
error-prone "BE bitfield hell." The robust recipe: **byteswap the whole containing word first (the
`be<>` already does this), then shift+mask on the host value** — bit positions then match the RFC's
MSB-first numbering and are byte-order-independent. So **endianness belongs to the word, not the
sub-field**: tag the word `be<>`, and declare each sub-field's *width* (not a per-field "_BE" flag).

Attach the bit layout to a field type that nests `be<>` and lists sub-fields MSB-first:

```cpp
struct VlanTag {
    be<std::uint16_t> tpid;                                          // 0x8100
    bits<be<std::uint16_t>, field<"pcp",3>, field<"dei",1>, field<"vid",12>> tci;
};
BOOST_DESCRIBE_STRUCT(VlanTag, (), (tpid, tci))

struct Ipv4 {
    bits<std::uint8_t,      field<"version",4>, field<"ihl",4>>      v_ihl;       // 1 byte → no swap
    /* … */
    bits<be<std::uint16_t>, field<"flags",3>,  field<"frag_off",13>> flags_frag;  // straddles 2 BE bytes
    /* … */
};
```

`field<"name", W>` uses a C++20 fixed-string NTTP for the column name. From the width pack nanotins
deduces, at compile time, MSB-first:

```
shift_j = word_bits − (w0 + … + wj)     mask_j = (1u << w_j) − 1
value_j = (word.host() >> shift_j) & mask_j
static_assert( Σ w == word_bits );      // catches the #1 manual bug: a miscounted bit width
```

Each sub-field's column element type is the smallest unsigned int that holds `W` bits (`pcp`→u8,
`vid`→u16). `column_traits<bits<…>>` is a **multi-column** trait: one `bits<>` member expands to one
Lance column per sub-field, each with its own Arrow type and name; the `store` extractor for sub-field
*j* is exactly the shift+mask above on the word's host value. The parser stays a plain overlay
(`bits<be<uint16>>` is layout-identical to the two wire bytes — no hand-written shifting anywhere).
Keeping the raw word as an extra column is opt-in via a tag; default is sub-fields only (queryable,
compact, bitpack-friendly).

This *is* your `fld:11` idea — with the correction that the layout (and the swap) is declared on the
word, so a field that straddles bytes (IPv4 `frag_off`, TCP flags) is handled correctly and portably.

## 4. SoA: owning host type + POD device view

```cpp
template<class T> class soa {                 // host, owning — one column buffer per field
    // tuple< vector<column_traits<Field_k>::elem> ... >  built from nt_members<T>
public:
    void resize(std::size_t n);
    soa_view<T> view();                       // hand to ex::bulk
    ArrowArray  to_arrow();                    // fixed-width column.data() IS the Arrow data buffer (zero copy)
};

template<class T> struct soa_view {           // POD: tuple<elem*...> — trivially copyable -> captured by value
    template<std::size_t K> __host__ __device__ auto* col() const;        // K-th column base pointer
    __host__ __device__ void store(std::size_t i, const T& r) const {
        constexpr auto M = nt_members<T>();
        [&]<std::size_t... K>(std::index_sequence<K...>) {
            ((col<K>()[i] = column_traits<member_type<T,K>>::get(r.*std::get<K>(M))), ...);  // per-field, converts BE→host
        }(std::make_index_sequence<nt_field_count<T>>{});
    }
    // load(i)->T is the symmetric fold for reads.
};
```

`store` is the whole story: an unrolled fold that, per field, reads through its type (so `be<U>`
swaps) and writes the host value into column `K` at row `i`. On GPU, adjacent threads write
`col<K>[i]` and `col<K>[i+1]` → **coalesced per column**. No allocation, no boost, `__device__`-clean.

Generalization for multi-column field types: members are first flattened to a compile-time
`columns_of<T>` list, where each entry is `{name, elem, arrow, extractor(const T&)→elem}`. A plain
`be<U>`/`U` member contributes one entry; a `bits<…>` member contributes one per sub-field (§3a); a
byte array contributes one fixed-size-binary entry. `store`, `to_arrow`, and `arrow_schema` all
iterate `columns_of<T>` (not the raw member list), so the fold above is really *per column* — which is
what makes bitfields, endianness, and plain fields one uniform mechanism.

---

## 5. `ex::bulk` ergonomics — plain struct in, SoA out

Fill a local row and `store` it. Do **not** attempt an `out[i].field = x` named-reference proxy —
that needs the proxy to mirror the struct's members, i.e. exactly the C++26 reflection feature we
don't have. Local-struct-then-store is both simpler and GPU-ideal (register struct → coalesced
column writes).

```cpp
auto sv = packets.view();                                  // POD device view (captured by value)
stdexec::bulk(sched, n, [=] __host__ __device__ (std::size_t i) {
    Ipv4 hdr = overlay<Ipv4>(file, refs[i]);               // or parse_*(...) -> register struct
    sv.store(i, hdr);                                       // be<> fields convert to host here
});
// host: ArrowArray a = packets.to_arrow();  ArrowSchema s = arrow_schema<Ipv4>();
//       nano_lance_write_batch(&w, &a, &s);                ← straight into nanolance
```

**Two sides, one source:** the owning `soa<T>` (host) allocates and exposes `to_arrow()` +
`arrow_schema<T>()` for nanolance; the `soa_view<T>` (POD pointers) is the device side captured into
`ex::bulk`. The same `T` + `BOOST_DESCRIBE_STRUCT` drives both.

---

## 6. Arrow schema generation

```cpp
template<class T> ArrowSchema arrow_schema() {             // built once on host
    // child K: { name = nt_names<T>[K], format = column_traits<member_type<T,K>>::arrow }
}
```
Because `store` writes **host-order** values, the SoA (and therefore the Lance file) is native LE —
correct for Arrow. Wire byte-order is fully consumed at parse/store time and never reaches disk.

---

## 7. Fixed vs variable, and the external-payload synergy

- **All-scalar rows are a pure coalesced scatter** on GPU — no prefix sum. The L1 `packets` row is
  all scalar *because the payload is stored external* (`payload_uri`/`off`/`size`), so the GPU hot
  path needs no variable-width handling. The external-reference design and the GPU design reinforce
  each other.
- **Variable-width fields** (a string `comment`, or extracted PDU bytes via `soatins::bytes`) need
  offsets = prefix-sum of per-row lengths → a two-phase bulk (lengths → scan → write at `offset[i]`).
  Keep these off the all-scalar fast path; route them through the length→scan→write path when needed.

---

## 8. Extending to L2/L3 PDUs — the payoff

Adding a protocol = a struct + a describe line + a (often trivial, overlay-only) parse fn + a registry
entry:

```cpp
struct UdpHdr { be<std::uint16_t> src, dst, len, csum; };
BOOST_DESCRIBE_STRUCT(UdpHdr, (), (src, dst, len, csum))
register_parser(/*ip_proto*/17, [](soatins::bytes p, UdpHdr& o){ o = overlay<UdpHdr>(p); return true; });
```

From that you automatically get a `soa<UdpHdr>`, an Arrow schema, and a Lance table — endianness
handled by the `be<>` field types, scatter handled by the generic `store`. "Save them all as they
are" = one Lance table per PDU type (or a column group), zero per-protocol schema/encoder code. New
parsers compose into nanolance with no new glue.

---

## 9. Language level & dependencies
- **C++20** suffices: concepts, `std::span`, member-pointer folds, lambdas in unevaluated/constexpr
  context. Matches what `stdexec`/`ex::bulk` needs.
- **C++23** optional niceties (`std::byteswap`, deducing-this, `if consteval`); gate behind feature
  tests so the CUDA toolchain's level doesn't block the build.
- **Dependencies:** `boost::describe` + `boost::mp11` (header-only, host-side only). No `boost` in
  device translation units — only the distilled `std::tuple`/`std::array` adapters cross into device
  code.
- **C++26 static reflection** removes `BOOST_DESCRIBE_STRUCT` (and the boost dep) without touching
  `soa`/`store`/`arrow_schema`/`column_traits`. The `be<>/le<>` wrappers stay — endianness is a wire
  fact, not a reflection feature.

---

## 10. CUDA-safety checklist (must hold as the design grows)
- `store`/`load`/`parse`/`be<>::host` are `__host__ __device__`, `noexcept`, allocation-free, no
  globals, no `boost`/heavy-STL in the body.
- Overlay structs are standard-layout and **packed** (no padding) so they match wire bytes; `be<>`
  uses a `unsigned char[N]` payload to stay 1-aligned and avoid unaligned loads.
- `soa_view<T>` and `BlockRef`/`Ipv4`/… are trivially copyable (host↔device memcpy / capture-by-value).
- Variable-width columns are the only thing that breaks pure scatter; keep them off the fast path.

---

## 11. Open items
- `column_traits` coverage table to pin down: signed ints, `float`/`double` (Arrow "f"/"g"), `bool`
  (Arrow "b", bit-packed — note Arrow bool is 1 bit, our scalar path is byte-wide; needs a small
  adapter), enums (store underlying), nested described struct → Arrow struct child (recursion).
- Whether nested PDUs become Arrow **struct columns** (one wide table per packet with nested headers)
  vs **separate tables** keyed by a packet id. Recommendation: separate tables per PDU type for step
  1 (simplest, matches "save them all"); struct-column nesting later.
- A `wire_be` struct tag + `to_host()` blanket transform as a convenience for all-scalar-BE headers
  (secondary to per-field `be<>`).
