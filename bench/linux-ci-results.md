# Linux CI results (auto-generated — do not edit)

_2026-10-25 17:09:22 UTC · commit `9e0a7f8`_

## Environment
```
Linux runnervmtai0g 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 19.1.1 (1ubuntu1~24.04.2)
cmake version 3.31.6
```

## Tests
```
smoke                =   7.43 sec*proc (118 tests)

Total Test time (real) =   7.46 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         16.91       16.91    4.189      4.90                (1.00x vs pq)
rust lance              9.29        9.29    3.472      3.69                (0.83x vs pq)
nanolance (cli)         4.79        7.75    3.465      1.10         3.94   (0.83x vs pq)
nanolance (py)          2.74        2.74    3.465      1.12                (0.83x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         17.65       17.65    5.212      2.90                (1.00x vs pq)
rust lance              3.45        3.45    8.485      1.62                (1.63x vs pq)
nanolance (cli)         3.56        5.21    8.480      0.44         1.71   (1.63x vs pq)
nanolance (py)          5.86        5.86    8.480      0.50                (1.63x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         23.25       23.25   15.513      6.36                (1.00x vs pq)
rust lance             14.94       14.94   18.916      3.14                (1.22x vs pq)
nanolance (cli)        26.92       29.42   17.987      4.23         5.92   (1.16x vs pq)
nanolance (py)         23.04       23.04   17.987      4.52                (1.16x vs pq)

========== float_smooth  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         20.15       20.15   14.600      2.24                (1.00x vs pq)
rust lance              6.33        6.33    8.686      2.99                (0.59x vs pq)
nanolance (cli)         5.54        7.06    8.609      0.82         2.41   (0.59x vs pq)
nanolance (py)          4.72        4.72    8.609      0.50                (0.59x vs pq)

========== bool_flags  (200000 rows, 1 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)          1.58        1.58    0.128      1.14                (1.00x vs pq)
rust lance              1.20        1.20    0.129      0.87                (1.01x vs pq)
nanolance (cli)         0.51        1.72    0.127      0.07         0.84   (1.00x vs pq)
nanolance (py)          0.31        0.31    0.127      0.10                (1.00x vs pq)

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
Timerange: Basic block 0 - 31656642
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 11465, part 1)
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
43,902,769 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
25,408,445 (57.87%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 7,864,202 (17.91%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 3,860,916 ( 8.79%)  ???:bool nano_lance::(anonymous namespace)::decode_column_impl(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, std::shared_ptr<nano_lance::(anonymous namespace)::ItemView> const&)::$_0::operator()<unsigned int>(unsigned int) const [/home/runner/work/nanolance/nanolance/build/nlbench]
 1,600,808 ( 3.65%)  ???:nano_lance::(anonymous namespace)::slice_leaf(nano_lance::ColumnValues&, unsigned long, unsigned long, unsigned long, unsigned long, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   934,120 ( 2.13%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj28EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   544,711 ( 1.24%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   527,518 ( 1.20%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   459,580 ( 1.05%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj27EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   237,270 ( 0.54%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj29EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   226,050 ( 0.51%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj26EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   199,797 ( 0.46%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   164,480 ( 0.37%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object
   149,163 ( 0.34%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   144,889 ( 0.33%)  ./elf/./elf/do-rel.h:_dl_relocate_object
   122,198 ( 0.28%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   111,155 ( 0.25%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj25EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    83,800 ( 0.19%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-sse2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    74,491 ( 0.17%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    69,508 ( 0.16%)  ./elf/./elf/dl-tunables.c:__GI___tunables_init [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    64,176 ( 0.15%)  ???:std::vector<unsigned char, std::allocator<unsigned char> >::resize(unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    64,144 ( 0.15%)  ???:nano_lance::(anonymous namespace)::decode_column_impl(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, std::shared_ptr<nano_lance::(anonymous namespace)::ItemView> const&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    49,675 ( 0.11%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj24EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    45,086 ( 0.10%)  ./malloc/./malloc/malloc.c:_int_free [/usr/lib/x86_64-linux-gnu/libc.so.6]
    43,392 ( 0.10%)  ./malloc/./malloc/malloc.c:malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]

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
```
