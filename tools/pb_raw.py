#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Dump a protobuf message's wire structure without a schema or protoc.

Used to read the raw `PageLayout` descriptors that `nlance-pagelayout --dump-corpus` writes, when the
tool's own summary does not show a field yet. Field numbers are in Lance's
`protos/encodings_v2_1.proto`.

    python3 tools/pb_raw.py corpus/descriptor_1.bin

Length-delimited fields are tried as nested messages first and shown as bytes when that fails. That
guess is sometimes wrong for short packed fields (a 2-byte `layers` value can parse as a message), so
read the output with the .proto open.
"""

import sys


def _varint(b, i):
    result = shift = 0
    while True:
        byte = b[i]
        i += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return result, i


def dump(b, indent=0, depth=0):
    i, out, pad = 0, [], " " * indent
    while i < len(b):
        key, i = _varint(b, i)
        field, wire = key >> 3, key & 7
        if wire == 0:
            value, i = _varint(b, i)
            out.append(f"{pad}f{field}={value}")
        elif wire == 2:
            n, i = _varint(b, i)
            sub, i = b[i:i + n], i + n
            try:
                if depth >= 8 or n == 0:
                    raise ValueError
                inner = dump(sub, indent + 2, depth + 1)
                out += [f"{pad}f{field}{{", *inner, f"{pad}}}"]
            except (ValueError, IndexError):
                out.append(f"{pad}f{field}=bytes[{n}] {sub[:24].hex()}")
        elif wire == 5:
            out.append(f"{pad}f{field}=fixed32")
            i += 4
        elif wire == 1:
            out.append(f"{pad}f{field}=fixed64")
            i += 8
        else:
            raise ValueError(f"unsupported wire type {wire}")
    return out


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: pb_raw.py FILE")
    with open(sys.argv[1], "rb") as handle:
        print("\n".join(dump(handle.read())))
