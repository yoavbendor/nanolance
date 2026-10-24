# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""``lance.mem_wal``: MemWAL's metadata and sharding specs, under pylance's names.

Implemented: ``LanceDataset.initialize_mem_wal`` and ``mem_wal_index_details`` (the ``__lance_mem_wal``
system index: its sharding specs, maintained indexes and writer-config defaults), and
``evaluate_sharding_spec``, which routes rows to shards with Lance's hash.

Not implemented: the writers, memtables, SSTables and the LSM scanners and planners. Their names are
here so that code importing them works, and using any of them raises ``NotSupportedError``.
"""

from __future__ import annotations

import struct
from dataclasses import asdict, dataclass, field
from typing import Any, Dict, List, Mapping, Optional, Union

import pyarrow as pa

from nanolance import _nanolance
from nanolance.lance._errors import native, unsupported

__all__ = [
    "ShardingField",
    "ShardingSpec",
    "evaluate_sharding_spec",
    "CompactedSsTable",
    "ShardSnapshot",
    "ShardWriter",
    "LsmScanner",
    "ExecutionPlan",
    "LsmPointLookupPlanner",
    "LsmVectorSearchPlanner",
]

INDEX_NAME = "__lance_mem_wal"
_DETAILS_SUFFIX = "MemWalIndexDetails"
_MAX_NUM_BUCKETS = 1024
_HNSW_PARAMS = ("num_edges", "ef_construction", "max_level")


# ---------------------------------------------------------------------------
# Sharding specs
# ---------------------------------------------------------------------------


@dataclass
class ShardingField:
    """Defines one MemWAL sharding field.

    Parameters
    ----------
    field_id : str
        Identifier for the derived field.
    source_ids : list of int
        Source field IDs used as inputs.
    transform : str, optional
        Optional transform name applied to the source fields.
    expression : str, optional
        Optional expression used to derive the field value.
    result_type : str
        Output type name for the derived field.
    parameters : dict of str to str, optional
        Extra transform parameters.
    """

    field_id: str
    source_ids: List[int]
    transform: Optional[str] = None
    expression: Optional[str] = None
    result_type: str = ""
    parameters: Dict[str, str] = field(default_factory=dict)


@dataclass
class ShardingSpec:
    """Specification for deriving MemWAL shard routing values."""

    spec_id: int
    fields: List[ShardingField]


def evaluate_sharding_spec(batch: pa.RecordBatch, spec: Union[ShardingSpec, Mapping[str, object]],
                           schema) -> pa.RecordBatch:
    """Evaluate a MemWAL sharding spec against one PyArrow RecordBatch.

    Parameters
    ----------
    batch : pyarrow.RecordBatch
        Input batch containing the sharding source columns.
    spec : ShardingSpec or dict
        MemWAL sharding spec to evaluate.
    schema : LanceSchema
        Lance table schema used to resolve source field IDs in the spec to
        input batch column names.

    Returns
    -------
    pyarrow.RecordBatch
        One column per sharding field, named by its ``field_id``. Bucket fields are ``int32``
        bucket numbers; identity fields are the source column; unsharded fields are ``int32`` zeros.
    """
    if not isinstance(batch, pa.RecordBatch):
        raise TypeError(f"Expected pyarrow.RecordBatch, got {type(batch)!r}")
    spec_dict = dict(spec) if isinstance(spec, Mapping) else asdict(spec)
    paths = _column_paths(schema)
    names: List[str] = []
    arrays: List[pa.Array] = []
    for sharding in spec_dict["fields"]:
        sharding = dict(sharding)
        field_id = sharding["field_id"]
        transform = sharding.get("transform")
        if transform == "bucket":
            array = _bucket_array(batch, sharding, paths)
        elif transform == "identity":
            array = batch.column(_source_index(batch, sharding, paths))
        elif transform == "unsharded":
            array = pa.array([0] * batch.num_rows, pa.int32())
        else:
            raise ValueError(f"Unsupported MemWAL sharding transform for field '{field_id}': {transform!r}")
        names.append(field_id)
        arrays.append(array)
    return pa.RecordBatch.from_arrays(arrays, names=names)


def _column_paths(schema) -> Dict[int, str]:
    """Source field id -> the batch column name: the dotted path of the field's ancestry."""
    out: Dict[int, str] = {}

    def walk(fields, prefix: str) -> None:
        for f in fields:
            path = f"{prefix}.{f.name()}" if prefix else f.name()
            out[f.id()] = path
            walk(f.children(), path)

    walk(schema.fields(), "")
    return out


def _source_index(batch: pa.RecordBatch, sharding: Mapping[str, Any], paths: Mapping[int, str]) -> int:
    field_id = sharding["field_id"]
    column = (sharding.get("parameters") or {}).get("column")
    if column is None or not str(column).strip():
        source_ids = list(sharding.get("source_ids") or [])
        if not source_ids:
            raise ValueError(f"MemWAL sharding field '{field_id}' has no source column")
        if source_ids[0] not in paths:
            raise ValueError(f"MemWAL sharding field '{field_id}' source id {source_ids[0]} "
                             "was not mapped to a batch column")
        column = paths[source_ids[0]]
    index = batch.schema.get_field_index(column)
    if index < 0:
        raise ValueError(f"Sharding source column '{column}' not found in batch")
    return index


def _bucket_array(batch: pa.RecordBatch, sharding: Mapping[str, Any], paths: Mapping[int, str]) -> pa.Array:
    field_id = sharding["field_id"]
    params = sharding.get("parameters") or {}
    if "num_buckets" not in params:
        raise ValueError(f"Bucket sharding field '{field_id}' missing 'num_buckets' parameter")
    try:
        num_buckets = int(params["num_buckets"])
    except ValueError as exc:
        raise ValueError(f"Bucket sharding field '{field_id}' has invalid num_buckets "
                         f"'{params['num_buckets']}': {exc}") from None
    if num_buckets <= 0:
        raise ValueError(f"Bucket sharding field '{field_id}' requires positive num_buckets, got {num_buckets}")
    column = batch.column(_source_index(batch, sharding, paths))
    hashes = _hash_column(column)
    return pa.array([(h & 0x7FFFFFFF) % num_buckets for h in hashes], pa.int32())


# ---------------------------------------------------------------------------
# Lance's hash for bucket sharding (lance-12 dataset/mem_wal/sharding.rs): Murmur3 with seed 0 over the
# value's bytes, except that the tail bytes of a string are mixed one by one, sign-extended. Every
# intermediate is kept modulo 2**32 (or 2**64 for longs), which is what Rust's wrapping i32 arithmetic is.
# A null hashes to the seed, 0.
# ---------------------------------------------------------------------------

_M32 = 0xFFFFFFFF
_M64 = 0xFFFFFFFFFFFFFFFF


def _rotl32(x: int, r: int) -> int:
    return ((x << r) | (x >> (32 - r))) & _M32


def _mix_k1(k1: int) -> int:
    k1 = (k1 * 0xCC9E2D51) & _M32
    k1 = _rotl32(k1, 15)
    return (k1 * 0x1B873593) & _M32


def _mix_h1(h1: int, k1: int) -> int:
    h1 = _rotl32((h1 ^ k1) & _M32, 13)
    return (h1 * 5 + 0xE6546B64) & _M32


def _fmix(h1: int, length: int) -> int:
    h1 = (h1 ^ length) & _M32
    h1 ^= h1 >> 16
    h1 = (h1 * 0x85EBCA6B) & _M32
    h1 ^= h1 >> 13
    h1 = (h1 * 0xC2B2AE35) & _M32
    h1 ^= h1 >> 16
    return h1


def _hash_int(value: int, seed: int = 0) -> int:
    return _fmix(_mix_h1(seed, _mix_k1(value & _M32)), 4)


def _hash_long(value: int, seed: int = 0) -> int:
    value &= _M64
    h1 = _mix_h1(seed, _mix_k1(value & _M32))
    h1 = _mix_h1(h1, _mix_k1((value >> 32) & _M32))
    return _fmix(h1, 8)


def _hash_bytes(data: bytes, seed: int = 0) -> int:
    h1 = seed
    full = len(data) - len(data) % 4
    for i in range(0, full, 4):
        h1 = _mix_h1(h1, _mix_k1(int.from_bytes(data[i:i + 4], "little")))
    for byte in data[full:]:
        h1 = _mix_h1(h1, _mix_k1((byte - 256 if byte >= 128 else byte) & _M32))
    return _fmix(h1, len(data))


def _float32_bits(value: float) -> int:
    if value == 0.0:
        return 0  # -0.0 hashes as 0.0
    if value != value:
        return 0x7FC00000
    return struct.unpack("<I", struct.pack("<f", value))[0]


def _float64_bits(value: float) -> int:
    if value == 0.0:
        return 0
    if value != value:
        return 0x7FF8000000000000
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def _hash_column(column: pa.Array) -> List[int]:
    """The hash of every value of a column (0 for a null)."""
    t = column.type
    if pa.types.is_boolean(t):
        return [0 if v is None else _hash_int(1 if v else 0) for v in column.to_pylist()]
    if pa.types.is_int8(t) or pa.types.is_int16(t) or pa.types.is_int32(t) \
            or pa.types.is_uint8(t) or pa.types.is_uint16(t) or pa.types.is_uint32(t):
        return _hash_each(column, _hash_int)
    if pa.types.is_date32(t) or pa.types.is_time32(t):
        return _hash_each(column.cast(pa.int32()), _hash_int)
    if pa.types.is_int64(t) or pa.types.is_uint64(t):
        return _hash_each(column, _hash_long)
    if pa.types.is_timestamp(t) or pa.types.is_time64(t):
        return _hash_each(column.cast(pa.int64()), _hash_long)
    if pa.types.is_float32(t):
        return [0 if v is None else _hash_int(_float32_bits(v)) for v in column.to_pylist()]
    if pa.types.is_float64(t):
        return [0 if v is None else _hash_long(_float64_bits(v)) for v in column.to_pylist()]
    if pa.types.is_string(t) or pa.types.is_large_string(t):
        return [0 if v is None else _hash_bytes(v.encode("utf-8")) for v in column.to_pylist()]
    raise ValueError(f"Unsupported bucket sharding column type: {t}")


def _hash_each(column: pa.Array, hash_value) -> List[int]:
    return [0 if v is None else hash_value(v) for v in column.to_pylist()]


# ---------------------------------------------------------------------------
# The MemWAL index's details: encoding (lance.table.MemWalIndexDetails) and decoding
# ---------------------------------------------------------------------------


def _varint_put(out: bytearray, value: int) -> None:
    value &= 0xFFFFFFFFFFFFFFFF
    while value >= 0x80:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    out.append(value)


def _key_put(out: bytearray, number: int, wire: int) -> None:
    _varint_put(out, (number << 3) | wire)


def _bytes_put(out: bytearray, number: int, data: bytes) -> None:
    _key_put(out, number, 2)
    _varint_put(out, len(data))
    out += data


def _uint_put(out: bytearray, number: int, value: int) -> None:
    if value:  # proto3: a zero scalar is not written
        _key_put(out, number, 0)
        _varint_put(out, value)


def _map_entry_put(out: bytearray, number: int, key: str, value: str) -> None:
    entry = bytearray()
    _bytes_put(entry, 1, key.encode("utf-8"))
    _bytes_put(entry, 2, value.encode("utf-8"))
    _bytes_put(out, number, bytes(entry))


def _encode_field(sharding: Mapping[str, Any]) -> bytes:
    out = bytearray()
    _bytes_put(out, 1, str(sharding["field_id"]).encode("utf-8"))
    packed = bytearray()
    for source in sharding.get("source_ids") or []:
        _varint_put(packed, int(source))
    if packed:
        _bytes_put(out, 2, bytes(packed))
    if sharding.get("transform") is not None:
        _bytes_put(out, 3, str(sharding["transform"]).encode("utf-8"))
    if sharding.get("expression") is not None:
        _bytes_put(out, 4, str(sharding["expression"]).encode("utf-8"))
    if sharding.get("result_type"):
        _bytes_put(out, 5, str(sharding["result_type"]).encode("utf-8"))
    for key, value in sorted((sharding.get("parameters") or {}).items()):
        _map_entry_put(out, 6, str(key), str(value))
    return bytes(out)


def _encode_details(num_shards: int, specs: List[Mapping[str, Any]], maintained: List[str],
                    defaults: Mapping[str, str]) -> bytes:
    out = bytearray()
    _uint_put(out, 2, num_shards)
    for spec in specs:
        message = bytearray()
        _uint_put(message, 1, int(spec["spec_id"]))
        for sharding in spec["fields"]:
            _bytes_put(message, 2, _encode_field(sharding))
        _bytes_put(out, 7, bytes(message))
    for name in maintained:
        _bytes_put(out, 8, name.encode("utf-8"))
    for key in sorted(defaults):
        _map_entry_put(out, 11, key, defaults[key])
    return bytes(out)


def _wire_fields(data: bytes):
    """(field number, wire type, value) of each field: bytes for length-delimited, an int for a varint."""
    pos = 0
    while pos < len(data):
        key, pos = _varint_get(data, pos)
        number, wire = key >> 3, key & 7
        if wire == 0:
            value, pos = _varint_get(data, pos)
        elif wire == 2:
            size, pos = _varint_get(data, pos)
            value, pos = data[pos:pos + size], pos + size
        elif wire == 1:
            value, pos = data[pos:pos + 8], pos + 8
        elif wire == 5:
            value, pos = data[pos:pos + 4], pos + 4
        else:
            raise ValueError("malformed MemWAL index details")
        yield number, wire, value


def _varint_get(data: bytes, pos: int):
    value = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return value, pos


def _decode_map_entry(data: bytes):
    key = value = ""
    for number, _, v in _wire_fields(data):
        if number == 1:
            key = v.decode("utf-8")
        elif number == 2:
            value = v.decode("utf-8")
    return key, value


def _decode_field(data: bytes) -> Dict[str, Any]:
    out: Dict[str, Any] = {"field_id": "", "source_ids": [], "transform": None, "expression": None,
                           "result_type": "", "parameters": {}}
    for number, wire, v in _wire_fields(data):
        if number == 1:
            out["field_id"] = v.decode("utf-8")
        elif number == 2 and wire == 2:
            pos = 0
            while pos < len(v):
                source, pos = _varint_get(v, pos)
                out["source_ids"].append(source)
        elif number == 2:
            out["source_ids"].append(v)
        elif number == 3:
            out["transform"] = v.decode("utf-8")
        elif number == 4:
            out["expression"] = v.decode("utf-8")
        elif number == 5:
            out["result_type"] = v.decode("utf-8")
        elif number == 6:
            key, value = _decode_map_entry(v)
            out["parameters"][key] = value
    return out


def _decode_details(raw: bytes) -> Dict[str, Any]:
    num_shards = 0
    specs: List[Dict[str, Any]] = []
    maintained: List[str] = []
    defaults: Dict[str, str] = {}
    for number, _, v in _wire_fields(raw):
        if number == 2:
            num_shards = v
        elif number == 7:
            spec: Dict[str, Any] = {"spec_id": 0, "fields": []}
            for n, _, s in _wire_fields(v):
                if n == 1:
                    spec["spec_id"] = s
                elif n == 2:
                    spec["fields"].append(_decode_field(s))
            specs.append(spec)
        elif number == 8:
            maintained.append(v.decode("utf-8"))
        elif number == 11:
            key, value = _decode_map_entry(v)
            defaults[key] = value
    return {"maintained_indexes": maintained, "num_shards": num_shards, "sharding_specs": specs,
            "writer_config_defaults": defaults}


def index_details(dataset) -> Optional[Dict[str, Any]]:
    """``LanceDataset.mem_wal_index_details``: the MemWAL index as the dataset's version holds it."""
    with native():
        raw = _nanolance._ds_mem_wal_details(dataset._uri, dataset._version)
    return None if raw is None else _decode_details(bytes(raw))


# ---------------------------------------------------------------------------
# initialize_mem_wal
# ---------------------------------------------------------------------------

# ShardWriterConfig::default(), the values Lance's binding starts from once any writer default is given.
_WRITER_DEFAULTS = {
    "durable_write": True,
    "max_wal_buffer_size": 10 * 1024 * 1024,
    "max_wal_flush_interval_ms": 100,
    "max_wal_persist_retries": 3,
    "wal_persist_retry_base_delay_ms": 50,
    "max_memtable_size": 256 * 1024 * 1024,
    "max_memtable_rows": 100_000,
    "max_memtable_batches": 8_000,
    "manifest_scan_batch_size": 2,
    "max_unflushed_memtable_bytes": 1024 * 1024 * 1024,
    "backpressure_log_interval_ms": 30_000,
    "stats_log_interval_ms": 60_000,
    "enable_memtable": True,
}
_HNSW_DEFAULTS = {"num_edges": 20, "ef_construction": 150, "max_level": 7}  # HnswBuildParams::default()


def _text(value) -> str:
    return ("true" if value else "false") if isinstance(value, bool) else str(value)


def _writer_defaults(given: Mapping[str, Any], hnsw_params: Optional[Mapping[str, Any]]) -> Dict[str, str]:
    """The writer-config defaults to record: none unless some writer knob or HNSW parameter is given;
    then every ShardWriterConfig tunable, the given ones overriding Lance's defaults."""
    given = {k: v for k, v in given.items() if v is not None}
    if not given and not hnsw_params:
        return {}
    config = dict(_WRITER_DEFAULTS)
    config.update(given)
    out = {key: _text(value) for key, value in config.items()}
    for index, params in (hnsw_params or {}).items():
        for key in params:
            if key not in _HNSW_PARAMS:
                raise ValueError(f"unknown HNSW build param '{key}' for index '{index}'; expected one of "
                                 "'num_edges', 'ef_construction', 'max_level'")
        merged = dict(_HNSW_DEFAULTS)
        merged.update(params)
        for key in _HNSW_PARAMS:
            out[f"hnsw.{index}.{key}"] = str(merged[key])
    return out


def _fields_by_id(schema) -> Dict[int, Any]:
    out: Dict[int, Any] = {}

    def walk(fields) -> None:
        for f in fields:
            out[f.id()] = f
            walk(f.children())

    walk(schema.fields())
    return out


def _identity_result_type(t: pa.DataType) -> Optional[str]:
    names = [(pa.types.is_boolean, "boolean"), (pa.types.is_int8, "int8"), (pa.types.is_int16, "int16"),
             (pa.types.is_int32, "int32"), (pa.types.is_int64, "int64"), (pa.types.is_uint8, "uint8"),
             (pa.types.is_uint16, "uint16"), (pa.types.is_uint32, "uint32"), (pa.types.is_uint64, "uint64"),
             (pa.types.is_string, "utf8"), (pa.types.is_large_string, "large_utf8")]
    for check, name in names:
        if check(t):
            return name
    return None


def _bucket_supported(t: pa.DataType) -> bool:
    return (pa.types.is_boolean(t) or pa.types.is_integer(t) or pa.types.is_date32(t)
            or pa.types.is_time32(t) or pa.types.is_time64(t) or pa.types.is_timestamp(t)
            or pa.types.is_string(t) or pa.types.is_large_string(t)
            or (pa.types.is_floating(t) and t != pa.float16()))


def _source_field(dataset, column: str, operation: str):
    field_ = dataset.lance_schema.field(column)
    if field_ is None:
        raise OSError(f"Invalid user input: {operation}: column '{column}' not found on the dataset")
    return field_


def _sharding(dataset, bucket_column, num_buckets, identity_column, unsharded):
    """(the sharding spec or None, the shard count), validated against the dataset."""
    if unsharded:
        spec = {"spec_id": 1, "fields": [{"field_id": "bucket", "source_ids": [], "transform": "unsharded",
                                          "expression": None, "result_type": "int32", "parameters": {}}]}
        return spec, 1
    if identity_column is not None:
        source = _source_field(dataset, identity_column, "identity_sharding")
        t = source.to_arrow().type
        result = _identity_result_type(t)
        if result is None:
            raise OSError(f"Invalid user input: identity_sharding: column '{identity_column}' has type {t}, "
                          "which cannot be used as a shard key")
        spec = {"spec_id": 1, "fields": [{"field_id": "bucket", "source_ids": [source.id()],
                                          "transform": "identity", "expression": None, "result_type": result,
                                          "parameters": {}}]}
        return spec, 0
    if bucket_column is not None:
        if not 1 <= num_buckets <= _MAX_NUM_BUCKETS:
            raise OSError("Invalid user input: bucket_sharding: num_buckets must be in "
                          f"[1, {_MAX_NUM_BUCKETS}], got {num_buckets}")
        source = _source_field(dataset, bucket_column, "bucket_sharding")
        t = source.to_arrow().type
        if not _bucket_supported(t):
            raise OSError(f"Invalid user input: bucket_sharding: column '{bucket_column}' has type {t}, "
                          "which cannot be used as a shard key")
        spec = {"spec_id": 1, "fields": [{"field_id": "bucket", "source_ids": [source.id()],
                                          "transform": "bucket", "expression": None, "result_type": "int32",
                                          "parameters": {"num_buckets": str(num_buckets)}}]}
        return spec, num_buckets
    return None, 0


def _maintained(dataset, names: List[str]) -> None:
    """Each name is an index of the dataset that the MemWAL can maintain (BTree, Inverted or Vector)."""
    raw = dataset._raw_indices()
    fields = _fields_by_id(dataset.lance_schema)
    for name in names:
        matches = [i for i in raw if i["name"] == name]
        if not matches:
            raise OSError(f"Invalid user input: Index '{name}' from maintained_indexes not found on dataset")
        url = matches[0]["type_url"]
        if url == "/lance.table.BTreeIndexDetails" or url == "/lance.table.InvertedIndexDetails":
            continue
        if url.endswith("VectorIndexDetails"):
            column = fields.get(int(matches[0]["field_ids"][0])) if matches[0]["field_ids"] else None
            t = column.to_arrow().type if column is not None else None
            if t is None or not (pa.types.is_fixed_size_list(t) and t.value_type == pa.float32()):
                shown = "no such column" if t is None else f"item type {t.value_type}"
                raise OSError(f"Invalid user input: HNSW index '{name}' requires a FixedSizeList<Float32> "
                              f"column; column '{column.name() if column is not None else ''}' has {shown}")
            continue
        raise OSError(f"Invalid user input: index '{name}' has type {url}, which the MemWAL cannot maintain. "
                      "Supported: BTree, Inverted, Vector")


def initialize(dataset, *, maintained_indexes=None, bucket_column=None, num_buckets=None, identity_column=None,
               unsharded=False, durable_write=None, max_wal_buffer_size=None, max_wal_flush_interval_ms=None,
               max_memtable_size=None, max_memtable_rows=None, max_memtable_batches=None,
               max_unflushed_memtable_bytes=None, manifest_scan_batch_size=None,
               backpressure_log_interval_ms=None, stats_log_interval_ms=None, hnsw_params=None) -> None:
    """``LanceDataset.initialize_mem_wal``: commit the MemWAL index (see that method's documentation)."""
    bucket = bucket_column is not None or num_buckets is not None
    if bucket and (bucket_column is None or num_buckets is None):
        raise ValueError("bucket sharding requires both `bucket_column` and `num_buckets`")
    modes = int(bucket) + int(identity_column is not None) + int(bool(unsharded))
    if modes > 1:
        raise ValueError("at most one of bucket sharding, `identity_column`, or `unsharded` may be set")
    if num_buckets is not None and num_buckets < 0:
        raise OverflowError("out of range integral type conversion attempted")
    spec, num_shards = _sharding(dataset, bucket_column, num_buckets, identity_column, bool(unsharded))
    defaults = _writer_defaults({
        "durable_write": durable_write, "max_wal_buffer_size": max_wal_buffer_size,
        "max_wal_flush_interval_ms": max_wal_flush_interval_ms, "max_memtable_size": max_memtable_size,
        "max_memtable_rows": max_memtable_rows, "max_memtable_batches": max_memtable_batches,
        "max_unflushed_memtable_bytes": max_unflushed_memtable_bytes,
        "manifest_scan_batch_size": manifest_scan_batch_size,
        "backpressure_log_interval_ms": backpressure_log_interval_ms,
        "stats_log_interval_ms": stats_log_interval_ms,
    }, hnsw_params)
    if any(i["name"] == INDEX_NAME for i in dataset._raw_indices()):
        raise OSError("Invalid user input: MemWAL is already initialized on this dataset.")
    names = list(maintained_indexes or [])
    _maintained(dataset, names)
    details = _encode_details(num_shards, [spec] if spec is not None else [], names, defaults)
    with native():
        _nanolance._ds_initialize_mem_wal(dataset._uri, dataset._version, details)
    dataset._refresh_latest("CreateIndex")


# ---------------------------------------------------------------------------
# Not implemented (M2): the names exist, and using any of them raises NotSupportedError
# ---------------------------------------------------------------------------


class _NotImplementedInNanolance:
    _nl_name = ""

    def __init__(self, *args, **kwargs):
        raise unsupported(f"lance.mem_wal.{self._nl_name} (MemWAL writers, memtables and scanners)")


class CompactedSsTable(_NotImplementedInNanolance):
    _nl_name = "CompactedSsTable"


class ShardSnapshot(_NotImplementedInNanolance):
    _nl_name = "ShardSnapshot"


class ShardWriter(_NotImplementedInNanolance):
    _nl_name = "ShardWriter"


class ExecutionPlan(_NotImplementedInNanolance):
    _nl_name = "ExecutionPlan"


class LsmScanner(_NotImplementedInNanolance):
    _nl_name = "LsmScanner"

    @classmethod
    def from_snapshots(cls, *args, **kwargs):
        raise unsupported("lance.mem_wal.LsmScanner.from_snapshots (MemWAL writers, memtables and scanners)")


class LsmPointLookupPlanner(_NotImplementedInNanolance):
    _nl_name = "LsmPointLookupPlanner"


class LsmVectorSearchPlanner(_NotImplementedInNanolance):
    _nl_name = "LsmVectorSearchPlanner"
