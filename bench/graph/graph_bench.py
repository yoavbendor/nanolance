#!/usr/bin/env python3
"""Graph-workload benchmark: nanolance vs pylance (Lance) vs Ladybug.

Generates one seeded synthetic graph, writes it once as Lance tables with pylance,
builds the adapter's scalar indexes on that single copy, loads the same graph into
Ladybug with COPY FROM parquet, then times the query shapes of cognee's nanolance
graph adapter (cognee_nanograph/adapter.py) on each engine.

Every query sample is also checked: nanolance must return the same rows as pylance,
and Ladybug the same ids. Any difference prints a MISMATCH line and exits non-zero.

See bench/graph/README.md for the query definitions and measured results.
"""

from __future__ import annotations

import argparse
import datetime as dt
import gc
import importlib.metadata
import json
import os
import platform
import shutil
import subprocess
import tempfile
import sys
import time
import uuid

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

import lance as pylance  # reference engine: pylance reading/writing the shared files
from nanolance import lance as nlance  # engine under test: nanolance reading the same files
import ladybug

# ---------------------------------------------------------------------------
# Schema: mirrors cognee_nanograph/tables.py (kept inline so the script is standalone).
# ---------------------------------------------------------------------------

_ZSTD = {"lance-encoding:compression": "zstd"}
_TS = pa.timestamp("us", tz="UTC")
_STR_LIST = pa.list_(pa.string())
PROVENANCE = ("source_ref_keys", "source_dataset_ids", "source_run_ids", "source_run_refs")

NODE_SCHEMA = pa.schema(
    [
        pa.field("id", pa.string()),
        pa.field("name", pa.string()),
        pa.field("type", pa.string()),
        pa.field("created_at", _TS),
        pa.field("updated_at", _TS),
        pa.field("properties", pa.string(), metadata=_ZSTD),
        *(pa.field(c, _STR_LIST) for c in PROVENANCE),
    ]
)
EDGE_SCHEMA = pa.schema(
    [
        pa.field("source_id", pa.string()),
        pa.field("target_id", pa.string()),
        pa.field("relationship_name", pa.string()),
        pa.field("created_at", _TS),
        pa.field("updated_at", _TS),
        pa.field("properties", pa.string(), metadata=_ZSTD),
        *(pa.field(c, _STR_LIST) for c in PROVENANCE),
        pa.field("edge_key", pa.string()),
    ]
)
NODE_INDEXES = (("id", "BTREE"), ("type", "BITMAP"))
EDGE_INDEXES = (
    ("source_id", "BTREE"),
    ("target_id", "BTREE"),
    ("edge_key", "BTREE"),
    ("relationship_name", "BITMAP"),
)

NODE_TYPES = [
    "Entity", "EntityType", "DocumentChunk", "TextDocument", "TextSummary",
    "Timestamp", "Event", "Person", "Organization", "Location",
    "Concept", "Product", "Topic", "Quote", "Dataset",
    "Code", "Image", "Table", "Section", "Claim",
]  # fmt: skip
REL_NAMES = [
    "is_a", "is_part_of", "contains", "mentions", "related_to",
    "made_by", "located_in", "works_for", "founded", "owns",
    "depends_on", "cites", "summarizes", "precedes", "follows",
    "derived_from", "describes", "refers_to", "has_property", "belongs_to_set",
    "authored", "reviewed_by", "tagged_with", "links_to", "supersedes",
    "implements", "uses", "produced", "observed_in", "similar_to",
]  # fmt: skip
assert len(NODE_TYPES) == 20 and len(REL_NAMES) == 30

# Endpoint popularity ~ rank^-ZIPF_S. 0.9 over 250k nodes gives the top node ~3% of
# endpoints, so the top-20 hubs have roughly 10k-30k edges each at the default size.
ZIPF_S = 0.9
TYPE_ZIPF_S = 0.8
HUB_K = 20
IN_CHUNK = 500  # adapter's _IN_CHUNK: IN lists sent to the Lance filter in slices


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def lit(value) -> str:
    """SQL string literal for a Lance filter."""
    return "'" + str(value).replace("'", "''") + "'"


def zipf_weights(n: int, s: float) -> np.ndarray:
    w = np.arange(1, n + 1, dtype=np.float64) ** -s
    return w / w.sum()


def timestamps(rng, n: int, span_days: int, max_update_days: int):
    base = int(dt.datetime(2025, 1, 1, tzinfo=dt.timezone.utc).timestamp() * 1_000_000)
    created = base + rng.integers(0, span_days * 86_400 * 1_000_000, size=n, dtype=np.int64)
    updated = created + rng.integers(0, max_update_days * 86_400 * 1_000_000, size=n, dtype=np.int64)
    return (
        pa.array(created.astype("datetime64[us]")).cast(_TS),
        pa.array(updated.astype("datetime64[us]")).cast(_TS),
    )


def json_payloads(rng, pool: str, n: int, lo: int, hi: int) -> list[str]:
    lengths = rng.integers(lo, hi, size=n).tolist()
    starts = rng.integers(0, len(pool) - hi - 1, size=n).tolist()
    scores = rng.random(n).round(4).tolist()
    return [
        json.dumps({"text": pool[s : s + l], "score": sc}, separators=(",", ":"))
        for s, l, sc in zip(starts, lengths, scores)
    ]


def provenance_lists(rng, n: int, pools: dict[str, list[str]]) -> dict[str, pa.Array]:
    """One-element list<string> per provenance column (cognee writes 1+ refs per row)."""
    offsets = pa.array(np.arange(n + 1, dtype=np.int32))
    out = {}
    for col in PROVENANCE:
        vals = pools[col]
        pick = rng.integers(0, len(vals), size=n)
        flat = pa.array(np.asarray(vals, dtype=object)[pick].tolist(), type=pa.string())
        out[col] = pa.ListArray.from_arrays(offsets, flat)
    return out


def make_text_pool(rng, chars: int = 8000) -> tuple[str, list[str]]:
    letters = list("abcdefghilmnoprstuv")
    vocab = [
        "".join(rng.choice(letters, size=int(rng.integers(3, 9))).tolist()) for _ in range(400)
    ]
    words, total = [], 0
    while total < chars:
        w = vocab[int(rng.integers(len(vocab)))]
        words.append(w)
        total += len(w) + 1
    return " ".join(words), vocab


def make_provenance_pools() -> dict[str, list[str]]:
    return {
        "source_ref_keys": [f"ref:{k:05d}" for k in range(2000)],
        "source_dataset_ids": [str(uuid.uuid5(uuid.NAMESPACE_URL, f"ds-{k}")) for k in range(8)],
        "source_run_ids": [str(uuid.uuid5(uuid.NAMESPACE_URL, f"run-{k}")) for k in range(20)],
        "source_run_refs": [f"run-ref-{k:03d}" for k in range(200)],
    }


# ---------------------------------------------------------------------------
# Synthetic graph
# ---------------------------------------------------------------------------


def generate_graph(rng, n_nodes: int, n_edges: int):
    pool, vocab = make_text_pool(rng)
    prov_pools = make_provenance_pools()

    # Nodes
    raw = rng.bytes(16 * n_nodes)
    ids = [str(uuid.UUID(bytes=raw[i : i + 16], version=4)) for i in range(0, 16 * n_nodes, 16)]
    assert len(set(ids)) == n_nodes
    type_idx = rng.choice(
        len(NODE_TYPES), size=n_nodes, p=zipf_weights(len(NODE_TYPES), TYPE_ZIPF_S)
    )
    node_type = np.array(NODE_TYPES, dtype=object)[type_idx]
    wa = rng.integers(0, len(vocab), size=n_nodes).tolist()
    wb = rng.integers(0, len(vocab), size=n_nodes).tolist()
    names = [f"{vocab[a]} {vocab[b]} {i:07d}" for i, (a, b) in enumerate(zip(wa, wb))]
    created, updated = timestamps(rng, n_nodes, 300, 30)
    node_props = json_payloads(rng, pool, n_nodes, 170, 470)
    node_prov = provenance_lists(rng, n_nodes, prov_pools)
    nodes = pa.Table.from_arrays(
        [
            pa.array(ids),
            pa.array(names),
            pa.array(node_type.tolist()),
            created,
            updated,
            pa.array(node_props),
            *[node_prov[c] for c in PROVENANCE],
        ],
        schema=NODE_SCHEMA,
    )

    # Edges: power-law endpoints, deduplicated on (source, relationship, target)
    n_rel = len(REL_NAMES)
    rel_p = zipf_weights(n_rel, 1.0)
    p_end = zipf_weights(n_nodes, ZIPF_S)
    perm_s = rng.permutation(n_nodes)
    perm_t = rng.permutation(n_nodes)
    keys = np.empty(0, dtype=np.int64)
    src = np.empty(0, dtype=np.int64)
    tgt = np.empty(0, dtype=np.int64)
    rel = np.empty(0, dtype=np.int64)
    while True:
        need = n_edges - len(np.unique(keys))
        if need <= 0 and len(keys):
            break
        k = int(max(need, 1) * 1.25) + 1000
        s = perm_s[rng.choice(n_nodes, size=k, p=p_end)]
        t = perm_t[rng.choice(n_nodes, size=k, p=p_end)]
        r = rng.choice(n_rel, size=k, p=rel_p)
        ok = s != t
        s, t, r = s[ok], t[ok], r[ok]
        src = np.concatenate([src, s])
        tgt = np.concatenate([tgt, t])
        rel = np.concatenate([rel, r])
        keys = (src * n_nodes + tgt) * n_rel + rel
        _, first = np.unique(keys, return_index=True)
        first = np.sort(first)[:n_edges]
        keys, src, tgt, rel = keys[first], src[first], tgt[first], rel[first]
        if len(keys) >= n_edges:
            break
    m = len(src)

    e_created, e_updated = timestamps(rng, m, 300, 30)
    e_props = json_payloads(rng, pool, m, 20, 100)
    e_prov = provenance_lists(rng, m, prov_pools)
    rel_names = np.array(REL_NAMES, dtype=object)[rel]
    src_ids = np.array(ids, dtype=object)[src]
    tgt_ids = np.array(ids, dtype=object)[tgt]
    edge_keys = [f"{a}|{b}|{c}" for a, b, c in zip(src_ids.tolist(), rel_names.tolist(), tgt_ids.tolist())]
    edges = pa.Table.from_arrays(
        [
            pa.array(src_ids.tolist()),
            pa.array(tgt_ids.tolist()),
            pa.array(rel_names.tolist()),
            e_created,
            e_updated,
            pa.array(e_props),
            *[e_prov[c] for c in PROVENANCE],
            pa.array(edge_keys),
        ],
        schema=EDGE_SCHEMA,
    )

    degree = np.bincount(src, minlength=n_nodes) + np.bincount(tgt, minlength=n_nodes)
    return nodes, edges, {"src": src, "tgt": tgt, "degree": degree, "ids": ids,
                          "names": names, "node_type": node_type}  # fmt: skip


def create_lance_indexes(path: str, indexes) -> dict[str, float]:
    timings = {}
    ds = pylance.dataset(path)
    for column, kind in indexes:
        t0 = time.perf_counter()
        ds.create_scalar_index(column, kind)
        timings[column] = round(time.perf_counter() - t0, 3)
        ds = pylance.dataset(path)
    return timings


def naive_utc(table: pa.Table) -> pa.Table:
    """Ladybug parquet import: timestamps as naive UTC microseconds."""
    for i, field in enumerate(table.schema):
        if pa.types.is_timestamp(field.type) and field.type.tz is not None:
            table = table.set_column(i, field.name, table.column(i).cast(pa.timestamp("us")))
    return table


# ---------------------------------------------------------------------------
# Query sample selection (seeded, fixed for all engines)
# ---------------------------------------------------------------------------


def pick_samples(rng, graph: dict, n_nodes: int, sample: int) -> dict[str, list]:
    ids, names, degree, node_type = graph["ids"], graph["names"], graph["degree"], graph["node_type"]
    hub_idx = np.argsort(-degree, kind="stable")[:HUB_K]
    is_hub = np.zeros(n_nodes, dtype=bool)
    is_hub[hub_idx] = True
    ordinary_pool = np.flatnonzero((degree > 0) & ~is_hub)
    ordinary = rng.choice(ordinary_pool, size=sample, replace=False)
    uniform = lambda k: rng.choice(n_nodes, size=k, replace=False)  # noqa: E731
    return {
        "ordinary_ids": [ids[i] for i in ordinary.tolist()],
        "hub_ids": [ids[i] for i in hub_idx.tolist()],
        "hub_degrees": [int(degree[i]) for i in hub_idx.tolist()],
        "batch50_ids": [[ids[i] for i in uniform(50).tolist()] for _ in range(sample)],
        "batch500_ids": [[ids[i] for i in uniform(500).tolist()] for _ in range(sample)],
        "batch50_names": [[names[i] for i in uniform(50).tolist()] for _ in range(sample)],
        "batch500_names": [[names[i] for i in uniform(500).tolist()] for _ in range(sample)],
        "types": [str(node_type[i]) for i in rng.choice(n_nodes, size=sample).tolist()],
        "rel3": [
            [REL_NAMES[j] for j in rng.choice(len(REL_NAMES), size=3, replace=False).tolist()]
            for _ in range(sample)
        ],
    }


# ---------------------------------------------------------------------------
# Engines. Each returns Arrow tables (or ints for counts) so that the timed work
# is comparable: scan/filter plus materialising the result in Arrow.
# ---------------------------------------------------------------------------

EDGE_COLS = ["source_id", "target_id", "relationship_name"]
NODE_READ = ["id", "name", "type", "properties"]


class LanceEngine:
    """Engine over the shared Lance dataset: pylance or nanolance `lance` module."""

    def __init__(self, label: str, module, root: str):
        self.label = label
        self.nodes = module.dataset(os.path.join(root, "nodes.lance"))
        self.edges = module.dataset(os.path.join(root, "edges.lance"))

    def _in(self, ds, column: str, values: list[str], columns: list[str]) -> pa.Table:
        tables = []
        for i in range(0, len(values), IN_CHUNK):
            chunk = values[i : i + IN_CHUNK]
            flt = f"{column} IN ({', '.join(lit(v) for v in chunk)})"
            tables.append(ds.to_table(columns=columns, filter=flt))
        if not tables:
            return ds.schema.empty_table().select(columns)
        return pa.concat_tables(tables)

    def one_hop(self, node_id: str) -> dict:
        edges = self.edges.to_table(
            columns=EDGE_COLS,
            filter=f"source_id = {lit(node_id)} OR target_id = {lit(node_id)}",
        )
        neighbours = sorted(
            {
                t if s == node_id else s
                for s, t in zip(edges["source_id"].to_pylist(), edges["target_id"].to_pylist())
            }
        )
        nodes = self._in(self.nodes, "id", neighbours, NODE_READ)
        return {"nodes": nodes, "edges": edges}

    def edges_in(self, ids: list[str]) -> pa.Table:
        return self._in(self.edges, "source_id", ids, EDGE_COLS)

    def nodes_in(self, ids: list[str]) -> pa.Table:
        return self._in(self.nodes, "id", ids, NODE_READ)

    def unindexed_in(self, names: list[str]) -> pa.Table:
        return self._in(self.nodes, "name", names, ["id", "name", "type"])

    def type_count(self, node_type: str) -> int:
        return self.nodes.count_rows(filter=f"type = {lit(node_type)}")

    def rel_count(self, rels: list[str]) -> int:
        flt = f"relationship_name IN ({', '.join(lit(r) for r in rels)})"
        return self.edges.count_rows(filter=flt)


class CachedKeysNanolance:
    """The adapter's current one-hop path: key columns read once, then is_in + take.

    Mirrors adapter._t_key_column / _t_take_by_key / _t_edges_touching. The key
    columns are cached per engine instance (read-only data, so one version).
    """

    def __init__(self, nodes_ds, edges_ds):
        self.nodes = nodes_ds
        self.edges = edges_ds
        self.src = edges_ds.to_table(columns=["source_id"])["source_id"].combine_chunks()
        self.tgt = edges_ds.to_table(columns=["target_id"])["target_id"].combine_chunks()
        self.node_ids = nodes_ds.to_table(columns=["id"])["id"].combine_chunks()

    def one_hop(self, node_id: str) -> dict:
        members = pa.array([node_id])
        mask = pc.or_(pc.is_in(self.src, members), pc.is_in(self.tgt, members))
        positions = pc.indices_nonzero(mask).to_pylist()
        if positions:
            edges = self.edges.take(positions, columns=EDGE_COLS)
        else:
            edges = self.edges.schema.empty_table().select(EDGE_COLS)
        neighbours = sorted(
            {
                t if s == node_id else s
                for s, t in zip(edges["source_id"].to_pylist(), edges["target_id"].to_pylist())
            }
        )
        if neighbours:
            n_pos = pc.indices_nonzero(pc.is_in(self.node_ids, pa.array(neighbours))).to_pylist()
            nodes = self.nodes.take(n_pos, columns=NODE_READ)
        else:
            nodes = self.nodes.schema.empty_table().select(NODE_READ)
        return {"nodes": nodes, "edges": edges}


class LadybugEngine:
    label = "ladybug"

    def __init__(self, path: str):
        self.db = ladybug.Database(path)
        self.conn = ladybug.Connection(self.db)

    def _q(self, cypher: str, params: dict | None = None):
        return self.conn.execute(cypher, params or {}).get_as_arrow()

    def one_hop(self, node_id: str) -> dict:
        nodes = self._q(
            "MATCH (a:Node {id: $id})-[r:Edge]-(b:Node) "
            "RETURN b.id AS id, b.name AS name, b.type AS type, "
            "b.properties AS properties, r.relationship_name AS relationship_name",
            {"id": node_id},
        )
        return {"nodes": nodes}

    def edges_in(self, ids: list[str]) -> pa.Table:
        return self._q(
            "MATCH (a:Node)-[r:Edge]->(b:Node) WHERE a.id IN $ids "
            "RETURN a.id AS source_id, b.id AS target_id, "
            "r.relationship_name AS relationship_name",
            {"ids": ids},
        )

    def nodes_in(self, ids: list[str]) -> pa.Table:
        return self._q(
            "MATCH (n:Node) WHERE n.id IN $ids RETURN n.id AS id, n.name AS name, "
            "n.type AS type, n.properties AS properties",
            {"ids": ids},
        )

    def unindexed_in(self, names: list[str]) -> pa.Table:
        return self._q(
            "MATCH (n:Node) WHERE n.name IN $names RETURN n.id AS id, n.name AS name, "
            "n.type AS type",
            {"names": names},
        )

    def type_count(self, node_type: str) -> int:
        t = self._q(
            "MATCH (n:Node) WHERE n.type = $t RETURN count(n) AS c", {"t": node_type}
        )
        return int(t.column("c")[0].as_py())

    def rel_count(self, rels: list[str]) -> int:
        t = self._q(
            "MATCH ()-[r:Edge]->() WHERE r.relationship_name IN $rels RETURN count(r) AS c",
            {"rels": rels},
        )
        return int(t.column("c")[0].as_py())


LADYBUG_DDL = [
    "CREATE NODE TABLE Node(id STRING PRIMARY KEY, name STRING, type STRING, "
    "created_at TIMESTAMP, updated_at TIMESTAMP, properties STRING, "
    "source_ref_keys STRING[], source_dataset_ids STRING[], source_run_ids STRING[], "
    "source_run_refs STRING[])",
    "CREATE REL TABLE Edge(FROM Node TO Node, relationship_name STRING, "
    "created_at TIMESTAMP, updated_at TIMESTAMP, properties STRING, "
    "source_ref_keys STRING[], source_dataset_ids STRING[], source_run_ids STRING[], "
    "source_run_refs STRING[], edge_key STRING)",
]


def load_ladybug(db_path: str, workdir: str, nodes: pa.Table, edges: pa.Table) -> tuple[LadybugEngine, dict]:
    nodes_pq = os.path.join(workdir, "ladybug_nodes.parquet")
    edges_pq = os.path.join(workdir, "ladybug_edges.parquet")
    pq.write_table(naive_utc(nodes), nodes_pq)
    pq.write_table(naive_utc(edges), edges_pq)
    parquet_bytes = os.path.getsize(nodes_pq) + os.path.getsize(edges_pq)
    t0 = time.perf_counter()
    eng = LadybugEngine(db_path)
    for ddl in LADYBUG_DDL:
        eng.conn.execute(ddl)
    eng.conn.execute(f"COPY Node FROM {lit(nodes_pq)}")
    eng.conn.execute(f"COPY Edge FROM {lit(edges_pq)}")
    load_s = time.perf_counter() - t0
    os.remove(nodes_pq)
    os.remove(edges_pq)
    return eng, {"load_seconds": round(load_s, 3), "parquet_bytes": parquet_bytes}


# ---------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------


def exact_key(table: pa.Table) -> list[dict]:
    return table.sort_by([(c, "ascending") for c in table.column_names]).to_pylist()


def payload_rows(payload) -> int:
    if isinstance(payload, int):
        return payload
    if isinstance(payload, dict):
        return payload["nodes"].num_rows
    return payload.num_rows


def ids_key(payload):
    """Comparable across engines: node ids as a set, edge triples as a sorted list."""
    if isinstance(payload, int):
        return payload
    if isinstance(payload, dict):
        payload = payload["nodes"]
    if "id" in payload.column_names:
        return sorted(set(payload["id"].to_pylist()))
    return sorted(
        zip(
            payload["source_id"].to_pylist(),
            payload["target_id"].to_pylist(),
            payload["relationship_name"].to_pylist(),
        )
    )


def exact_payload(payload):
    if isinstance(payload, int):
        return payload
    if isinstance(payload, dict):
        return {k: exact_payload(v) for k, v in payload.items()}
    return exact_key(payload)


def describe_diff(a, b) -> str:
    if isinstance(a, int) or isinstance(b, int):
        return f"count {a} vs {b}"
    sa, sb = ids_key(a), ids_key(b)
    if len(sa) != len(sb):
        return f"row count {len(sa)} vs {len(sb)}"
    diff = [x for x, y in zip(sa, sb) if x != y][:3]
    return f"first differing ids {diff}"


# ---------------------------------------------------------------------------
# Timing
# ---------------------------------------------------------------------------


def time_calls(fn, items: list, reps: int, warm: int) -> list[float]:
    for item in items[:warm]:
        fn(item)
    samples = []
    for _ in range(reps):
        for item in items:
            t0 = time.perf_counter()
            fn(item)
            samples.append((time.perf_counter() - t0) * 1000.0)
    return samples


def stats(samples: list[float]) -> dict:
    arr = np.asarray(samples)
    return {
        "n": int(arr.size),
        "p50_ms": round(float(np.percentile(arr, 50)), 3),
        "p95_ms": round(float(np.percentile(arr, 95)), 3),
        "mean_ms": round(float(arr.mean()), 3),
    }


# ---------------------------------------------------------------------------
# Machine and versions
# ---------------------------------------------------------------------------


def machine_info() -> dict:
    cpu = "unknown"
    try:
        with open("/proc/cpuinfo") as fh:
            for line in fh:
                if line.startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    mem_gb = None
    try:
        with open("/proc/meminfo") as fh:
            for line in fh:
                if line.startswith("MemTotal"):
                    mem_gb = round(int(line.split()[1]) / 1024 / 1024, 1)
                    break
    except OSError:
        pass
    return {
        "cpu_model": cpu,
        "logical_cores": os.cpu_count(),
        "memory_gb": mem_gb,
        "platform": platform.platform(),
        "python": platform.python_version(),
    }


def pkg_version(name: str) -> str:
    try:
        return importlib.metadata.version(name)
    except importlib.metadata.PackageNotFoundError:
        return "unknown"


def nanolance_commit() -> str | None:
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(nlance.__file__))))
    try:
        out = subprocess.run(
            ["git", "-C", root, "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=10,
        )  # fmt: skip
        return out.stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def dir_bytes(path: str) -> int:
    if os.path.isfile(path):
        return os.path.getsize(path)
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            total += os.path.getsize(os.path.join(root, f))
    return total


def index_plan(path: str, filt: str, columns: list[str], module) -> str:
    try:
        plan = module.dataset(path).scanner(filter=filt, columns=columns).explain_plan(verbose=False)
        return "ScalarIndexQuery" if "ScalarIndexQuery" in plan else "no index used"
    except Exception as exc:  # plan introspection is informational only
        return f"unavailable ({type(exc).__name__})"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def print_table(rows: list[dict]) -> None:
    head = (
        f"{'query':<30}{'nanolance p50/p95/mean (ms)':>36}"
        f"{'pylance p50/p95/mean (ms)':>36}{'ladybug p50/p95/mean (ms)':>36}{'avg rows':>12}"
    )
    print(head)
    print("-" * len(head))

    def cell(stat: dict | None) -> str:
        if not stat:
            return "-".rjust(34)
        return f"{stat['p50_ms']:>9.2f} {stat['p95_ms']:>9.2f} {stat['mean_ms']:>9.2f}".rjust(34)

    for r in rows:
        print(
            f"{r['query']:<30}{cell(r.get('nanolance')):>36}{cell(r.get('pylance')):>36}"
            f"{cell(r.get('ladybug')):>36}{r['avg_rows']:>12.1f}"
        )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nodes", type=int, default=None, help="node count (default 250000, or 20000 with --small)")
    ap.add_argument("--edges", type=int, default=None, help="edge count (default 1000000, or 80000 with --small)")
    ap.add_argument("--small", action="store_true", help="quick preset: 20000 nodes / 80000 edges")
    ap.add_argument("--seed", type=int, default=20260101)
    ap.add_argument("--sample", type=int, default=20, help="query ids per query shape")
    ap.add_argument("--reps", type=int, default=5, help="timed passes over each sample")
    ap.add_argument("--warmup", type=int, default=3, help="untimed calls per engine per query")
    ap.add_argument("--workdir", default=os.path.join(tempfile.gettempdir(), "graphbench"))
    ap.add_argument("--json", dest="json_out", default=None, help="write machine-readable results here")
    ap.add_argument("--keep-data", action="store_true", help="keep the generated data after the run")
    args = ap.parse_args()

    n_nodes = args.nodes or (20_000 if args.small else 250_000)
    n_edges = args.edges or (80_000 if args.small else 1_000_000)
    run_dir = os.path.join(args.workdir, f"run-{n_nodes}x{n_edges}-seed{args.seed}")
    if os.path.exists(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)
    lance_root = os.path.join(run_dir, "lance")
    os.makedirs(lance_root)
    ladybug_path = os.path.join(run_dir, "ladybug.db")

    print(f"[gen] {n_nodes} nodes, {n_edges} edges, seed {args.seed}", flush=True)
    rng = np.random.default_rng(args.seed)
    t0 = time.perf_counter()
    nodes_tbl, edges_tbl, graph = generate_graph(rng, n_nodes, n_edges)
    gen_s = time.perf_counter() - t0
    m = edges_tbl.num_rows
    print(f"[gen] done in {gen_s:.1f}s ({m} edges after dedupe)", flush=True)

    # One Lance copy, written by pylance. nanolance reads this same copy.
    t0 = time.perf_counter()
    nodes_path = os.path.join(lance_root, "nodes.lance")
    edges_path = os.path.join(lance_root, "edges.lance")
    pylance.write_dataset(nodes_tbl, nodes_path, mode="create")
    pylance.write_dataset(edges_tbl, edges_path, mode="create")
    write_s = time.perf_counter() - t0

    t0 = time.perf_counter()
    node_idx_s = create_lance_indexes(nodes_path, NODE_INDEXES)
    edge_idx_s = create_lance_indexes(edges_path, EDGE_INDEXES)
    index_s = time.perf_counter() - t0
    print(f"[lance] wrote in {write_s:.1f}s, indexes in {index_s:.1f}s", flush=True)

    lb, lb_info = load_ladybug(ladybug_path, run_dir, nodes_tbl, edges_tbl)
    print(f"[ladybug] COPY load {lb_info['load_seconds']:.1f}s", flush=True)

    # Degree stats for the report
    deg = graph["degree"]
    top = np.sort(deg)[::-1]
    degree_stats = {
        "max_degree": int(top[0]),
        "top20_min_degree": int(top[HUB_K - 1]),
        "median_degree": float(np.median(deg)),
        "nodes_with_no_edges": int((deg == 0).sum()),
    }

    samples = pick_samples(np.random.default_rng(args.seed + 1), graph, n_nodes, args.sample)
    del graph
    gc.collect()

    nano = LanceEngine("nanolance", nlance, lance_root)
    pyl = LanceEngine("pylance", pylance, lance_root)
    cached = CachedKeysNanolance(nano.nodes, nano.edges)

    plan_checks = {
        "nanolance_reads_pylance_btree_source_id": index_plan(
            edges_path, "source_id = 'x'", ["source_id"], nlance),
        "pylance_btree_source_id": index_plan(
            edges_path, "source_id = 'x'", ["source_id"], pylance),
        "nanolance_bitmap_relationship_name": index_plan(
            edges_path, "relationship_name = 'is_a'", ["source_id"], nlance),
        "nanolance_btree_node_id": index_plan(
            nodes_path, "id = 'x'", ["id"], nlance),
        "nanolance_name_unindexed": index_plan(
            nodes_path, "name = 'x'", ["id"], nlance),
    }  # fmt: skip
    print("[plans] " + json.dumps(plan_checks), flush=True)
    print(f"[indices] nanolance sees on nodes: {[i['fields'] for i in nano.nodes.list_indices()]}"
          f", on edges: {[i['fields'] for i in nano.edges.list_indices()]}", flush=True)  # fmt: skip

    # Query definitions: name, sample items, method, engines that run it.
    # The cached-keys rows are the adapter's current one-hop path on nanolance only.
    specs = [
        ("one_hop_single", samples["ordinary_ids"], "one_hop", ["nanolance", "pylance", "ladybug"]),
        ("one_hop_hub", samples["hub_ids"], "one_hop", ["nanolance", "pylance", "ladybug"]),
        ("edges_in_batch_50", samples["batch50_ids"], "edges_in", ["nanolance", "pylance", "ladybug"]),
        ("edges_in_batch_500", samples["batch500_ids"], "edges_in", ["nanolance", "pylance", "ladybug"]),
        ("nodes_in_batch_50", samples["batch50_ids"], "nodes_in", ["nanolance", "pylance", "ladybug"]),
        ("nodes_in_batch_500", samples["batch500_ids"], "nodes_in", ["nanolance", "pylance", "ladybug"]),
        ("in_unindexed_50", samples["batch50_names"], "unindexed_in", ["nanolance", "pylance", "ladybug"]),
        ("in_unindexed_500", samples["batch500_names"], "unindexed_in", ["nanolance", "pylance", "ladybug"]),
        ("type_filter_count", samples["types"], "type_count", ["nanolance", "pylance", "ladybug"]),
        ("full_scan_count_edges_rel3", samples["rel3"], "rel_count", ["nanolance", "pylance", "ladybug"]),
        ("one_hop_single_cached_keys", samples["ordinary_ids"], "cached", ["nanolance_cached"]),
        ("one_hop_hub_cached_keys", samples["hub_ids"], "cached", ["nanolance_cached"]),
    ]  # fmt: skip

    engines = {"nanolance": nano, "pylance": pyl, "ladybug": lb, "nanolance_cached": cached}

    def runner(engine_name: str, method: str):
        eng = engines[engine_name]
        if method == "cached":
            return eng.one_hop
        return getattr(eng, method)

    results: dict[str, dict] = {}
    mismatches: list[str] = []
    for name, items, method, names in specs:
        print(f"[query] {name}: verifying and timing {len(items)} samples", flush=True)
        fns = {e: runner(e, method) for e in names}
        for e in names:
            time_calls(fns[e], items, reps=1, warm=args.warmup)  # warm-up, discarded

        # Correctness: every sample item, every engine.
        row_counts = []
        for i, item in enumerate(items):
            outs = {e: fns[e](item) for e in names}
            row_counts.append(payload_rows(outs[names[0]]))
            ref = "pylance" if "pylance" in outs else None
            if ref:
                for e in names:
                    if e == ref:
                        continue
                    if e == "ladybug":
                        ok = ids_key(outs[e]) == ids_key(outs[ref])
                        label = "ids"
                    else:
                        ok = exact_payload(outs[e]) == exact_payload(outs[ref])
                        label = "rows"
                    if not ok:
                        detail = describe_diff(outs[ref], outs[e])
                        line = f"MISMATCH query={name} item={i} {e} vs {ref} ({label}): {detail}"
                        print(line, flush=True)
                        mismatches.append(line)
            else:  # cached-keys row: compare with the plain nanolance path, exact rows
                ref_out = nano.one_hop(item) if method == "cached" else None
                if exact_payload(outs["nanolance_cached"]) != exact_payload(ref_out):
                    line = f"MISMATCH query={name} item={i} nanolance_cached vs nanolance (rows): {describe_diff(ref_out, outs['nanolance_cached'])}"
                    print(line, flush=True)
                    mismatches.append(line)

        # Timing: every engine, sample items x reps.
        for e in names:
            gc.collect()
            samples_ms = time_calls(fns[e], items, reps=args.reps, warm=0)
            results.setdefault(name, {"avg_rows": float(np.mean(row_counts))})[e] = stats(samples_ms)
            results[name]["avg_rows"] = float(np.mean(row_counts))

    # Cross-check: nanolance cached vs pylance also already covered by the chain above.
    rows = []
    for name, _, _, _ in specs:
        r = {"query": name, "avg_rows": results[name]["avg_rows"]}
        for e in ("nanolance", "pylance", "ladybug"):
            if e in results[name]:
                r[e] = results[name][e]
        if "nanolance_cached" in results[name]:  # cached-keys rows are the nanolance column
            r["nanolance"] = results[name]["nanolance_cached"]
        rows.append(r)

    print()
    print_table(rows)
    print()
    print(f"nodes={n_nodes} edges={m} hubs(top-{HUB_K}) degrees={samples['hub_degrees'][:5]}...")
    print(f"mismatches: {len(mismatches)}")

    sizes = {
        "lance_nodes_bytes": dir_bytes(nodes_path),
        "lance_edges_bytes": dir_bytes(edges_path),
        "ladybug_bytes": dir_bytes(ladybug_path),
        "nodes_rows": nodes_tbl.num_rows,
        "edges_rows": m,
    }
    out = {
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "benchmark_script": "bench/graph/graph_bench.py",
        "machine": machine_info(),
        "versions": {
            "pylance": pkg_version("pylance"),
            "nanolance": pkg_version("nanolance"),
            "nanolance_git_commit": nanolance_commit(),
            "ladybug": pkg_version("ladybug"),
            "pyarrow": pa.__version__,
            "numpy": np.__version__,
        },
        "config": {
            "nodes": n_nodes, "edges": m, "seed": args.seed, "sample": args.sample,
            "reps": args.reps, "warmup": args.warmup, "in_chunk": IN_CHUNK,
            "zipf_endpoint_exponent": ZIPF_S, "node_types": len(NODE_TYPES),
            "relationship_names": len(REL_NAMES),
        },  # fmt: skip
        "data": {
            "sizes": sizes,
            "gen_seconds": round(gen_s, 2),
            "lance_write_seconds": round(write_s, 2),
            "lance_index_seconds": {"nodes": node_idx_s, "edges": edge_idx_s},
            "ladybug_load": lb_info,
            "degree": degree_stats,
            "index_plans": plan_checks,
        },
        "results": rows,
        "mismatches": mismatches,
    }
    if args.json_out:
        with open(args.json_out, "w") as fh:
            json.dump(out, fh, indent=2)
        print(f"wrote {args.json_out}")

    if not args.keep_data:
        shutil.rmtree(run_dir, ignore_errors=True)
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
