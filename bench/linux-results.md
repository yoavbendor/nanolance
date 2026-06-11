# Linux CI results

_2026-06-11 15:20:09 UTC · commit `9c37f82`_

## Environment
```
Linux runnervm3jyl0 6.17.0-1015-azure #15~24.04.1-Ubuntu SMP Wed May  6 22:37:49 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 18.1.3 (1ubuntu1)
cmake version 3.31.6
```

## Tests
```
smoke    =   0.20 sec*proc (25 tests)

Total Test time (real) =   0.25 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         24.42       24.42    4.189      6.82                (1.00x vs pq)
rust lance             11.55       11.55    3.472      5.60                (0.83x vs pq)
nanolance              26.20       34.60    3.643     12.67         9.31   (0.87x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         26.92       26.92    5.212      4.03                (1.00x vs pq)
rust lance              3.01        3.01    8.485      3.36                (1.63x vs pq)
nanolance              12.04       16.40    9.013     14.05        12.00   (1.73x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         32.05       32.05   15.513      9.03                (1.00x vs pq)
rust lance             18.53       18.53   18.872      6.31                (1.22x vs pq)
nanolance             151.86      157.95   18.206     15.47        12.72   (1.17x vs pq)

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
Timerange: Basic block 0 - 20189238
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 4305, part 1)
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
39,425,852 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
13,127,819 (33.30%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 9,592,040 (24.33%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 7,146,944 (18.13%)  ???:void nano_lance::fastlanes::unpack_1024<unsigned long>(unsigned int, unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
 3,652,082 ( 9.26%)  ???:auto nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&)::$_0::operator()<std::vector<unsigned int, std::allocator<unsigned int> > >(std::vector<unsigned int, std::allocator<unsigned int> >&) const [/home/runner/work/nanolance/nanolance/build/nlbench]
 1,437,324 ( 3.65%)  ???:nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   523,059 ( 1.33%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   506,812 ( 1.29%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   343,251 ( 0.87%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
   190,786 ( 0.48%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   183,129 ( 0.46%)  ./malloc/./malloc/malloc.c:_int_free [/usr/lib/x86_64-linux-gnu/libc.so.6]
   148,282 ( 0.38%)  ./malloc/./malloc/malloc.c:malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
   144,992 ( 0.37%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   116,149 ( 0.29%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   111,741 ( 0.28%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object
   110,405 ( 0.28%)  ./elf/./elf/do-rel.h:_dl_relocate_object
   100,447 ( 0.25%)  ???:nano_lance::pb::decode_column_metadata(std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::pb::ColumnMetadata&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    96,079 ( 0.24%)  ./malloc/./malloc/malloc.c:free [/usr/lib/x86_64-linux-gnu/libc.so.6]
    74,870 ( 0.19%)  ./malloc/./malloc/malloc.c:_int_free_merge_chunk [/usr/lib/x86_64-linux-gnu/libc.so.6]
    71,545 ( 0.18%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-sse2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    68,268 ( 0.17%)  ./elf/./elf/dl-tunables.c:__GI___tunables_init [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    64,284 ( 0.16%)  ./malloc/./malloc/malloc.c:unlink_chunk.isra.0 [/usr/lib/x86_64-linux-gnu/libc.so.6]
    59,004 ( 0.15%)  ???:nano_lance::pb::(anonymous namespace)::read_varint(std::vector<unsigned char, std::allocator<unsigned char> > const&, unsigned long&, unsigned long&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    56,469 ( 0.14%)  ???:std::vector<unsigned char, std::allocator<unsigned char> >::resize(unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    47,712 ( 0.12%)  ???:operator new(unsigned long) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    35,684 ( 0.09%)  ???:std::locale::locale() [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    35,200 ( 0.09%)  ./libio/./libio/genops.c:_IO_link_in [/usr/lib/x86_64-linux-gnu/libc.so.6]
    33,982 ( 0.09%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_assign_aux<__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
    31,200 ( 0.08%)  ./libio/./libio/genops.c:__GI__IO_un_link.part.0 [/usr/lib/x86_64-linux-gnu/libc.so.6]
    30,400 ( 0.08%)  ./libio/./libio/fileops.c:_IO_file_fopen@@GLIBC_2.2.5 [/usr/lib/x86_64-linux-gnu/libc.so.6]
    29,054 ( 0.07%)  ???:nano_lance::read_lance_data_file_bytes(std::filesystem::__cxx11::path const&, unsigned long, unsigned long, std::vector<unsigned char, std::allocator<unsigned char> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    28,114 ( 0.07%)  ???:std::basic_streambuf<char, std::char_traits<char> >::xsgetn(char*, long) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    27,268 ( 0.07%)  ???:std::basic_filebuf<char, std::char_traits<char> >::underflow() [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    25,728 ( 0.07%)  ???:std::basic_ios<char, std::char_traits<char> >::_M_cache_locale(std::locale const&) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    24,480 ( 0.06%)  ???:std::ios_base::ios_base() [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    24,315 ( 0.06%)  ./malloc/./malloc/malloc.c:_int_free_maybe_consolidate [/usr/lib/x86_64-linux-gnu/libc.so.6]
```
