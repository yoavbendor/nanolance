# Linux CI results (auto-generated — do not edit)

_2026-11-22 09:37:24 UTC · commit `b5d76eb`_

## Environment
```
Linux runnervm0hi8g 7.0.0-1020-azure #21-Ubuntu SMP PREEMPT Tue Oct 27 22:34:38 UTC 2026 x86_64 GNU/Linux
Ubuntu clang version 19.1.7 (20ubuntu4)
cmake version 4.5.0
```

## Tests
```
smoke                =   7.16 sec*proc (138 tests)

Total Test time (real) =   7.32 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         17.32       17.32    4.189      4.89                (1.00x vs pq)
rust lance              9.23        9.23    3.472      3.57                (0.83x vs pq)
nanolance (cli)         8.30       16.27    3.465      0.51         3.96   (0.83x vs pq)
nanolance (py)          1.95        1.95    3.465      0.45                (0.83x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         17.71       17.71    5.212      2.71                (1.00x vs pq)
rust lance              4.35        4.35    8.485      1.61                (1.63x vs pq)
nanolance (cli)         3.54        6.04    8.488      0.88         1.53   (1.63x vs pq)
nanolance (py)          2.40        2.40    8.488      0.94                (1.63x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         22.40       22.40   15.513      6.49                (1.00x vs pq)
rust lance             14.82       14.82   18.937      3.40                (1.22x vs pq)
nanolance (cli)        19.73       24.08   17.989      4.38         5.77   (1.16x vs pq)
nanolance (py)         19.83       19.83   17.989      4.32                (1.16x vs pq)

========== float_smooth  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         20.12       20.12   14.600      2.28                (1.00x vs pq)
rust lance              5.04        5.04    8.686      3.00                (0.59x vs pq)
nanolance (cli)         4.84        7.15    8.609      0.81         2.41   (0.59x vs pq)
nanolance (py)          3.69        3.69    8.609      0.52                (0.59x vs pq)

========== bool_flags  (200000 rows, 1 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)          1.56        1.56    0.128      1.14                (1.00x vs pq)
rust lance              1.24        1.24    0.129      0.92                (1.01x vs pq)
nanolance (cli)         0.69        2.04    0.127      0.07         0.89   (1.00x vs pq)
nanolance (py)          0.31        0.31    0.127      0.09                (1.00x vs pq)

note: best of 5 writes / 7 reads. parquet, rust lance and nanolance (py) write and read in this Python process, from the Arrow table in memory: compare these three. nanolance (cli) runs arrowipc2lance and nlbench as subprocesses: write(core)=ingest+encode+commit in a fresh process (its first-touch page faults included), write(proc)=its whole wall clock (process start + Arrow IPC parse from stdin + write); read ms=nlbench; read(lance)=rust lance reading the file nanolance wrote.
```

## Native-read profile (callgrind, nlbench on pcap_ref)
```
--------------------------------------------------------------------------------
Profile data file '/tmp/cg.out' (creator: callgrind-3.26.0)
--------------------------------------------------------------------------------
I1 cache: 
D1 cache: 
LL cache: 
Timerange: Basic block 0 - 13925561
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 17808, part 1)
Events recorded:  Ir
Events shown:     Ir
Event sort order: Ir
Thresholds:       98
Include dirs:     
User annotated:   
Auto-annotation:  on

--------------------------------------------------------------------------------
Ir                  
--------------------------------------------------------------------------------
21,212,153 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
11,455,448 (54.00%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 3,020,082 (14.24%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 1,932,057 ( 9.11%)  ???:bool nano_lance::(anonymous namespace)::decode_dict_rle_column(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, nano_lance::(anonymous namespace)::ColumnEncodingPlan const&, std::vector<unsigned char, std::allocator<unsigned char> > const*)::$_0::operator()<unsigned int>(unsigned int) const [/home/runner/work/nanolance/nanolance/build/nlbench]
   923,505 ( 4.35%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj28EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   529,974 ( 2.50%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   525,066 ( 2.48%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   459,580 ( 2.17%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj27EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   237,270 ( 1.12%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj29EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   226,050 ( 1.07%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj26EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   200,236 ( 0.94%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   181,703 ( 0.86%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object_no_relro
   146,991 ( 0.69%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object_no_relro [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   128,593 ( 0.61%)  ./elf/./elf/do-rel.h:_dl_relocate_object_no_relro
   122,614 ( 0.58%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   111,155 ( 0.52%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj25EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    56,922 ( 0.27%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    56,739 ( 0.27%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-avx2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    49,675 ( 0.23%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj24EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    34,504 ( 0.16%)  ???:std::vector<unsigned char, std::allocator<unsigned char> >::resize(unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    32,342 ( 0.15%)  ???:nano_lance::(anonymous namespace)::decode_dict_rle_column(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, nano_lance::(anonymous namespace)::ColumnEncodingPlan const&, std::vector<unsigned char, std::allocator<unsigned char> > const*) [/home/runner/work/nanolance/nanolance/build/nlbench]
    29,295 ( 0.14%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj23EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    28,543 ( 0.13%)  ???:nano_lance::miniblock::split_payload(std::span<unsigned char const, 18446744073709551615ul>, nano_lance::miniblock::ChunkShape const&, std::vector<nano_lance::miniblock::ChunkView, std::allocator<nano_lance::miniblock::ChunkView> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    27,806 ( 0.13%)  ./elf/./elf/dl-tunables.c:__GI___tunables_init [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    24,202 ( 0.11%)  ./elf/./elf/dl-version.c:_dl_check_map_versions [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    23,914 ( 0.11%)  ???:nano_lance::miniblock::read_chunk_header(std::span<unsigned char const, 18446744073709551615ul>, unsigned long, nano_lance::miniblock::ChunkShape const&, nano_lance::miniblock::ChunkHeader&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    22,514 ( 0.11%)  ./elf/./elf/dl-runtime.c:_dl_fixup [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    19,552 ( 0.09%)  ./malloc/./malloc/malloc.c:free [/usr/lib/x86_64-linux-gnu/libc.so.6]
    19,033 ( 0.09%)  ./malloc/./malloc/malloc.c:_int_free_merge_chunk [/usr/lib/x86_64-linux-gnu/libc.so.6]
    15,937 ( 0.08%)  ???:nano_lance::bitpack::decode_block(std::vector<unsigned char, std::allocator<unsigned char> > const&, unsigned long, unsigned long, std::vector<unsigned char, std::allocator<unsigned char> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    15,446 ( 0.07%)  ./malloc/./malloc/malloc.c:malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    14,546 ( 0.07%)  ???:nano_lance::child_block::decode_rle_runs(nano_lance::page_layout::Compressive const&, nano_lance::page_layout::Compressive const&, std::vector<unsigned char, std::allocator<unsigned char> > const&, std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::page_layout::BufferScheme, unsigned int, unsigned int, std::vector<unsigned char, std::allocator<unsigned char> >&, std::vector<unsigned long, std::allocator<unsigned long> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    12,259 ( 0.06%)  ./malloc/./malloc/malloc.c:__libc_malloc2 [/usr/lib/x86_64-linux-gnu/libc.so.6]
    12,256 ( 0.06%)  ???:std::locale::_Impl::_M_install_facet(std::locale::id const*, std::locale::facet const*) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.35]
    11,421 ( 0.05%)  ???:nano_lance::(anonymous namespace)::decode_fixed_width_column(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, nano_lance::(anonymous namespace)::ColumnEncodingPlan const&, unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    11,172 ( 0.05%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_range_insert<unsigned char const*>(__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, unsigned char const*, unsigned char const*, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
```
