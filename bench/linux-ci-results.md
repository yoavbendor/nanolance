# Linux CI results (auto-generated — do not edit)

_2026-10-04 09:27:22 UTC · commit `181a399`_

## Environment
```
Linux runnervm8df0l 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 19.1.1 (1ubuntu1~24.04.2)
cmake version 3.31.6
```

## Tests
```
smoke    =   6.60 sec*proc (44 tests)

Total Test time (real) =   6.67 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         13.18       13.18    4.189      3.80                (1.00x vs pq)
rust lance              6.83        6.83    3.472      2.88                (0.83x vs pq)
nanolance (cli)         4.09        7.14    3.465      0.65         3.46   (0.83x vs pq)
nanolance (py)          1.79        1.79    3.465      0.89                (0.83x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         14.28       14.28    5.212      2.14                (1.00x vs pq)
rust lance              3.56        3.56    8.485      1.44                (1.63x vs pq)
nanolance (cli)         3.11        4.79    8.480      0.29         1.38   (1.63x vs pq)
nanolance (py)          2.17        2.17    8.480      0.32                (1.63x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         16.75       16.75   15.513      5.00                (1.00x vs pq)
rust lance             11.94       11.94   18.951      2.73                (1.22x vs pq)
nanolance (cli)        11.03       13.69   18.437      1.84         2.46   (1.19x vs pq)
nanolance (py)         10.08       10.08   18.437      2.22                (1.19x vs pq)

========== float_smooth  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         16.70       16.70   14.600      1.76                (1.00x vs pq)
rust lance              3.99        3.99    8.686      2.49                (0.59x vs pq)
nanolance (cli)         4.24        5.71    8.609      0.62         1.94   (0.59x vs pq)
nanolance (py)          3.31        3.31    8.609      0.35                (0.59x vs pq)

========== bool_flags  (200000 rows, 1 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)          1.22        1.22    0.128      1.07                (1.00x vs pq)
rust lance              1.02        1.02    0.129      0.80                (1.01x vs pq)
nanolance (cli)         0.37        1.44    0.127      0.05         0.74   (1.00x vs pq)
nanolance (py)          0.20        0.20    0.127      0.06                (1.00x vs pq)

note: best of 5 writes / 7 reads. parquet, rust lance and nanolance (py) write and read in this Python process, from the Arrow table in memory: compare these three. nanolance (cli) runs arrowipc2lance and nlbench as subprocesses: write(core)=ingest+encode+commit in a fresh process (its first-touch page faults included), write(proc)=its whole wall clock (process start + Arrow IPC parse from stdin + write); read ms=nlbench; read(lance)=rust lance reading the file nanolance wrote.
```

## Native-read profile (callgrind, nlbench on pcap_ref)
```
--------------------------------------------------------------------------------
Profile data file '/tmp/cg.out' (creator: callgrind-3.22.0)
--------------------------------------------------------------------------------
I1 cache: 
D1 cache: 
LL cache: 
Timerange: Basic block 0 - 31593050
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 6682, part 1)
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
43,741,331 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
25,372,043 (58.00%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 7,838,124 (17.92%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 3,860,916 ( 8.83%)  ???:bool nano_lance::(anonymous namespace)::decode_column_impl(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, std::shared_ptr<nano_lance::(anonymous namespace)::ItemView> const&)::$_0::operator()<unsigned int>(unsigned int) const [/home/runner/work/nanolance/nanolance/build/nlbench]
 1,600,808 ( 3.66%)  ???:nano_lance::(anonymous namespace)::slice_leaf(nano_lance::ColumnValues&, unsigned long, unsigned long, unsigned long, unsigned long, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   934,120 ( 2.14%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj28EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   546,456 ( 1.25%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   528,726 ( 1.21%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   459,580 ( 1.05%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj27EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   237,270 ( 0.54%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj29EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   226,050 ( 0.52%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj26EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   199,021 ( 0.45%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   146,895 ( 0.34%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   121,636 ( 0.28%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   118,611 ( 0.27%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object
   115,423 ( 0.26%)  ./elf/./elf/do-rel.h:_dl_relocate_object
   111,155 ( 0.25%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj25EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    84,152 ( 0.19%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    81,704 ( 0.19%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-sse2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    69,508 ( 0.16%)  ./elf/./elf/dl-tunables.c:__GI___tunables_init [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    63,922 ( 0.15%)  ???:std::vector<unsigned char, std::allocator<unsigned char> >::resize(unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    55,664 ( 0.13%)  ./malloc/./malloc/malloc.c:_int_free [/usr/lib/x86_64-linux-gnu/libc.so.6]
    55,559 ( 0.13%)  ???:nano_lance::(anonymous namespace)::split_miniblock_payload(std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::(anonymous namespace)::MiniBlockChunkShape const&, std::vector<nano_lance::(anonymous namespace)::MiniBlockChunkView, std::allocator<nano_lance::(anonymous namespace)::MiniBlockChunkView> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    55,043 ( 0.13%)  ???:nano_lance::(anonymous namespace)::decode_column_impl(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, std::shared_ptr<nano_lance::(anonymous namespace)::ItemView> const&) [/home/runner/work/nanolance/nanolance/build/nlbench]

--------------------------------------------------------------------------------
The following files chosen for auto-annotation could not be found:
--------------------------------------------------------------------------------
  ./elf/../sysdeps/generic/dl-new-hash.h
  ./elf/../sysdeps/x86_64/dl-machine.h
  ./elf/./elf/dl-lookup.c
  ./elf/./elf/dl-reloc.c
  ./elf/./elf/dl-tunables.c
  ./elf/./elf/do-rel.h
  ./malloc/./malloc/malloc.c
  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-sse2.S
```
