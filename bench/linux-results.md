# Linux CI results

_2026-06-09 09:45:31 UTC · commit `e3dd239`_

## Environment
```
Linux runnervm3jyl0 6.17.0-1015-azure #15~24.04.1-Ubuntu SMP Wed May  6 22:37:49 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 18.1.3 (1ubuntu1)
cmake version 3.31.6
```

## Tests
```
smoke    =   0.14 sec*proc (17 tests)

Total Test time (real) =   0.19 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine            write ms   file bytes    B/row   read ms  read(lance)
parquet (zstd)        23.7      837,834    4.189      6.38                (1.00x vs pq)
rust lance            14.4      694,409    3.472      5.89                (0.83x vs pq)
nanolance             62.8      728,616    3.643     14.59         9.28   (0.87x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine            write ms   file bytes    B/row   read ms  read(lance)
parquet (zstd)        27.8    1,042,350    5.212      3.54                (1.00x vs pq)
rust lance             4.2    1,696,945    8.485      3.59                (1.63x vs pq)
nanolance             16.7    1,802,616    9.013     13.49        11.50   (1.73x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine            write ms   file bytes    B/row   read ms  read(lance)
parquet (zstd)        43.7    3,102,688   15.513      8.36                (1.00x vs pq)
rust lance            20.3    3,779,882   18.899      5.06                (1.22x vs pq)
nanolance            330.6    3,641,112   18.206     15.92        11.81   (1.17x vs pq)

note: nanolance write ms includes process startup; read ms 'read ms' col is each engine's native reader, 'read(lance)' is rust-lance reading the nanolance file (interop). best of 7.
```

## Native-read profile (callgrind, nlbench on pcap_ref)
```
--------------------------------------------------------------------------------
Profile data file '/tmp/cg.out' (creator: callgrind-3.22.0)
--------------------------------------------------------------------------------
I1 cache: 
D1 cache: 
LL cache: 
Timerange: Basic block 0 - 14474632
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 3401, part 1)
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
60,542,317 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
11,792,035 (19.48%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
10,417,052 (17.21%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_range_insert<__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
10,410,607 (17.20%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_range_insert<unsigned char const*>(__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, unsigned char const*, unsigned char const*, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
10,400,000 (17.18%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_range_insert<__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
 7,146,944 (11.80%)  ???:void nano_lance::fastlanes::unpack_1024<unsigned long>(unsigned int, unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
 5,253,115 ( 8.68%)  ???:nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   714,983 ( 1.18%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
   520,371 ( 0.86%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   506,812 ( 0.84%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   342,668 ( 0.57%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
   190,786 ( 0.32%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   183,000 ( 0.30%)  ./malloc/./malloc/malloc.c:_int_free [/usr/lib/x86_64-linux-gnu/libc.so.6]
   148,240 ( 0.24%)  ./malloc/./malloc/malloc.c:malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
   144,992 ( 0.24%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   116,149 ( 0.19%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   111,741 ( 0.18%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object
   110,405 ( 0.18%)  ./elf/./elf/do-rel.h:_dl_relocate_object
   100,447 ( 0.17%)  ???:nano_lance::pb::decode_column_metadata(std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::pb::ColumnMetadata&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    96,067 ( 0.16%)  ./malloc/./malloc/malloc.c:free [/usr/lib/x86_64-linux-gnu/libc.so.6]
    74,633 ( 0.12%)  ./malloc/./malloc/malloc.c:_int_free_merge_chunk [/usr/lib/x86_64-linux-gnu/libc.so.6]
    71,522 ( 0.12%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-sse2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    68,268 ( 0.11%)  ./elf/./elf/dl-tunables.c:__GI___tunables_init [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    64,185 ( 0.11%)  ./malloc/./malloc/malloc.c:unlink_chunk.isra.0 [/usr/lib/x86_64-linux-gnu/libc.so.6]
    59,004 ( 0.10%)  ???:nano_lance::pb::(anonymous namespace)::read_varint(std::vector<unsigned char, std::allocator<unsigned char> > const&, unsigned long&, unsigned long&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    47,696 ( 0.08%)  ???:operator new(unsigned long) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    35,684 ( 0.06%)  ???:std::locale::locale() [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    35,200 ( 0.06%)  ./libio/./libio/genops.c:_IO_link_in [/usr/lib/x86_64-linux-gnu/libc.so.6]
    33,982 ( 0.06%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_assign_aux<__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
    31,200 ( 0.05%)  ./libio/./libio/genops.c:__GI__IO_un_link.part.0 [/usr/lib/x86_64-linux-gnu/libc.so.6]
    30,400 ( 0.05%)  ./libio/./libio/fileops.c:_IO_file_fopen@@GLIBC_2.2.5 [/usr/lib/x86_64-linux-gnu/libc.so.6]
    29,054 ( 0.05%)  ???:nano_lance::read_lance_data_file_bytes(std::filesystem::__cxx11::path const&, unsigned long, unsigned long, std::vector<unsigned char, std::allocator<unsigned char> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    28,114 ( 0.05%)  ???:std::basic_streambuf<char, std::char_traits<char> >::xsgetn(char*, long) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    27,268 ( 0.05%)  ???:std::basic_filebuf<char, std::char_traits<char> >::underflow() [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]

--------------------------------------------------------------------------------
```
