# Linux CI results (auto-generated — do not edit)

_2026-09-25 05:39:44 UTC · commit `31cb823`_

## Environment
```
Linux runnervmtr4k5 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 19.1.1 (1ubuntu1~24.04.2)
cmake version 3.31.6
```

## Tests
```
smoke    =   5.53 sec*proc (42 tests)

Total Test time (real) =   5.65 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         19.22       19.22    4.189      4.76                (1.00x vs pq)
rust lance              9.00        9.00    3.472      4.41                (0.83x vs pq)
nanolance               5.31       12.45    3.643      1.38         9.01   (0.87x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         18.97       18.97    5.212      2.97                (1.00x vs pq)
rust lance              4.95        4.95    8.485      2.70                (1.63x vs pq)
nanolance               3.87        6.37    9.013      1.56        12.12   (1.73x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         24.58       24.58   15.513      6.50                (1.00x vs pq)
rust lance             15.54       15.54   18.840      4.74                (1.21x vs pq)
nanolance              25.58       30.07   18.015      7.05         8.40   (1.16x vs pq)

========== float_smooth  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         22.89       22.89   14.600      2.87                (1.00x vs pq)
rust lance              6.79        6.79    8.686      4.53                (0.59x vs pq)
nanolance               4.30        6.61    8.399      1.16         5.35   (0.58x vs pq)

========== bool_flags  (200000 rows, 1 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)          1.61        1.61    0.128      1.41                (1.00x vs pq)
rust lance              1.17        1.17    0.129      1.27                (1.01x vs pq)
nanolance               0.49        1.51    0.127      0.80         1.55   (0.99x vs pq)

note: best of 5 writes / 7 reads. write(core)=in-process encode work (parquet/lance: the write call; nanolance: ingest+encode+commit, EXCLUDING process startup + Arrow-IPC parse). write(proc)=full wall clock (nanolance includes subprocess startup + IPC parse). read ms=native reader; read(lance)=rust-lance reading the nanolance file.
```

## Native-read profile (callgrind, nlbench on pcap_ref)
```
--------------------------------------------------------------------------------
Profile data file '/tmp/cg.out' (creator: callgrind-3.22.0)
--------------------------------------------------------------------------------
I1 cache: 
D1 cache: 
LL cache: 
Timerange: Basic block 0 - 16265300
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 4059, part 1)
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
24,979,622 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
12,440,150 (49.80%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 3,888,821 (15.57%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 2,422,485 ( 9.70%)  ???:bool nano_lance::(anonymous namespace)::decode_column_impl(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, std::shared_ptr<nano_lance::(anonymous namespace)::ItemView> const&)::$_0::operator()<std::vector<unsigned int, std::allocator<unsigned int> > >(std::vector<unsigned int, std::allocator<unsigned int> >&) const [/home/runner/work/nanolance/nanolance/build/nlbench]
   923,505 ( 3.70%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 28u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
   531,275 ( 2.13%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   514,688 ( 2.06%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   459,580 ( 1.84%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 27u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
   237,270 ( 0.95%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 29u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
   226,050 ( 0.90%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 26u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
   198,437 ( 0.79%)  ./malloc/./malloc/malloc.c:_int_free [/usr/lib/x86_64-linux-gnu/libc.so.6]
   193,890 ( 0.78%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   159,815 ( 0.64%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
   151,897 ( 0.61%)  ./malloc/./malloc/malloc.c:malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
   145,299 ( 0.58%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   132,707 ( 0.53%)  ???:nano_lance::pb::decode_column_metadata(std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::pb::ColumnMetadata&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   124,954 ( 0.50%)  ???:nano_lance::page_layout::decode_page_layout(std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::page_layout::PageLayout&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   121,093 ( 0.48%)  ???:nano_lance::(anonymous namespace)::append_repeated_value(std::vector<unsigned char, std::allocator<unsigned char> >&, unsigned char const*, unsigned long, unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
   118,180 ( 0.47%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   113,255 ( 0.45%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object
   111,468 ( 0.45%)  ./elf/./elf/do-rel.h:_dl_relocate_object
   111,155 ( 0.44%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 25u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
    97,286 ( 0.39%)  ./malloc/./malloc/malloc.c:free [/usr/lib/x86_64-linux-gnu/libc.so.6]
    76,040 ( 0.30%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-sse2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    71,064 ( 0.28%)  ???:nano_lance::pb::(anonymous namespace)::read_varint(std::vector<unsigned char, std::allocator<unsigned char> > const&, unsigned long&, unsigned long&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    69,508 ( 0.28%)  ./elf/./elf/dl-tunables.c:__GI___tunables_init [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    63,111 ( 0.25%)  ???:nano_lance::page_layout::(anonymous namespace)::read_varint(nano_lance::page_layout::(anonymous namespace)::Cursor&, unsigned long&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    55,135 ( 0.22%)  ???:nano_lance::(anonymous namespace)::split_miniblock_payload(std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::(anonymous namespace)::MiniBlockChunkShape const&, std::vector<nano_lance::(anonymous namespace)::MiniBlockChunkView, std::allocator<nano_lance::(anonymous namespace)::MiniBlockChunkView> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    54,816 ( 0.22%)  ???:operator new(unsigned long) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    54,542 ( 0.22%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_assign_aux<__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
    49,675 ( 0.20%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 24u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
    43,895 ( 0.18%)  ???:nano_lance::(anonymous namespace)::decode_column_impl(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&, std::shared_ptr<nano_lance::(anonymous namespace)::ItemView> const&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    42,678 ( 0.17%)  ???:nano_lance::read_lance_data_file_bytes(std::filesystem::__cxx11::path const&, unsigned long, unsigned long, std::vector<unsigned char, std::allocator<unsigned char> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    41,498 ( 0.17%)  ???:std::vector<unsigned char, std::allocator<unsigned char> >::resize(unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    36,518 ( 0.15%)  ./malloc/./malloc/malloc.c:malloc_consolidate [/usr/lib/x86_64-linux-gnu/libc.so.6]
    29,295 ( 0.12%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 23u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
```
