# Linux CI results (auto-generated — do not edit)

_2026-07-09 20:17:38 UTC · commit `841723f`_

## Environment
```
Linux runnervm5mmn9 6.17.0-1018-azure #18~24.04.1-Ubuntu SMP Thu May 28 16:39:11 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Ubuntu clang version 19.1.1 (1ubuntu1~24.04.2)
cmake version 3.31.6
```

## Tests
```
smoke    =   4.81 sec*proc (28 tests)

Total Test time (real) =   4.92 sec
```

## Benchmark (parquet vs rust lance vs nanolance)
```

========== pcap_ref  (200000 rows, 3 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         22.59       22.59    4.189      5.81                (1.00x vs pq)
rust lance             10.53       10.53    3.472      6.05                (0.83x vs pq)
nanolance              15.68       23.45    3.643      9.34         9.59   (0.87x vs pq)

========== wide_int  (200000 rows, 4 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         24.42       24.42    5.212      3.64                (1.00x vs pq)
rust lance              2.69        2.69    8.485      3.84                (1.63x vs pq)
nanolance              10.25       13.38    9.013      9.57        11.82   (1.73x vs pq)

========== high_card  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         29.72       29.72   15.513      8.07                (1.00x vs pq)
rust lance             17.56       17.56   18.943      5.80                (1.22x vs pq)
nanolance              44.00       49.75   18.206     13.19        11.97   (1.17x vs pq)

========== float_smooth  (200000 rows, 2 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)         26.04       26.04   14.600      3.10                (1.00x vs pq)
rust lance              2.95        2.95   11.991      3.28                (0.82x vs pq)
nanolance               7.50       10.59    8.399      4.03         7.00   (0.58x vs pq)

========== bool_flags  (200000 rows, 1 cols) ==========
engine           write(core) write(proc)    B/row   read ms  read(lance)
parquet (zstd)          1.89        1.89    0.128      1.42                (1.00x vs pq)
rust lance              1.31        1.31    0.129      2.28                (1.01x vs pq)
nanolance               1.48        2.83    0.127      0.81         2.57   (0.99x vs pq)

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
Timerange: Basic block 0 - 17525708
Trigger: Program termination
Profiled target:  build/nlbench /tmp/nlb/pcap_ref_nl.lance 1 (PID 4028, part 1)
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
31,205,419 (100.0%)  PROGRAM TOTALS

--------------------------------------------------------------------------------
Ir                   file:function
--------------------------------------------------------------------------------
12,440,099 (39.87%)  ./string/../sysdeps/x86_64/multiarch/memset-vec-unaligned-erms.S:__memset_avx2_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 8,444,620 (27.06%)  ./string/../sysdeps/x86_64/multiarch/memmove-vec-unaligned-erms.S:__memcpy_avx_unaligned_erms [/usr/lib/x86_64-linux-gnu/libc.so.6]
 3,700,683 (11.86%)  ???:bool nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&)::$_0::operator()<std::vector<unsigned int, std::allocator<unsigned int> > >(std::vector<unsigned int, std::allocator<unsigned int> >&) const [/home/runner/work/nanolance/nanolance/build/nlbench]
 1,969,016 ( 6.31%)  ???:void nano_lance::fastlanes::unpack_1024<unsigned long>(unsigned int, unsigned long const*, unsigned long*) [/home/runner/work/nanolance/nanolance/build/nlbench]
 1,422,012 ( 4.56%)  ???:nano_lance::decode_lance_physical_column(std::filesystem::__cxx11::path const&, nano_lance::pb::Field const&, nano_lance::pb::ColumnMetadata const&, nano_lance::ColumnValues&, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >&) [/home/runner/work/nanolance/nanolance/build/nlbench]
   533,258 ( 1.71%)  ./elf/./elf/dl-lookup.c:do_lookup_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   512,596 ( 1.64%)  ./elf/../sysdeps/generic/dl-new-hash.h:_dl_lookup_symbol_x
   192,920 ( 0.62%)  ./elf/./elf/dl-lookup.c:_dl_lookup_symbol_x [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   144,992 ( 0.46%)  ./elf/./elf/dl-reloc.c:_dl_relocate_object [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   117,530 ( 0.38%)  ./elf/./elf/dl-lookup.c:check_match [/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2]
   111,853 ( 0.36%)  ./elf/../sysdeps/x86_64/dl-machine.h:_dl_relocate_object
   110,503 ( 0.35%)  ./elf/./elf/do-rel.h:_dl_relocate_object
   109,747 ( 0.35%)  ./malloc/./malloc/malloc.c:_int_free [/usr/lib/x86_64-linux-gnu/libc.so.6]
   102,435 ( 0.33%)  ???:nano_lance::pb::decode_column_metadata(std::vector<unsigned char, std::allocator<unsigned char> > const&, nano_lance::pb::ColumnMetadata&) [/home/runner/work/nanolance/nanolance/build/nlbench]
    83,531 ( 0.27%)  ./malloc/./malloc/malloc.c:_int_malloc [/usr/lib/x86_64-linux-gnu/libc.so.6]
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
    24,773 ( 0.08%)  ./malloc/./malloc/malloc.c:malloc_consolidate [/usr/lib/x86_64-linux-gnu/libc.so.6]
    23,403 ( 0.07%)  ???:std::istream::sentry::sentry(std::istream&, bool) [/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33]

--------------------------------------------------------------------------------
The following files chosen for auto-annotation could not be found:
--------------------------------------------------------------------------------
  ./elf/../sysdeps/generic/dl-new-hash.h
  ./elf/../sysdeps/x86_64/dl-machine.h
  ./elf/./elf/dl-lookup.c
```
