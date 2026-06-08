#!/usr/bin/env python3
"""Generate Lance v2.2 blob.v2 golden fixtures for nano_lance_writer.

Writes:
  <output-dir>/input.arrow       Arrow IPC stream (write-side blob v2 layout)
  <output-dir>/dataset.lance     Reference dataset from Lance Python
  <output-dir>/blob_v2_spec.json Machine-readable expectations for C++ tests

Use --check-only to verify Python dependencies (CTest exit 77 if missing).
Use --validate to assert the generated dataset matches the spec.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path


def _import_or_skip():
    try:
        import lance  # type: ignore
        import pyarrow as pa  # type: ignore
        import pyarrow.ipc as ipc  # type: ignore
        from lance.blob import Blob, blob_array, blob_field  # type: ignore
    except Exception as exc:  # pragma: no cover - environment dependent
        print(f"blob v2 golden generation skipped: {exc}", file=sys.stderr)
        return None
    return lance, pa, ipc, blob_field, blob_array, Blob


def _storage_child_specs(field) -> list[dict]:
    storage = field.type.storage_type if hasattr(field.type, "storage_type") else field.type
    specs = []
    for child in storage:
        specs.append(
            {
                "name": child.name,
                "arrow_type": str(child.type),
                "nullable": child.nullable,
            }
        )
    return specs


def _extension_name(field) -> str | None:
    if hasattr(field.type, "extension_name"):
        return field.type.extension_name
    metadata = field.metadata or {}
    value = metadata.get(b"ARROW:extension:name") or metadata.get("ARROW:extension:name")
    if isinstance(value, bytes):
        return value.decode("utf-8")
    return value


def _build_spec(rows, ds_schema) -> dict:
    payload = ds_schema.field("payload_ref")
    ext_name = _extension_name(payload)
    return {
        "data_storage_version": "2.2",
        "extension_name": ext_name or "lance.blob.v2",
        "supported_kinds": [3],
        "unsupported_kinds": ["inline", "packed", "dedicated"],
        "write_field": {
            "name": "payload_ref",
            "nullable": payload.nullable,
            "storage_children": _storage_child_specs(payload),
        },
        "companion_columns": ["packet_id"],
        "rows": int(rows.num_rows),
        "read_materialized_children": [
            "kind",
            "position",
            "size",
            "blob_id",
            "blob_uri",
        ],
        "sample_external_kind": 3,
        "external_only": True,
    }


def generate(output_dir: Path) -> int:
    modules = _import_or_skip()
    if modules is None:
        return 77

    lance, pa, ipc, blob_field, blob_array, Blob = modules
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)

    pcap_path = output_dir / "capture_001.pcapng"
    pcap_path.write_bytes(b"\x00" * 4096)

    schema = pa.schema(
        [
            pa.field("packet_id", pa.uint64(), nullable=False),
            blob_field("payload_ref"),
        ]
    )
    file_uri = pcap_path.as_uri()
    s3_uri = "s3://bucket/capture_001.pcapng"
    external_positions = [100, 200]
    external_sizes = [64, 128]

    rows = pa.table(
        {
            "packet_id": [1, 2],
            "payload_ref": blob_array(
                [
                    Blob.from_uri(file_uri, position=external_positions[0], size=external_sizes[0]),
                    Blob.from_uri(s3_uri, position=external_positions[1], size=external_sizes[1]),
                ]
            ),
        },
        schema=schema,
    )

    ipc_path = output_dir / "input.arrow"
    with ipc_path.open("wb") as sink:
        with ipc.new_stream(sink, schema) as writer:
            writer.write_table(rows)

    lance_dir = output_dir / "dataset.lance"
    lance.write_dataset(
        rows,
        str(lance_dir),
        mode="create",
        data_storage_version="2.2",
        allow_external_blob_outside_bases=True,
    )

    ds = lance.dataset(str(lance_dir))
    read_table = ds.to_table()
    payload_type = read_table.schema.field("payload_ref").type
    if not pa.types.is_struct(payload_type):
        print("validation failed: payload_ref is not a struct when materialized", file=sys.stderr)
        return 1
    materialized = [payload_type.field(i).name for i in range(payload_type.num_fields)]

    spec = _build_spec(rows, ds.schema)
    spec["materialized_children"] = materialized
    spec["external_rows"] = [
        {
            "index": 0,
            "uri": file_uri,
            "position": external_positions[0],
            "size": external_sizes[0],
            "kind": 3,
            "blob_id": 0,
            "take_blobs_readable": True,
        },
        {
            "index": 1,
            "uri": s3_uri,
            "position": external_positions[1],
            "size": external_sizes[1],
            "kind": 3,
            "blob_id": 0,
            "take_blobs_readable": False,
        },
    ]
    spec_path = output_dir / "blob_v2_spec.json"
    spec_path.write_text(json.dumps(spec, indent=2) + "\n", encoding="utf-8")

    print(f"generated blob v2 golden fixtures in {output_dir}")
    return 0


def validate(output_dir: Path) -> int:
    modules = _import_or_skip()
    if modules is None:
        return 77

    lance, pa, ipc, _blob_field, _blob_array, _Blob = modules
    spec_path = output_dir / "blob_v2_spec.json"
    ipc_path = output_dir / "input.arrow"
    lance_dir = output_dir / "dataset.lance"
    for path in (spec_path, ipc_path, lance_dir):
        if not path.exists():
            print(f"validation failed: missing {path}", file=sys.stderr)
            return 1

    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    with ipc_path.open("rb") as source:
        reader = ipc.open_stream(source)
        ipc_schema = reader.schema
    payload_ipc = ipc_schema.field("payload_ref")
    if _extension_name(payload_ipc) != spec["extension_name"]:
        print(
            "validation failed: IPC extension name mismatch",
            _extension_name(payload_ipc),
            spec["extension_name"],
            file=sys.stderr,
        )
        return 1

    ds = lance.dataset(str(lance_dir))
    if _extension_name(ds.schema.field("payload_ref")) != spec["extension_name"]:
        print("validation failed: dataset extension name mismatch", file=sys.stderr)
        return 1

    table = ds.to_table()
    names = [table.schema.field("payload_ref").type.field(i).name for i in range(table.schema.field("payload_ref").type.num_fields)]
    expected = spec.get("materialized_children") or spec.get("read_materialized_children")
    if names != expected:
        print(f"validation failed: materialized children {names} != {expected}", file=sys.stderr)
        return 1

    payload = table.column("payload_ref").combine_chunks()
    kinds = payload.field("kind").to_pylist()
    if any(kind != spec.get("sample_external_kind", 3) for kind in kinds):
        print(f"validation failed: unexpected external kind values {kinds}", file=sys.stderr)
        return 1

    positions = payload.field("position").to_pylist()
    sizes = payload.field("size").to_pylist()
    blob_ids = payload.field("blob_id").to_pylist()
    blob_uris = payload.field("blob_uri").to_pylist()
    for row in spec.get("external_rows", []):
        idx = row["index"]
        if positions[idx] != row["position"] or sizes[idx] != row["size"]:
            print(
                f"validation failed: descriptor range mismatch at {idx}: "
                f"{positions[idx]}, {sizes[idx]} != {row['position']}, {row['size']}",
                file=sys.stderr,
            )
            return 1
        if blob_ids[idx] != row["blob_id"] or blob_uris[idx] != row["uri"]:
            print(
                f"validation failed: descriptor reference mismatch at {idx}: "
                f"{blob_ids[idx]}, {blob_uris[idx]} != {row['blob_id']}, {row['uri']}",
                file=sys.stderr,
            )
            return 1

    print(f"blob v2 golden fixtures validated under {output_dir}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).with_name("golden") / "blob_v2",
    )
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--validate", action="store_true")
    args = parser.parse_args(argv)

    modules = _import_or_skip()
    if args.check_only:
        if modules is None:
            return 77
        print("Python Lance and PyArrow are available for blob v2 golden fixtures")
        return 0
    if modules is None:
        return 77
    if args.validate:
        return validate(args.output_dir)
    return generate(args.output_dir)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
