# src/encoding: how one page of one column is laid out

Lance's own encoding crate (`lance-encoding`) separates two things, and so does this directory:

- **value encodings** (`values/`): how a buffer of values is compressed -- bitpacking, run-length, dictionary,
  FSST, general (zstd / LZ4) compression, byte-stream split. Lance's `encodings/physical/`.
- **page layouts** (`layout/`): how a page is structured around those buffers -- mini-block chunks with their
  repetition / definition levels, full-zip rows, constant and all-null pages. Lance's `encodings/logical/`
  and the `PageLayout` messages of `encodings_v2_1.proto`.

The page decoders (`lance_column_decoder.cpp`, `lance_v20_decoder.cpp` for the 2.0 format) read a column's
pages through `EncodingsIo` (`nanolance/encodings_io.hpp`, implemented by the file layer) and put the
pieces together; the file layer's writer (`file/data_file_writer.cpp`) writes pages from the same pieces.
What to encode with is chosen in `encoding_choice.cpp` (whether RLE, bitpacking, a constant or a dictionary
pays for a column).

## A module

One module per encoding, `values/<name>.{hpp,cpp}` or `layout/<name>.{hpp,cpp}`, namespace
`nano_lance::<name>`, holding **both directions** of that encoding and nothing else:

- `encode_*` functions: values (or a page's parts) in, the encoded bytes out, as Lance writes them.
- `decode_*` functions: the encoded bytes in, values out, refusing malformed input with a message (the read
  path's input is untrusted: sizes and counts come from the file).
- No I/O, no schema mapping, no choice of encoding: pure functions on buffers, so a test can run them in
  memory.
- The header says the byte layout in a few lines, and names Lance's source it follows.

The modules so far:

| module | what it holds |
|---|---|
| `values/bitpack` | FastLanes bitpacking: `encode_block` / `decode_block` for an inline block, `decode_series` for a buffer of blocks, `decode_out_of_line` for a descriptor-width buffer |
| `values/general` | zstd / LZ4 general compression and nanolance's `[u64 size][zstd]` frame |
| `values/byte_stream_split` | byte-stream split of fixed-width values |
| `values/rle` | run-length values and run-length definition levels |
| `values/dictionary` | dictionary blocks of fixed and variable-width values, and their indices |
| `values/scalar_value` | a constant page's one value |
| `layout/miniblock` | mini-block chunk framing (header, levels, buffers, padding) and the metadata words that index the chunks |

## Its test

Each module has a pair test, `tests/encoding/test_<name>.cpp` (ctest `nano_lance_encoding_<name>`, label
`smoke`), that checks encode then decode gives back the input:

- for every value width and the parameter edges the format has (bit widths, run lengths, dictionary sizes,
  block boundaries: 0, 1, a block minus one, a block, more than a block);
- the decoder's refusals: truncated buffers, impossible widths or counts, sizes that do not match;
- where the module only decodes (Lance writes something nanolance does not), hand-built bytes from the
  layout the header describes.

That a page round-trips through nanolance and through pylance (the interop half) stays with the binding
tests (`bindings/python/tests/test_lance_parity.py`, `test_interop.py`, `test_writer_tail.py`), which read
files each library wrote with the other.
