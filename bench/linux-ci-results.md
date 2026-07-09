# Linux CI results (auto-generated — do not edit)

_2026-07-09 21:44:29 UTC · commit `e206212`_

## Environment
```
Linux runnervm5mmn9 6.17.0-1018-azure #18~24.04.1-Ubuntu SMP Thu May 28 16:39:11 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 19.1.1 (1ubuntu1~24.04.2)
cmake version 3.31.6
```

## Tests
```
smoke    =   4.45 sec*proc (28 tests)

Total Test time (real) =   4.51 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         23.15       23.15    4.189      6.29                (1.00x vs pq)
rust lance             10.45       10.45    3.472      4.95                (0.83x vs pq)
nanolance               9.81       17.82    3.643     12.34         8.42   (0.87x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         24.77       24.77    5.212      3.77                (1.00x vs pq)
rust lance              2.55        2.55    8.485      3.15                (1.63x vs pq)
nanolance              10.10       13.98    9.013     11.45        11.49   (1.73x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         30.28       30.28   15.513      8.25                (1.00x vs pq)
rust lance             18.31       18.31   18.993      5.43                (1.22x vs pq)
nanolance              33.65       39.46   18.206     12.84        11.00   (1.17x vs pq)

========== float_smooth  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         25.51       25.51   14.600      2.99                (1.00x vs pq)
rust lance              2.53        2.53   11.991      2.89                (0.82x vs pq)
nanolance               7.73       10.98    8.399      4.54         7.02   (0.58x vs pq)

========== bool_flags  (200000 rows, 1 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)          2.07        2.07    0.128      1.53                (1.00x vs pq)
rust lance              1.21        1.21    0.129      2.02                (1.01x vs pq)
nanolance               1.47        2.99    0.127      0.93         2.34   (0.99x vs pq)

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
Timerange: Basic block 0 - 17525792
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 4001, part 1)
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
31,205,924 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
12,440,099 (39.86%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 8,444,620 (27.06%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 3,700,683 (11.86%)  ???:bool nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&)::$_0::operator()<std::vector<unsigned int, std::allocator<unsigned int> > >(std::vector<unsigned int, std::allocator<unsigned int> >&) const [/home/runner/work/nanolance/nanolance/build/nlbench]
 1,969,016 ( 6.31%)  ???:void nano_lance::fastlanes::unpack_1024<unsigned long>(unsigned int, unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
 1,422,030 ( 4.56%)  ???:nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   533,258 ( 1.71%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   512,596 ( 1.64%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   192,920 ( 0.62%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   144,992 ( 0.46%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   117,530 ( 0.38%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   111,853 ( 0.36%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object
   110,503 ( 0.35%)  ./elf/./elf/do-rel.h:_dl_relocate_object
   109,747 ( 0.35%)  ./malloc/./malloc/malloc.c:_int_free [/usr/lib/x86_64-linux-gnu/libc.so.6]
   102,435 ( 0.33%)  ???:nano_lance::pb::decode_column_metadata(std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::pb::ColumnMetadata&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    83,842 ( 0.27%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    81,658 ( 0.26%)  ./malloc/./malloc/malloc.c:malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    73,880 ( 0.24%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-sse2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    68,289 ( 0.22%)  ./elf/./elf/dl-tunables.c:__GI___tunables_init [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    58,824 ( 0.19%)  ???:nano_lance::pb::(anonymous namespace)::read_varint(std::vector<unsigned char, std::allocator<unsigned char> > const&, unsigned long&, unsigned long&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    52,475 ( 0.17%)  ./malloc/./malloc/malloc.c:free [/usr/lib/x86_64-linux-gnu/libc.so.6]
    46,635 ( 0.15%)  ???:nano_lance::read_lance_data_file_bytes(std::filesystem::__cxx11::path const&, unsigned long, unsigned long, std::vector<unsigned char, std::allocator<unsigned char> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    41,526 ( 0.13%)  ???:std::vector<unsigned char, std::allocator<unsigned char> >::resize(unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    37,118 ( 0.12%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_assign_aux<__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
    29,152 ( 0.09%)  ???:operator new(unsigned long) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    28,114 ( 0.09%)  ???:std::basic_streambuf<char, std::char_traits<char> >::xsgetn(char*, long) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    27,268 ( 0.09%)  ???:std::basic_filebuf<char, std::char_traits<char> >::underflow() [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    24,764 ( 0.08%)  ./malloc/./malloc/malloc.c:malloc_consolidate [/usr/lib/x86_64-linux-gnu/libc.so.6]
    23,403 ( 0.07%)  ???:std::istream::sentry::sentry(std::istream&, bool) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]

--------------------------------------------------------------------------------
The following files chosen for auto-annotation could not be found:
--------------------------------------------------------------------------------
  ./elf/../sysdeps/generic/dl-new-hash.h
  ./elf/../sysdeps/x86_64/dl-machine.h
  ./elf/./elf/dl-lookup.c
```
