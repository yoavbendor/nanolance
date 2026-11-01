# Linux CI results (auto-generated — do not edit)

_2026-11-01 10:06:43 UTC · commit `bef5eee`_

## Environment
```
Linux runnervm48cm3 7.0.0-1017-azure #17~24.04.1-Ubuntu SMP PREEMPT Thu Sep 24 18:46:57 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 19.1.1 (1ubuntu1~24.04.2)
cmake version 3.31.6
```

## Tests
```
smoke                =  10.41 sec*proc (121 tests)

Total Test time (real) =  10.50 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         21.30       21.30    4.189      5.86                (1.00x vs pq)
rust lance             12.16       12.16    3.472      4.59                (0.83x vs pq)
nanolance (cli)        10.00       19.79    3.465      1.35         5.21   (0.83x vs pq)
nanolance (py)          2.54        2.54    3.465      1.24                (0.83x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         22.23       22.23    5.212      3.45                (1.00x vs pq)
rust lance              4.39        4.39    8.485      2.32                (1.63x vs pq)
nanolance (cli)         4.27        7.30    8.488      0.58         2.42   (1.63x vs pq)
nanolance (py)          3.10        3.10    8.488      0.65                (1.63x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         28.83       28.83   15.513      7.90                (1.00x vs pq)
rust lance             17.69       17.69   18.905      4.66                (1.22x vs pq)
nanolance (cli)        30.69       35.99   17.989      5.50         7.25   (1.16x vs pq)
nanolance (py)         24.95       24.95   17.989      5.65                (1.16x vs pq)

========== float_smooth  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         24.74       24.74   14.600      2.92                (1.00x vs pq)
rust lance              6.90        6.90    8.686      4.09                (0.59x vs pq)
nanolance (cli)         6.49        9.34    8.609      1.13         3.40   (0.59x vs pq)
nanolance (py)          4.75        4.75    8.609      0.70                (0.59x vs pq)

========== bool_flags  (200000 rows, 1 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)          1.84        1.84    0.128      1.35                (1.00x vs pq)
rust lance              1.56        1.56    0.129      1.25                (1.01x vs pq)
nanolance (cli)         0.82        2.34    0.127      0.09         1.13   (1.00x vs pq)
nanolance (py)          0.41        0.41    0.127      0.14                (1.00x vs pq)

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
Timerange: Basic block 0 - 31641420
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 18123, part 1)
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
43,863,908 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
25,396,679 (57.90%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 7,863,252 (17.93%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 3,860,916 ( 8.80%)  ???:bool nano_lance::(anonymous namespace)::decode_dict_rle_column(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, nano_lance::(anonymous namespace)::ColumnEncodingPlan const&)::$_0::operator()<unsigned int>(unsigned int) const [/home/runner/work/nanolance/nanolance/build/nlbench]
 1,600,952 ( 3.65%)  ???:nano_lance::(anonymous namespace)::slice_leaf(nano_lance::ColumnValues&, unsigned long, unsigned long, unsigned long, unsigned long, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   934,120 ( 2.13%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj28EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   545,080 ( 1.24%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   527,628 ( 1.20%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   459,580 ( 1.05%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj27EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   237,270 ( 0.54%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj29EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   226,050 ( 0.52%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj26EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   199,894 ( 0.46%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   164,624 ( 0.38%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object
   149,163 ( 0.34%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   144,979 ( 0.33%)  ./elf/./elf/do-rel.h:_dl_relocate_object
   122,263 ( 0.28%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   111,155 ( 0.25%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj25EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    82,913 ( 0.19%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-sse2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    74,421 ( 0.17%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    69,508 ( 0.16%)  ./elf/./elf/dl-tunables.c:__GI___tunables_init [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    69,412 ( 0.16%)  ???:nano_lance::(anonymous namespace)::decode_dict_rle_column(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, nano_lance::(anonymous namespace)::ColumnEncodingPlan const&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    64,080 ( 0.15%)  ???:std::vector<unsigned char, std::allocator<unsigned char> >::resize(unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    49,675 ( 0.11%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj24EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    47,010 ( 0.11%)  ./malloc/./malloc/malloc.c:_int_free [/usr/lib/x86_64-linux-gnu/libc.so.6]

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
