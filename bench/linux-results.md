# Linux CI results

_2026-06-09 07:25:56 UTC · commit `f0e2b41`_

## Environment
```
Linux runnervm3jyl0 6.17.0-1015-azure #15~24.04.1-Ubuntu SMP Wed May  6 22:37:49 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 18.1.3 (1ubuntu1)
cmake version 3.31.6
```

## Tests
```
smoke    =   0.10 sec*proc (17 tests)

Total Test time (real) =   0.11 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine            write ms   file bytes    B/row   read ms  read(lance)
parquet (zstd)        31.3      837,834    4.189      8.26                (1.00x vs pq)
rust lance            13.7      694,409    3.472      5.68                (0.83x vs pq)
nanolance             55.8      728,616    3.643     28.58        10.00   (0.87x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine            write ms   file bytes    B/row   read ms  read(lance)
parquet (zstd)        30.9    1,042,350    5.212      4.03                (1.00x vs pq)
rust lance             4.8    1,696,945    8.485      4.06                (1.63x vs pq)
nanolance             13.1    1,802,616    9.013     28.59        12.24   (1.73x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine            write ms   file bytes    B/row   read ms  read(lance)
parquet (zstd)        39.7    3,102,688   15.513      9.07                (1.00x vs pq)
rust lance            19.5    3,784,810   18.924      6.42                (1.22x vs pq)
nanolance            316.2    3,641,112   18.206     24.06        12.31   (1.17x vs pq)

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
Timerange: Basic block 0 - 71813732
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 3370, part 1)
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
290,010,665 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
57,200,904 (19.72%)  ???:nano_lance::lance_table_read_dataset(std::filesystem::__cxx11::path const&, ArrowSchema&, std::vector<ArrowArray, std::allocator<ArrowArray> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
42,000,000 (14.48%)  ???:nano_lance::(anonymous namespace)::append_column_value_at_row(nano_lance::LanceField const&, nano_lance::ColumnValues const&, long, std::vector<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > > > const*, ArrowArray&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
29,002,402 (10.00%)  ./string/../sysdeps/x86_64/multiarch/memcmp-avx2-movbe.S:__memcmp_avx2_movbe [/usr/lib/x86_64-linux-gnu/libc.so.6]
20,339,483 ( 7.01%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
19,601,624 ( 6.76%)  ./string/../sysdeps/x86_64/multiarch/strlen-avx2.S:__strlen_avx2 [/usr/lib/x86_64-linux-gnu/libc.so.6]
17,800,870 ( 6.14%)  ???:nano_lance::(anonymous namespace)::append_one_string(ArrowArray&, std::basic_string_view<char, std::char_traits<char> >, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
16,000,040 ( 5.52%)  ???:bool std::operator==<char, std::char_traits<char>, std::allocator<char> >(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, char const*) [/home/runner/work/nanolance/nanolance/build/nlbench]
15,600,684 ( 5.38%)  ???:ArrowArrayAppendUInt(ArrowArray*, unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
12,400,031 ( 4.28%)  ???:nano_lance::lance_logical_type_value_bytes(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&) [/home/runner/work/nanolance/nanolance/build/nlbench]
11,200,000 ( 3.86%)  ???:ArrowArrayFinishElement(ArrowArray*) [/home/runner/work/nanolance/nanolance/build/nlbench]
10,417,052 ( 3.59%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_range_insert<__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
10,411,380 ( 3.59%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_range_insert<unsigned char const*>(__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, unsigned char const*, unsigned char const*, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
10,400,773 ( 3.59%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_range_insert<__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
 7,146,944 ( 2.46%)  ???:void nano_lance::fastlanes::unpack_1024<unsigned long>(unsigned int, unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
 5,248,475 ( 1.81%)  ???:nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]

--------------------------------------------------------------------------------
The following files chosen for auto-annotation could not be found:
--------------------------------------------------------------------------------
  ./string/../sysdeps/x86_64/multiarch/memcmp-avx2-movbe.S
  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S
  ./string/../sysdeps/x86_64/multiarch/strlen-avx2.S

```
