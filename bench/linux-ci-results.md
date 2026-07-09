# Linux CI results (auto-generated — do not edit)

_2026-07-09 11:20:13 UTC · commit `5287560`_

## Environment
```
Linux runnervm5mmn9 6.17.0-1018-azure #18~24.04.1-Ubuntu SMP Thu May 28 16:39:11 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 19.1.1 (1ubuntu1~24.04.2)
cmake version 3.31.6
```

## Tests
```
smoke    =   2.03 sec*proc (26 tests)

Total Test time (real) =   2.23 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         23.29       23.29    4.189      6.56                (1.00x vs pq)
rust lance             10.61       10.61    3.472      5.69                (0.83x vs pq)
nanolance              15.89       23.99    3.643     12.25         9.66   (0.87x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         26.22       26.22    5.212      3.62                (1.00x vs pq)
rust lance              3.20        3.20    8.485      4.05                (1.63x vs pq)
nanolance              10.86       14.87    9.013     13.15        13.54   (1.73x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         30.69       30.69   15.513      8.36                (1.00x vs pq)
rust lance             18.42       18.42   18.925      6.54                (1.22x vs pq)
nanolance              46.07       51.59   18.206     15.70        13.95   (1.17x vs pq)

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
Timerange: Basic block 0 - 20206777
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 3988, part 1)
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
39,547,165 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
13,127,809 (33.20%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 9,592,105 (24.25%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 7,146,944 (18.07%)  ???:void nano_lance::fastlanes::unpack_1024<unsigned long>(unsigned int, unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
 3,700,683 ( 9.36%)  ???:bool nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&)::$_0::operator()<std::vector<unsigned int, std::allocator<unsigned int> > >(std::vector<unsigned int, std::allocator<unsigned int> >&) const [/home/runner/work/nanolance/nanolance/build/nlbench]
 1,429,408 ( 3.61%)  ???:nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   530,258 ( 1.34%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   509,746 ( 1.29%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   355,140 ( 0.90%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
   191,853 ( 0.49%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   183,298 ( 0.46%)  ./malloc/./malloc/malloc.c:_int_free [/usr/lib/x86_64-linux-gnu/libc.so.6]
   148,603 ( 0.38%)  ./malloc/./malloc/malloc.c:malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
   144,992 ( 0.37%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   116,839 ( 0.30%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   111,789 ( 0.28%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object
   110,447 ( 0.28%)  ./elf/./elf/do-rel.h:_dl_relocate_object
   102,435 ( 0.26%)  ???:nano_lance::pb::decode_column_metadata(std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::pb::ColumnMetadata&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    96,275 ( 0.24%)  ./malloc/./malloc/malloc.c:free [/usr/lib/x86_64-linux-gnu/libc.so.6]
    74,776 ( 0.19%)  ./malloc/./malloc/malloc.c:_int_free_merge_chunk [/usr/lib/x86_64-linux-gnu/libc.so.6]
    73,682 ( 0.19%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-sse2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    68,289 ( 0.17%)  ./elf/./elf/dl-tunables.c:__GI___tunables_init [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    67,497 ( 0.17%)  ./malloc/./malloc/malloc.c:unlink_chunk.isra.0 [/usr/lib/x86_64-linux-gnu/libc.so.6]
    58,824 ( 0.15%)  ???:nano_lance::pb::(anonymous namespace)::read_varint(std::vector<unsigned char, std::allocator<unsigned char> > const&, unsigned long&, unsigned long&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    58,071 ( 0.15%)  ???:std::vector<unsigned char, std::allocator<unsigned char> >::resize(unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    47,856 ( 0.12%)  ???:operator new(unsigned long) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    37,118 ( 0.09%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_assign_aux<__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
    35,684 ( 0.09%)  ???:std::locale::locale() [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    35,200 ( 0.09%)  ./libio/./libio/genops.c:_IO_link_in [/usr/lib/x86_64-linux-gnu/libc.so.6]
    31,200 ( 0.08%)  ./libio/./libio/genops.c:__GI__IO_un_link.part.0 [/usr/lib/x86_64-linux-gnu/libc.so.6]
    30,400 ( 0.08%)  ./libio/./libio/fileops.c:_IO_file_fopen@@GLIBC_2.2.5 [/usr/lib/x86_64-linux-gnu/libc.so.6]
    29,850 ( 0.08%)  ???:nano_lance::read_lance_data_file_bytes(std::filesystem::__cxx11::path const&, unsigned long, unsigned long, std::vector<unsigned char, std::allocator<unsigned char> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    28,114 ( 0.07%)  ???:std::basic_streambuf<char, std::char_traits<char> >::xsgetn(char*, long) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    27,268 ( 0.07%)  ???:std::basic_filebuf<char, std::char_traits<char> >::underflow() [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    25,728 ( 0.07%)  ???:std::basic_ios<char, std::char_traits<char> >::_M_cache_locale(std::locale const&) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    24,810 ( 0.06%)  ./malloc/./malloc/malloc.c:malloc_consolidate [/usr/lib/x86_64-linux-gnu/libc.so.6]
    24,480 ( 0.06%)  ???:std::ios_base::ios_base() [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
```
