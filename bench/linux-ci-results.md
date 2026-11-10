# Linux CI results (auto-generated — do not edit)

_2026-11-10 18:24:56 UTC · commit `07ec05b`_

## Environment
```
Linux runnervmwuk1x 7.0.0-1019-azure #20-Ubuntu SMP PREEMPT Mon Oct 19 22:41:16 UTC 2026 x86_64 GNU/Linux
Ubuntu clang version 19.1.7 (20ubuntu4)
cmake version 4.4.5
```

## Tests
```
smoke                =   8.33 sec*proc (121 tests)

Total Test time (real) =   8.45 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         21.59       21.59    4.189      5.26                (1.00x vs pq)
rust lance             10.07       10.07    3.472      5.83                (0.83x vs pq)
nanolance (cli)         8.39       15.87    3.465      0.95         5.27   (0.83x vs pq)
nanolance (py)          3.33        3.33    3.465      0.71                (0.83x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         22.98       22.98    5.212      3.34                (1.00x vs pq)
rust lance              4.52        4.52    8.485      2.63                (1.63x vs pq)
nanolance (cli)         4.02        6.40    8.488      0.54         2.44   (1.63x vs pq)
nanolance (py)          2.95        2.95    8.488      0.65                (1.63x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         27.99       27.99   15.513      7.46                (1.00x vs pq)
rust lance             18.92       18.92   18.876      4.99                (1.22x vs pq)
nanolance (cli)        21.79       26.26   17.989      5.51         8.02   (1.16x vs pq)
nanolance (py)         24.92       24.92   17.989      5.37                (1.16x vs pq)

========== float_smooth  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         26.03       26.03   14.600      3.16                (1.00x vs pq)
rust lance              6.28        6.28    8.686      4.37                (0.59x vs pq)
nanolance (cli)         5.37        7.59    8.609      0.87         3.53   (0.59x vs pq)
nanolance (py)          4.42        4.42    8.609      0.72                (0.59x vs pq)

========== bool_flags  (200000 rows, 1 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)          2.05        2.05    0.128      1.71                (1.00x vs pq)
rust lance              1.33        1.33    0.129      1.35                (1.01x vs pq)
nanolance (cli)         0.63        1.85    0.127      0.04         1.27   (1.00x vs pq)
nanolance (py)          0.21        0.21    0.127      0.06                (1.00x vs pq)

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
Timerange: Basic block 0 - 13920593
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 17890, part 1)
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
21,173,092 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
11,463,674 (54.14%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 3,006,607 (14.20%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 1,930,458 ( 9.12%)  ???:bool nano_lance::(anonymous namespace)::decode_dict_rle_column(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, nano_lance::(anonymous namespace)::ColumnEncodingPlan const&)::$_0::operator()<unsigned int>(unsigned int) const [/home/runner/work/nanolance/nanolance/build/nlbench]
   923,505 ( 4.36%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj28EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   529,427 ( 2.50%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   522,914 ( 2.47%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   459,580 ( 2.17%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj27EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   237,270 ( 1.12%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj29EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   226,050 ( 1.07%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj26EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
   199,751 ( 0.94%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   181,263 ( 0.86%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object_no_relro
   146,716 ( 0.69%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object_no_relro [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   128,327 ( 0.61%)  ./elf/./elf/do-rel.h:_dl_relocate_object_no_relro
   122,284 ( 0.58%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   111,155 ( 0.52%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj25EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    62,082 ( 0.29%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    56,275 ( 0.27%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-avx2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    49,675 ( 0.23%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj24EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    34,708 ( 0.16%)  ???:nano_lance::(anonymous namespace)::decode_dict_rle_column(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, nano_lance::(anonymous namespace)::ColumnEncodingPlan const&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    34,576 ( 0.16%)  ???:std::vector<unsigned char, std::allocator<unsigned char> >::resize(unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    29,295 ( 0.14%)  ???:_ZN5nanom8columnar9fastlanes6detail8unpack_wITkNS1_4wordEmLj23EEEvPKT_PS4_ [/home/runner/work/nanolance/nanolance/build/nlbench]
    28,526 ( 0.13%)  ???:nano_lance::miniblock::split_payload(std::span<unsigned char const, 18446744073709551615ul>, nano_lance::miniblock::ChunkShape const&, std::vector<nano_lance::miniblock::ChunkView, std::allocator<nano_lance::miniblock::ChunkView> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    27,806 ( 0.13%)  ./elf/./elf/dl-tunables.c:__GI___tunables_init [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    24,202 ( 0.11%)  ./elf/./elf/dl-version.c:_dl_check_map_versions [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    23,914 ( 0.11%)  ???:nano_lance::miniblock::read_chunk_header(std::span<unsigned char const, 18446744073709551615ul>, unsigned long, nano_lance::miniblock::ChunkShape const&, nano_lance::miniblock::ChunkHeader&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    22,408 ( 0.11%)  ./elf/./elf/dl-runtime.c:_dl_fixup [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    17,857 ( 0.08%)  ./malloc/./malloc/malloc.c:free [/usr/lib/x86_64-linux-gnu/libc.so.6]
    15,937 ( 0.08%)  ???:nano_lance::bitpack::decode_block(std::vector<unsigned char, std::allocator<unsigned char> > const&, unsigned long, unsigned long, std::vector<unsigned char, std::allocator<unsigned char> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    14,828 ( 0.07%)  ./malloc/./malloc/malloc.c:malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    14,694 ( 0.07%)  ./malloc/./malloc/malloc.c:_int_free_merge_chunk [/usr/lib/x86_64-linux-gnu/libc.so.6]
    12,762 ( 0.06%)  ./malloc/./malloc/malloc.c:__libc_malloc2 [/usr/lib/x86_64-linux-gnu/libc.so.6]
    12,256 ( 0.06%)  ???:std::locale::_Impl::_M_install_facet(std::locale::id const*, std::locale::facet const*) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.35]
    11,415 ( 0.05%)  ???:nano_lance::(anonymous namespace)::decode_fixed_width_column(nano_lance::EncodingsIo const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, nano_lance::(anonymous namespace)::ColumnEncodingPlan const&, unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    11,172 ( 0.05%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_range_insert<unsigned char const*>(__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, unsigned char const*, unsigned char const*, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
    11,159 ( 0.05%)  ???:nano_lance::page_layout::decode_page_layout(std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::page_layout::PageLayout&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
```
