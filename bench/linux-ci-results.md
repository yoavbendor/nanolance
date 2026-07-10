# Linux CI results (auto-generated — do not edit)

_2026-07-10 10:52:39 UTC · commit `a8b3d8f`_

## Environment
```
Linux runnervm5mmn9 6.17.0-1018-azure #18~24.04.1-Ubuntu SMP Thu May 28 16:39:11 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 19.1.1 (1ubuntu1~24.04.2)
cmake version 3.31.6
```

## Tests
```
smoke    =   3.66 sec*proc (28 tests)

Total Test time (real) =   3.67 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         17.15       17.15    4.189      4.83                (1.00x vs pq)
rust lance              8.30        8.30    3.472      4.38                (0.83x vs pq)
nanolance               5.81       12.04    3.643      8.64         6.87   (0.87x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         18.13       18.13    5.212      2.74                (1.00x vs pq)
rust lance              2.12        2.12    8.485      2.44                (1.63x vs pq)
nanolance               5.39        8.31    9.013      8.92         9.10   (1.73x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         23.31       23.31   15.513      6.56                (1.00x vs pq)
rust lance             15.08       15.08   18.983      4.80                (1.22x vs pq)
nanolance              26.39       31.32   18.206     10.54         8.88   (1.17x vs pq)

========== float_smooth  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         20.50       20.50   14.600      2.30                (1.00x vs pq)
rust lance              6.71        6.71    8.686      4.02                (0.59x vs pq)
nanolance               5.00        7.78    8.399      3.40         5.28   (0.58x vs pq)

========== bool_flags  (200000 rows, 1 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)          1.60        1.60    0.128      1.14                (1.00x vs pq)
rust lance              1.00        1.00    0.129      1.63                (1.01x vs pq)
nanolance               0.58        1.74    0.127      0.71         1.82   (0.99x vs pq)

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
Timerange: Basic block 0 - 17531120
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 3858, part 1)
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
31,295,913 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
12,440,099 (39.75%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 8,444,620 (26.98%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 3,700,683 (11.82%)  ???:bool nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&)::$_0::operator()<std::vector<unsigned int, std::allocator<unsigned int> > >(std::vector<unsigned int, std::allocator<unsigned int> >&) const [/home/runner/work/nanolance/nanolance/build/nlbench]
 1,422,030 ( 4.54%)  ???:nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   923,505 ( 2.95%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 28u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
   533,258 ( 1.70%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   512,596 ( 1.64%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   459,580 ( 1.47%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 27u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
   237,270 ( 0.76%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 29u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
   226,050 ( 0.72%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 26u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
   192,920 ( 0.62%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   144,992 ( 0.46%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   117,530 ( 0.38%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   112,781 ( 0.36%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object
   111,155 ( 0.36%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 25u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
   111,083 ( 0.35%)  ./elf/./elf/do-rel.h:_dl_relocate_object
   109,820 ( 0.35%)  ./malloc/./malloc/malloc.c:_int_free [/usr/lib/x86_64-linux-gnu/libc.so.6]
   102,435 ( 0.33%)  ???:nano_lance::pb::decode_column_metadata(std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::pb::ColumnMetadata&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    84,112 ( 0.27%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    81,658 ( 0.26%)  ./malloc/./malloc/malloc.c:malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
    73,880 ( 0.24%)  ./string/../sysdeps/x86_64/multiarch/../multiarch/strcmp-sse2.S:strcmp [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    68,289 ( 0.22%)  ./elf/./elf/dl-tunables.c:__GI___tunables_init [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
    58,824 ( 0.19%)  ???:nano_lance::pb::(anonymous namespace)::read_varint(std::vector<unsigned char, std::allocator<unsigned char> > const&, unsigned long&, unsigned long&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    52,503 ( 0.17%)  ./malloc/./malloc/malloc.c:free [/usr/lib/x86_64-linux-gnu/libc.so.6]
    49,675 ( 0.16%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 24u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
    46,635 ( 0.15%)  ???:nano_lance::read_lance_data_file_bytes(std::filesystem::__cxx11::path const&, unsigned long, unsigned long, std::vector<unsigned char, std::allocator<unsigned char> >&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    41,526 ( 0.13%)  ???:std::vector<unsigned char, std::allocator<unsigned char> >::resize(unsigned long) [/home/runner/work/nanolance/nanolance/build/nlbench]
    37,118 ( 0.12%)  ???:void std::vector<unsigned char, std::allocator<unsigned char> >::_M_assign_aux<__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > > >(__gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, __gnu_cxx::__normal_iterator<unsigned char const*, std::vector<unsigned char, std::allocator<unsigned char> > >, std::forward_iterator_tag) [/home/runner/work/nanolance/nanolance/build/nlbench]
    29,295 ( 0.09%)  ???:void nano_lance::fastlanes::unpack_1024_w<unsigned long, 23u>(unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
    29,152 ( 0.09%)  ???:operator new(unsigned long) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    28,114 ( 0.09%)  ???:std::basic_streambuf<char, std::char_traits<char> >::xsgetn(char*, long) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    27,268 ( 0.09%)  ???:std::basic_filebuf<char, std::char_traits<char> >::underflow() [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    24,977 ( 0.08%)  ./malloc/./malloc/malloc.c:malloc_consolidate [/usr/lib/x86_64-linux-gnu/libc.so.6]
    23,403 ( 0.07%)  ???:std::istream::sentry::sentry(std::istream&, bool) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
    20,553 ( 0.07%)  ???:std::basic_filebuf<char, std::char_traits<char> >::_M_seek(long, std::_Ios_Seekdir, __mbstate_t) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]
```
