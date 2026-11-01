"""Data overlays: new values for some cells of a fragment, without rewriting its data files.

Lance's DataOverlay (format "data overlay files", unstable in Lance 12) commits a small data file that holds new
values for chosen rows of chosen columns; readers take the newest value of each cell. This example writes a
dataset, patches 3 cells of one column with an overlay, reads the patched values back with nanolance (and with
pylance when it is installed), and shows that the fragment's original data file was left as it was.

    python examples/data_overlay.py /tmp/overlay_demo.lance
"""

from __future__ import annotations

import os
import shutil
import sys

import pyarrow as pa

import nanolance.lance as nl


def write_overlay(path: str, ds, column: str, offsets: list[int], values: pa.Array, name: str = "patch.lance"):
    """Commit `values` over rows `offsets` of `column` in the first fragment of the dataset at `path`."""
    field_id = next(f.id() for f in ds.lance_schema.fields() if f.name() == column)
    with nl.file.LanceFileWriter(os.path.join(path, "data", name)) as w:
        w.write_batch(pa.table({column: values}))
    base = ds.get_fragments()[0].metadata.files[0]
    data_file = nl.fragment.DataFile(
        path=name, fields=[field_id], column_indices=[0],
        file_major_version=base.file_major_version, file_minor_version=base.file_minor_version,
        file_size_bytes=os.path.getsize(os.path.join(path, "data", name)))
    op = nl.LanceOperation.DataOverlay([nl.LanceOperation.DataOverlayGroup(
        ds.get_fragments()[0].fragment_id, [nl.LanceOperation.DataOverlayFile(data_file, offsets=offsets)])])
    return nl.LanceDataset.commit(path, op, read_version=ds.version)


def main(path: str) -> None:
    shutil.rmtree(path, ignore_errors=True)
    table = pa.table({"id": pa.array(range(10), pa.int32()),
                      "price": pa.array([i * 10 for i in range(10)], pa.int32())})
    ds = nl.write_dataset(table, path)
    data_files = sorted(os.listdir(os.path.join(path, "data")))

    # New prices for rows 2, 5 and 7 of the first fragment. Offsets are physical row offsets in the fragment.
    ds = write_overlay(path, ds, "price", [2, 5, 7], pa.array([999, 555, 777], pa.int32()))
    print("version", ds.version, "prices", nl.dataset(path).to_table().column("price").to_pylist())
    assert nl.dataset(path).to_table().column("price").to_pylist() == [0, 10, 999, 30, 40, 555, 60, 777, 80, 90]

    # The fragment's own data file is untouched: the overlay is one more small file beside it.
    added = sorted(set(os.listdir(os.path.join(path, "data"))) - set(data_files))
    print("files added by the overlay:", added)

    try:
        import lance  # pylance reads overlays only with this flag set (Lance 12: an unstable feature)
    except ImportError:
        return
    os.environ["LANCE_ENABLE_UNSTABLE_DATA_OVERLAY_FILES"] = "1"
    print("pylance reads:", lance.dataset(path).to_table().column("price").to_pylist())


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/nanolance_overlay_demo.lance")
