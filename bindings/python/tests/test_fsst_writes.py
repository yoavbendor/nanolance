"""FSST-compressed string pages WRITTEN by nanolance (roadmap F2), read back by both readers.

pylance compresses high-cardinality strings with FSST; nanolance only read them, so its files were
1.5-4x larger on exactly that shape (unique ids, URLs, free text). It now trains one symbol table per
column and writes each page as FSST when that is at least 10% smaller, on the multi-chunk page
layout -- one table per page, 1024 values per chunk.

Every shape is checked the way the list matrix is: both readers, whole table and by range, through
sliced batches and several fragments, with and without zstd, and against pylance's own file size.
"""

from __future__ import annotations

import random

import pyarrow as pa
import pytest

import nanolance
from tests.support import require_pylance

N = 20_000
_WORDS = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "theta", "kappa", "lambda", "sigma", "omega"]


@pytest.fixture(scope="module")
def lance_mod():
    return require_pylance()


def _sentences(n, seed, nulls=False):
    rng = random.Random(seed)
    return [
        None if nulls and i % 9 == 0 else " ".join(rng.choice(_WORDS) for _ in range(rng.randint(3, 12))) + f" {i}"
        for i in range(n)
    ]


SHAPES = {
    "unique_ids": lambda: pa.array([f"s{i}-{i * 7919 % 100003}" for i in range(N)]),
    "urls": lambda: pa.array([f"https://example.com/users/{i * 7919 % 100003}/profile?tab={i % 13}" for i in range(N)]),
    "sentences": lambda: pa.array(_sentences(N, 1)),
    "sentences_nulls": lambda: pa.array(_sentences(N, 2, nulls=True)),
    "large_utf8": lambda: pa.array(_sentences(N, 3), pa.large_utf8()),
    "list_items": lambda: pa.array(
        [None if i % 11 == 0 else [f"item-{i}-{j}-{i * 31 % 977}" for j in range(i % 4)] for i in range(N)],
        pa.list_(pa.utf8()),
    ),
    "struct_field": lambda: pa.array(
        [None if i % 5 == 0 else {"t": f"user{i}@mail{i % 97}.example.org"} for i in range(N)]
    ),
    "map_values": lambda: pa.array(
        [[(f"k{j}", f"value {i} {j} {i * 13 % 1009}") for j in range(i % 3)] for i in range(N)],
        pa.map_(pa.utf8(), pa.utf8()),
    ),
}


def _data_bytes(path):
    return sum(f.stat().st_size for f in (path / "data").iterdir())


def _both_readers_agree(lance_mod, path, table):
    got = pa.table(nanolance.read_table(path))
    got.validate(full=True)
    assert got.to_pydict() == table.to_pydict(), "nanolance re-read mismatch"
    theirs = lance_mod.dataset(str(path)).to_table()
    assert theirs.schema == table.schema
    assert theirs.to_pydict() == table.to_pydict(), "stock Lance re-read mismatch"


@pytest.mark.parametrize("compression", [False, True], ids=["plain", "zstd"])
@pytest.mark.parametrize("shape", sorted(SHAPES))
def test_fsst_pages_round_trip_and_match_pylance_size(lance_mod, tmp_path, shape, compression):
    table = pa.table({"id": pa.array(range(N), pa.int64()), "c": SHAPES[shape]()})
    ours = tmp_path / "ours.lance"
    nanolance.write_table(table, ours, compression=compression)
    _both_readers_agree(lance_mod, ours, table)
    expected = table.slice(12_345, 77).to_pydict()
    assert pa.table(nanolance.read_table(ours, offset=12_345, length=77)).to_pydict() == expected
    assert lance_mod.dataset(str(ours)).to_table(offset=12_345, limit=77).to_pydict() == expected
    # The size guard: FSST on high-cardinality strings is what pylance does, and nanolance's files
    # used to be 1.5-4x larger here. Held within 5% of pylance's (they are usually smaller).
    theirs = tmp_path / "theirs.lance"
    lance_mod.write_dataset(table, str(theirs))
    assert _data_bytes(ours) <= 1.05 * _data_bytes(theirs), (_data_bytes(ours), _data_bytes(theirs))


@pytest.mark.parametrize("shape", ["sentences_nulls", "list_items"])
def test_fsst_pages_through_sliced_batches_fragments_and_deletions(lance_mod, tmp_path, shape):
    """Several fragments -- each trains its own table -- fed by sliced batches, then a deletion."""
    table = pa.table({"id": pa.array(range(N), pa.int64()), "c": SHAPES[shape]()})
    path = tmp_path / "frag.lance"
    with nanolance.LanceWriter(path, max_rows_per_fragment=7_000) as writer:
        for batch in table.to_batches(max_chunksize=1_337):
            writer.write_batch(batch)
    _both_readers_agree(lance_mod, path, table)
    expected = table.slice(6_990, 25).to_pydict()
    assert pa.table(nanolance.read_table(path, offset=6_990, length=25)).to_pydict() == expected
    lance_mod.dataset(str(path)).delete("id % 4 == 1")
    expected = lance_mod.dataset(str(path)).to_table()
    assert pa.table(nanolance.read_table(path)).to_pydict() == expected.to_pydict()


def test_fsst_skips_what_it_cannot_help(lance_mod, tmp_path):
    """Binary is never FSST'd (as in Lance); a small string column (under 32 KiB) is not either; and
    incompressible strings fall back to plain pages. All still round-trip."""
    rng = random.Random(9)
    table = pa.table(
        {
            "b": pa.array([f"https://example.com/{i}".encode() for i in range(2_000)], pa.binary()),
            "small": pa.array([f"s{i}" for i in range(2_000)]),
            "noise": pa.array([rng.randbytes(24).hex() for _ in range(2_000)]),
        }
    )
    path = tmp_path / "skip.lance"
    nanolance.write_table(table, path)
    _both_readers_agree(lance_mod, path, table)


def test_nullable_chunks_leave_room_for_their_levels(lance_mod, tmp_path):
    """Regression, found while measuring F2: a nullable plain string chunk could fill the 32,760
    value bytes a chunk's control word can describe AND carry 128 bytes of definition levels, so its
    footprint overflowed the word and stock Lance read past the page ("the offset + length of the
    sliced Buffer cannot exceed the existing length"). 1024 nullable ~33-byte values hit it. Binary,
    so FSST does not take the column."""
    values = [None if i % 9 == 0 else (f"value-{i:08d}-" + "x" * 18).encode() for i in range(5_000)]
    table = pa.table({"c": pa.array(values, pa.binary())})
    path = tmp_path / "nullable.lance"
    nanolance.write_table(table, path)
    _both_readers_agree(lance_mod, path, table)


def test_a_column_mixing_fsst_and_plain_pages(lance_mod, tmp_path):
    """Pages are cut at 32,768 rows and FSST is decided per page, so 32,868 unique strings make a
    full FSST page and a 100-row page too small to pay for its table -- written plain. Each page
    carries its own descriptor; the reader used to take the first page's for every page and
    decoded the plain one as FSST ("FSST code ... is not in the symbol table")."""
    n = 32_768 + 100
    table = pa.table({"c": pa.array([f"user-{i}-{i * 7919 % 100003}@mail.example.org" for i in range(n)])})
    path = tmp_path / "mixed.lance"
    nanolance.write_table(table, path)
    _both_readers_agree(lance_mod, path, table)
    tail = table.slice(n - 150, 150).to_pydict()
    assert pa.table(nanolance.read_table(path, offset=n - 150, length=150)).to_pydict() == tail
