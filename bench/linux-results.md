# Linux CI results

_2026-06-09 07:45:07 UTC · commit `02950ae`_

## Environment
```
Linux runnervm3jyl0 6.17.0-1015-azure #15~24.04.1-Ubuntu SMP Wed May  6 22:37:49 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 18.1.3 (1ubuntu1)
cmake version 3.31.6
```

## Tests
```
smoke    =   0.14 sec*proc (17 tests)

Total Test time (real) =   0.20 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine            write ms   file bytes    B/row   read ms  read(lance)
parquet (zstd)        23.8      837,834    4.189      6.21                (1.00x vs pq)
rust lance            14.4      694,409    3.472      5.88                (0.83x vs pq)
nanolance             62.9      728,616    3.643     16.64         8.66   (0.87x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine            write ms   file bytes    B/row   read ms  read(lance)
parquet (zstd)        27.8    1,042,350    5.212      3.61                (1.00x vs pq)
rust lance             4.4    1,696,945    8.485      3.26                (1.63x vs pq)
nanolance             17.7    1,802,616    9.013     20.61        11.53   (1.73x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine            write ms   file bytes    B/row   read ms  read(lance)
parquet (zstd)        42.9    3,102,688   15.513      8.71                (1.00x vs pq)
rust lance            19.8    3,785,962   18.930      5.57                (1.22x vs pq)
nanolance            355.4    3,641,112   18.206     20.19        11.81   (1.17x vs pq)

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
Timerange: Basic block 0 - 34813862
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 3465, part 1)
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
141,411,327 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
21,001,325 (14.85%)  ???:nano_lance::lance_table_read_dataset(std::filesystem::__cxx11::path const&, ArrowSchema&, std::vector<ArrowArray, std::allocator<ArrowArray> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
20,339,523 (14.38%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
17,800,870 (12.59%)  ???:nano_lance::(anonymous namespace)::append_one_string(ArrowArray&, std::basic_string_view<char, std::char_traits<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
15,600,684 (11.03%)  ???:ArrowArrayAppendUInt(ArrowArray*, unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
11,200,000 ( 7.92%)  ???:ArrowArrayFinishElement(ArrowArray*) [/home/runner/work/nanolance/nanolance/build/nlbench]
10,417,052 ( 7.37%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_range_insert<__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
10,411,380 ( 7.36%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_range_insert<unsigned char const*>(__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, unsigned char const*, unsigned char const*, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
10,400,773 ( 7.35%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_range_insert<__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
 7,146,944 ( 5.05%)  ???:void nano_lance::fastlanes::unpack_1024<unsigned long>(unsigned int, unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
 6,600,000 ( 4.67%)  ???:nano_lance::(anonymous namespace)::append_string_at_row(ArrowArray&, nano_lance::VariableWidthColumnValues const&, unsigned long, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
 5,248,475 ( 3.71%)  ???:nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   714,983 ( 0.51%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
   520,371 ( 0.37%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   506,812 ( 0.36%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   422,964 ( 0.30%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
   190,786 ( 0.13%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   188,313 ( 0.13%)  ./malloc/./malloc/malloc.c:_int_free [/usr/lib/x86_64-linux-gnu/libc.so.6]

--------------------------------------------------------------------------------
The following files chosen for auto-annotation could not be found:
--------------------------------------------------------------------------------
  ./elf/../sysdeps/generic/dl-new-hash.h
  ./elf/./elf/dl-lookup.c
  ./malloc/./malloc/malloc.c
  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S
  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S

```
