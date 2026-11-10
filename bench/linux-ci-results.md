# Linux CI results (auto-generated — do not edit)

_2026-11-10 17:24:28 UTC · commit `e9b8d23`_

## Environment
```
Linux runnervmwuk1x 7.0.0-1019-azure #20-Ubuntu SMP PREEMPT Mon Oct 19 22:41:16 UTC 2026 x86_64 GNU/Linux
Ubuntu clang version 19.1.7 (20ubuntu4)
cmake version 4.4.5
```

## Tests
```
smoke                =  10.41 sec*proc (121 tests)

Total Test time (real) =  10.54 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         21.62       21.62    4.189      8.08                (1.00x vs pq)
rust lance             11.83       11.83    3.472     13.12                (0.83x vs pq)
nanolance (cli)        10.00       20.11    3.465      1.90        14.31   (0.83x vs pq)
nanolance (py)          2.69        2.69    3.465      1.90                (0.83x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         21.99       21.99    5.212      4.17                (1.00x vs pq)
rust lance              6.33        6.33    8.485      2.58                (1.63x vs pq)
nanolance (cli)         4.39        7.72    8.488      0.68         2.60   (1.63x vs pq)
nanolance (py)          3.04        3.04    8.488      0.72                (1.63x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         27.78       27.78   15.513      8.42                (1.00x vs pq)
rust lance             19.26       19.26   18.880      6.91                (1.22x vs pq)
nanolance (cli)        25.23       30.69   17.989      5.73        11.43   (1.16x vs pq)
nanolance (py)         25.52       25.52   17.989      5.77                (1.16x vs pq)

========== float_smooth  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         26.04       26.04   14.600      3.64                (1.00x vs pq)
rust lance              9.25        9.25    8.686      6.34                (0.59x vs pq)
nanolance (cli)         6.42        9.40    8.609      1.51         5.06   (0.59x vs pq)
nanolance (py)          4.79        4.79    8.609      0.94                (0.59x vs pq)

========== bool_flags  (200000 rows, 1 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)          1.89        1.89    0.128      1.39                (1.00x vs pq)
rust lance              1.71        1.71    0.129      1.35                (1.01x vs pq)
nanolance (cli)         0.84        2.45    0.127      0.09         1.32   (1.00x vs pq)
nanolance (py)          0.41        0.41    0.127      0.13                (1.00x vs pq)

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
Timerange: Basic block 0 - 28443013
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 17994, part 1)
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
40,689,348 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
22,375,817 (54.99%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 7,864,666 (19.33%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 3,860,916 ( 9.49%)  ???:bool nano_lance::(anonymous namespace)::decode_dict_rle_column(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, nano_lance::(anonymous namespace)::ColumnEncodingPlan const&)::$_0::operator()<unsigned int>(unsigned int) const [/home/runner/work/nanolance/nanolance/build/nlbench]
 1,600,952 ( 3.93%)  ???:nano_lance::(anonymous namespace)::slice_leaf(nano_lance::ColumnValues&, unsigned long, unsigned long, unsigned long, unsigned long, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   934,120 ( 2.30%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj28EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   529,566 ( 1.30%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   523,159 ( 1.29%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   459,580 ( 1.13%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj27EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   237,270 ( 0.58%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj29EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   226,050 ( 0.56%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj26EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   199,848 ( 0.49%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   181,263 ( 0.45%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object_no_relro
   146,716 ( 0.36%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object_no_relro [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   128,327 ( 0.32%)  ./elf/./elf/do-rel.h:_dl_relocate_object_no_relro
   122,350 ( 0.30%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   111,155 ( 0.27%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj25EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    75,227 ( 0.18%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    69,416 ( 0.17%)  ???:nano_lance::(anonymous namespace)::decode_dict_rle_column(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, nano_lance::(anonymous namespace)::ColumnEncodingPlan const&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    68,932 ( 0.17%)  ???:std::vector<unsigned char, std::allocator<unsigned char> >::resize(unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    56,315 ( 0.14%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-avx2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    49,675 ( 0.12%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj24EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    29,295 ( 0.07%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj23EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    29,063 ( 0.07%)  ???:nano_lance::miniblock::split_payload(std::span<unsigned char const, 18446744073709551615ul>, nano_lance::miniblock::ChunkShape const&, std::vector<nano_lance::miniblock::ChunkView, std::allocator<nano_lance::miniblock::ChunkView> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]

--------------------------------------------------------------------------------
The following files chosen for auto-annotation could not be found:
--------------------------------------------------------------------------------
  ./elf/../sysdeps/generic/dl-new-hash.h
  ./elf/../sysdeps/x86_64/dl-machine.h
  ./elf/./elf/dl-lookup.c
  ./elf/./elf/dl-reloc.c
  ./elf/./elf/do-rel.h
  ./malloc/./malloc/malloc.c
  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-avx2.S
  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S
```
