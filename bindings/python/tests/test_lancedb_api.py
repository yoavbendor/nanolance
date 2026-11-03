"""nanolance.lancedb: LanceDB's Python API (lancedb 0.39) over nanolance.

The expected values are what lancedb 0.39.0 returns for the same calls (recorded by running each case on it): errors
and their messages, result objects, default limits and distances, nested-field schema errors (cognee's migration keys
off their text), write refusal on a checked-out version. Tables are plain Lance datasets: pylance reads them.
cognee's own LanceDB tests (108) and journeys (14) pass with nanolance installed as lancedb; see
docs/LANCEDB_COMPAT.md.
"""

from __future__ import annotations

import asyncio

import numpy as np
import pyarrow as pa
import pytest

import nanolance.lancedb as lancedb
from nanolance.lancedb.index import BTree, IvfFlat
from nanolance.lancedb.pydantic import LanceModel, Vector
from tests.support import require_pylance


class Item(LanceModel):
    id: str
    vector: Vector(3)
    n: int = 0


def run(coro):
    return asyncio.run(coro)


def test_lance_model_schema():
    schema = Item.to_arrow_schema()
    assert schema == pa.schema([pa.field("id", pa.string(), False),
                                pa.field("vector", pa.list_(pa.field("item", pa.float32()), 3), True),
                                pa.field("n", pa.int64(), False)])
    assert Item.field_names() == ["id", "vector", "n"]


def test_async_semantics_as_lancedb(tmp_path):
    async def main():
        db = await lancedb.connect_async(tmp_path / "db")
        t = await db.create_table("t", schema=Item)
        assert (t.name, await t.version(), await t.count_rows()) == ("t", 1, 0)
        for call, message in ((db.create_table("t", schema=Item), "Table 't' already exists"),
                              (db.open_table("nope"), "Table 'nope' was not found"),
                              (db.drop_table("nope"), "Table 'nope' was not found")):
            with pytest.raises(ValueError, match=message):
                await call
        assert await (await db.create_table("t", schema=Item, exist_ok=True)).version() == 1
        assert (await t.add([{"id": "a", "vector": [1, 0, 0], "n": 0}])).version == 2
        assert (await t.add([Item(id="b", vector=[0, 1, 0], n=2)])).version == 3
        reordered = pa.table({"vector": pa.array([[0, 0, 1]], pa.list_(pa.float32(), 3)), "id": ["c"], "n": [3]})
        assert (await t.add(reordered)).version == 4
        assert [r["id"] for r in await t.query().to_list()] == ["a", "b", "c"]
        assert await t.query().where("n > 0").select(["id"]).to_list() == [{"id": "b"}, {"id": "c"}]
        # default: squared L2, limit 10, every column then _distance
        rows = await t.vector_search([1, 0, 0]).to_list()
        assert [(r["id"], r["_distance"]) for r in rows] == [("a", 0.0), ("b", 2.0), ("c", 2.0)]
        assert list(rows[0]) == ["id", "vector", "n", "_distance"]
        assert await t.vector_search([1, 0, 0]).distance_type("cosine").select(["id"]).limit(2).to_list() == [
            {"id": "a", "_distance": 0.0}, {"id": "b", "_distance": 1.0}]
        assert await t.query().nearest_to([0, 1, 0]).limit(1).select(["id"]).to_list() == [
            {"id": "b", "_distance": 0.0}]
        assert await t.count_rows("n > 1") == 2
        m = await (t.merge_insert("id").when_matched_update_all().when_not_matched_insert_all()
                   .execute([{"id": "a", "vector": [1, 1, 0], "n": 9}, {"id": "d", "vector": [1, 1, 1], "n": 4}]))
        assert (m.version, m.num_updated_rows, m.num_inserted_rows, m.num_deleted_rows, m.num_rows) == (5, 1, 1, 0, 2)
        u = await t.update({"n": 7}, where="id = 'b'")
        assert (u.rows_updated, u.version) == (1, 6)
        u = await t.update(updates_sql={"n": "n + 1"})
        assert (u.rows_updated, u.version) == (4, 7)
        d = await t.delete("id = 'c'")
        assert (d.num_deleted_rows, d.version) == (1, 8)
        assert sorted((r["id"], r["n"]) for r in (await t.to_arrow()).to_pylist()) == [("a", 10), ("b", 8), ("d", 5)]
        assert [v["version"] for v in await t.list_versions()] == list(range(1, 9))
        assert await db.table_names() == ["t"] and (await db.list_tables()).tables == ["t"]
        await t.checkout(2)
        assert await t.count_rows() == 1
        with pytest.raises(ValueError, match="cannot be modified when a specific version is checked out"):
            await t.add([{"id": "z", "vector": [0, 0, 0], "n": 1}])
        await t.checkout_latest()
        assert (await t.version(), await t.count_rows()) == (8, 3)
        stats = await t.optimize()
        assert (stats.compaction.fragments_removed, stats.compaction.fragments_added) == (1, 1)  # as lancedb
        assert (await t.stats())["num_rows"] == 3
        await db.drop_table("t")
        assert await db.table_names() == []
        await db.drop_table("nope", ignore_missing=True)

    run(main())


def test_unknown_nested_field_errors_as_lancedb(tmp_path):
    """cognee migrates a collection's schema when an upsert fails with "not found in target schema"."""

    async def main():
        db = await lancedb.connect_async(tmp_path / "db")
        payload = pa.struct([pa.field("a", pa.int64())])
        t = await db.create_table("x", schema=pa.schema([pa.field("id", pa.string()), pa.field("p", payload)]))
        await t.add([{"id": "1", "p": {"a": 1}}])
        rows = [{"id": "2", "p": {"a": 2, "b": "new"}}]
        with pytest.raises(ValueError, match="Invalid input, field 'b' does not exist in table schema"):
            await t.add(rows)
        with pytest.raises(ValueError, match="Field 'b' not found in target schema"):
            await t.merge_insert("id").when_matched_update_all().when_not_matched_insert_all().execute(rows)
        with pytest.raises(ValueError, match="Field 'b' not found in target schema"):
            await t.merge_insert("id").when_not_matched_insert_all().execute(pa.Table.from_pylist(rows))
        assert await t.count_rows() == 1

    run(main())


def test_indexes_prefilter_and_bypass(tmp_path):
    rng = np.random.default_rng(3)
    vectors = rng.standard_normal((512, 16)).astype("float32")

    async def main():
        db = await lancedb.connect_async(tmp_path / "db")
        data = [{"id": str(i), "vector": vectors[i].tolist(), "g": i % 4} for i in range(512)]
        t = await db.create_table("v", data=data)
        assert (await t.schema()).field("vector").type == pa.list_(pa.field("item", pa.float32()), 16)
        q = vectors[7]
        exact = await t.vector_search(q).distance_type("cosine").where("g = 3").select(["id"]).limit(5).to_list()
        await t.create_index("vector", config=IvfFlat(distance_type="cosine", num_partitions=2))
        await t.create_index("g", config=BTree())
        assert sorted((i.index_type, i.columns[0]) for i in await t.list_indices()) == [
            ("BTree", "g"), ("IvfFlat", "vector")]
        bypass = await (t.vector_search(q).distance_type("cosine").where("g = 3").bypass_vector_index()
                        .select(["id"]).limit(5).to_list())
        assert [r["id"] for r in bypass] == [r["id"] for r in exact]
        probed = await (t.vector_search(q).distance_type("cosine").where("g = 3").nprobes(2).select(["id"])
                        .limit(5).to_list())
        assert [r["id"] for r in probed] == [r["id"] for r in exact]  # every partition probed: exact
        two = await t.vector_search(np.stack([vectors[1], vectors[2]])).limit(1).select(["id"]).to_list()
        assert [(r["id"], r["query_index"]) for r in two] == [("1", 0), ("2", 1)]

    run(main())


def test_sync_api_graphrag_shape(tmp_path):
    """What GraphRAG's LanceDB vector store does, on the sync API."""
    lance = require_pylance()
    db = lancedb.connect(str(tmp_path / "db"))
    rows = [{"id": f"d{i}", "text": f"doc {i}", "vector": [float(i), 1.0, 0.0], "attributes": "{}"} for i in range(20)]
    t = db.create_table("entities", data=rows, mode="overwrite")
    t.create_index(vector_column_name="vector", index_type="IVF_FLAT", num_partitions=2)
    hits = t.search([3.0, 1.0, 0.0]).where("id != 'd3'", prefilter=True).limit(3).to_list()
    assert [h["id"] for h in hits] == ["d2", "d4", "d1"] and len(hits) == 3
    assert t.search().where("id = 'd5'").to_list()[0]["text"] == "doc 5"
    t.update(where="id = 'd5'", values={"text": "doc five"})
    t.delete("id IN ('d0', 'd1')")
    assert t.count_rows() == 18 and db.open_table("entities").count_rows() == 18
    # A plain Lance dataset: pylance reads what the layer wrote.
    theirs = lance.dataset(t.uri)
    assert theirs.count_rows() == 18
    assert theirs.to_table(filter="id = 'd5'").column("text").to_pylist() == ["doc five"]


def test_install_as_lancedb_aliases_both_modules():
    """In a fresh process: swapping `lance` back and forth in this one would re-import pylance, which panics."""
    import subprocess
    import sys

    code = """
import sys
import nanolance.lancedb as nld
nld.install_as_lancedb()
import lance, lancedb
from lancedb.pydantic import LanceModel
assert lancedb is nld and lance.__name__ == "nanolance.lance"
assert LanceModel is sys.modules["nanolance.lancedb.pydantic"].LanceModel
assert sys.modules["lancedb.table"].AsyncTable is nld.AsyncTable
nld.uninstall_as_lancedb()
assert "lancedb" not in sys.modules
print("ok")
"""
    out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)
    assert out.stdout.strip() == "ok", out.stderr
