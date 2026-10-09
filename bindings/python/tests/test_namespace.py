"""Namespaces against pylance: DirectoryNamespace's catalog (``__manifest``) and the REST spec.

A catalog either library writes must read the same in the other -- namespaces, their properties,
tables (child tables in Lance's ``<hash>_<object_id>`` directories), declared tables, data -- and both
libraries must be able to write into one catalog. The REST client and adapter must talk to Lance's.
Failures must raise the same lance_namespace errors with the same words.
"""

from __future__ import annotations

import concurrent.futures
import io

import pyarrow as pa
import pytest

import nanolance.lance as nl
import nanolance.lance.namespace as nl_ns
from tests.support import require_pylance

lance_namespace = pytest.importorskip("lance_namespace")
from lance_namespace import (  # noqa: E402
    CountTableRowsRequest,
    CreateNamespaceRequest,
    CreateTableRequest,
    DeclareTableRequest,
    DescribeNamespaceRequest,
    DescribeTableRequest,
    DropNamespaceRequest,
    DropTableRequest,
    InsertIntoTableRequest,
    ListNamespacesRequest,
    ListTablesRequest,
    QueryTableRequest,
    RegisterTableRequest,
    TableExistsRequest,
)


def _ipc(table: pa.Table) -> bytes:
    sink = io.BytesIO()
    with pa.ipc.new_stream(sink, table.schema) as writer:
        writer.write_table(table)
    return sink.getvalue()


_DATA = pa.table({"id": [1, 2, 3], "name": ["a", "b", "c"]})


def _write_catalog(lib, ns, tag: str) -> None:
    ns.create_namespace(CreateNamespaceRequest(id=["ws"], properties={"owner": tag}))
    ns.create_namespace(CreateNamespaceRequest(id=["ws", "sub"]))
    ns.create_table(CreateTableRequest(id=["ws", "t1"], properties={"k": "v"}), _ipc(_DATA))
    ns.create_table(CreateTableRequest(id=["root_t"]), _ipc(_DATA))
    ns.declare_table(DeclareTableRequest(id=["ws", "decl"]))
    ns.create_table(CreateTableRequest(id=["ws", "sub", "deep"]), _ipc(_DATA))
    ns.drop_table(DropTableRequest(id=["ws", "sub", "deep"]))
    lib.write_dataset(pa.table({"id": [4], "name": ["d"]}), namespace_client=ns, table_id=["ws", "t1"], mode="append")


def _read_catalog(lib, ns):
    t1 = ns.describe_table(DescribeTableRequest(id=["ws", "t1"]))
    return {
        "namespaces": (ns.list_namespaces(ListNamespacesRequest(id=[])).namespaces,
                       ns.list_namespaces(ListNamespacesRequest(id=["ws"])).namespaces),
        "tables": (sorted(ns.list_tables(ListTablesRequest(id=["ws"])).tables),
                   ns.list_tables(ListTablesRequest(id=[])).tables,
                   sorted(ns.list_tables(ListTablesRequest(id=["ws"], include_declared=False)).tables)),
        "t1": (t1.location.rsplit("/", 1)[-1].split("_", 1)[-1], t1.properties),
        "declared": ns.describe_table(DescribeTableRequest(id=["ws", "decl"], check_declared=True)).is_only_declared,
        "rows": lib.dataset(namespace_client=ns, table_id=["ws", "t1"]).to_table().to_pydict(),
        "count": ns.count_table_rows(CountTableRowsRequest(id=["root_t"], predicate="id > 1")).count,
    }


@pytest.mark.parametrize("writer", ["nanolance", "pylance"])
def test_catalog_reads_the_same_in_both(tmp_path, writer):
    lance = require_pylance()
    import lance.namespace as py_ns

    libs = {"nanolance": (nl, nl_ns), "pylance": (lance, py_ns)}
    lib, mod = libs[writer]
    _write_catalog(lib, mod.DirectoryNamespace(root=str(tmp_path)), writer)
    seen = {name: _read_catalog(l, m.DirectoryNamespace(root=str(tmp_path))) for name, (l, m) in libs.items()}
    assert seen["nanolance"] == seen["pylance"]
    assert seen["nanolance"]["namespaces"] == (["ws"], ["sub"])
    assert seen["nanolance"]["t1"] == ("ws$t1", {"k": "v"})
    assert seen["nanolance"]["rows"] == {"id": [1, 2, 3, 4], "name": ["a", "b", "c", "d"]}
    assert seen["nanolance"]["declared"] is True
    assert seen["nanolance"]["tables"][2] == ["t1"]


def test_both_libraries_write_one_catalog(tmp_path):
    lance = require_pylance()
    import lance.namespace as py_ns

    order = [nl_ns, py_ns, nl_ns, py_ns]
    for i, mod in enumerate(order):
        ns = mod.DirectoryNamespace(root=str(tmp_path))
        if i == 0:
            ns.create_namespace(CreateNamespaceRequest(id=["ws"]))
        ns.create_table(CreateTableRequest(id=["ws", f"t{i}"]), _ipc(_DATA))
    for mod in (nl_ns, py_ns):
        ns = mod.DirectoryNamespace(root=str(tmp_path))
        assert sorted(ns.list_tables(ListTablesRequest(id=["ws"])).tables) == ["t0", "t1", "t2", "t3"]
    ds = lance.dataset(namespace_client=py_ns.DirectoryNamespace(root=str(tmp_path)), table_id=["ws", "t0"])
    assert ds.to_table() == _DATA
    ds.validate()


def _errors(ns):
    """The error each of a set of failing calls raises: (class, message)."""
    ns.create_namespace(CreateNamespaceRequest(id=["ws"]))
    ns.create_table(CreateTableRequest(id=["ws", "t"]), _ipc(_DATA))
    calls = [
        lambda: ns.create_namespace(CreateNamespaceRequest(id=["ws"])),
        lambda: ns.create_namespace(CreateNamespaceRequest(id=["missing", "child"])),
        lambda: ns.create_namespace(CreateNamespaceRequest(id=[])),
        lambda: ns.drop_namespace(DropNamespaceRequest(id=["ws"])),
        lambda: ns.drop_namespace(DropNamespaceRequest(id=["nope"])),
        lambda: ns.drop_namespace(DropNamespaceRequest(id=[])),
        lambda: ns.describe_namespace(DescribeNamespaceRequest(id=["nope"])),
        lambda: ns.describe_table(DescribeTableRequest(id=["ws", "nope"])),
        lambda: ns.describe_table(DescribeTableRequest(id=["nope"])),
        lambda: ns.table_exists(TableExistsRequest(id=["ws", "nope"])),
        lambda: ns.create_table(CreateTableRequest(id=["ws", "t"]), _ipc(_DATA)),
        lambda: ns.create_table(CreateTableRequest(id=["ws", "u"]), b""),
        lambda: ns.create_table(CreateTableRequest(id=[]), _ipc(_DATA)),
        lambda: ns.create_table(CreateTableRequest(id=["ws", "u"], mode="bogus"), _ipc(_DATA)),
        lambda: ns.declare_table(DeclareTableRequest(id=["ws", "t"])),
        lambda: ns.drop_table(DropTableRequest(id=["ws", "nope"])),
        lambda: ns.register_table(RegisterTableRequest(id=["ws", "r"], location="s3://b/t.lance")),
        lambda: ns.register_table(RegisterTableRequest(id=["ws", "r"], location="/tmp/t.lance")),
        lambda: ns.register_table(RegisterTableRequest(id=["ws", "r"], location="../t.lance")),
        lambda: ns.register_table(RegisterTableRequest(id=["missing", "r"], location="t.lance")),
        lambda: ns.insert_into_table(InsertIntoTableRequest(id=["ws", "t"], mode="upsert"), _ipc(_DATA)),
    ]
    out = []
    for call in calls:
        try:
            call()
        except Exception as exc:  # noqa: BLE001
            out.append((type(exc).__name__, str(exc).replace(str(ns.namespace_id()), "<ns>")))
        else:
            out.append(None)
    return out


def test_errors_match_pylance(tmp_path):
    require_pylance()
    import lance.namespace as py_ns

    ours = _errors(nl_ns.DirectoryNamespace(root=str(tmp_path / "a")))
    theirs = _errors(py_ns.DirectoryNamespace(root=str(tmp_path / "b")))
    assert ours == theirs
    assert all(e is not None for e in ours)


@pytest.mark.parametrize("server,client", [("nanolance", "pylance"), ("pylance", "nanolance"),
                                           ("nanolance", "nanolance")])
def test_rest_against_lance(tmp_path, server, client):
    require_pylance()
    import lance.namespace as py_ns

    mods = {"nanolance": nl_ns, "pylance": py_ns}
    with mods[server].RestAdapter("dir", {"root": str(tmp_path)}, port=0) as adapter:
        ns = mods[client].RestNamespace(uri=f"http://127.0.0.1:{adapter.port}")
        ns.create_namespace(CreateNamespaceRequest(id=["ws"], properties={"p": "1"}))
        assert ns.create_table(CreateTableRequest(id=["ws", "t"]), _ipc(_DATA)).version == 1
        ns.insert_into_table(InsertIntoTableRequest(id=["ws", "t"], mode="append"), _ipc(_DATA))
        assert ns.list_tables(ListTablesRequest(id=["ws"])).tables == ["t"]
        assert ns.describe_namespace(DescribeNamespaceRequest(id=["ws"])).properties == {"p": "1"}
        data = ns.query_table(QueryTableRequest(id=["ws", "t"], k=10, vector={}, filter="id > 1")).data
        assert pa.ipc.open_file(pa.BufferReader(data)).read_all().num_rows == 4
        errors = []
        for call in (lambda: ns.describe_table(DescribeTableRequest(id=["ws", "nope"])),
                     lambda: ns.drop_namespace(DropNamespaceRequest(id=["ws"])),
                     lambda: ns.create_table(CreateTableRequest(id=["ws", "t"]), _ipc(_DATA))):
            with pytest.raises(lance_namespace.errors.LanceNamespaceError) as info:
                call()
            errors.append((type(info.value).__name__, str(info.value)))
        assert errors == [
            ("TableNotFoundError", "Table not found: table id 'ws$nope'"),
            ("NamespaceNotEmptyError", "Namespace not empty: 'ws' (contains 1 child objects)"),
            ("TableAlreadyExistsError", "Table already exists: t"),
        ]


def test_concurrent_writers_from_both_libraries(tmp_path):
    require_pylance()
    import lance.namespace as py_ns

    nl_ns.DirectoryNamespace(root=str(tmp_path)).create_namespace(CreateNamespaceRequest(id=["ws"]))

    def create(i):
        mod = nl_ns if i % 2 else py_ns
        ns = mod.DirectoryNamespace(root=str(tmp_path), commit_retries="1000")
        ns.create_table(CreateTableRequest(id=["ws", f"t{i}"]), _ipc(_DATA))

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        list(pool.map(create, range(12)))
    for mod in (nl_ns, py_ns):
        tables = mod.DirectoryNamespace(root=str(tmp_path)).list_tables(ListTablesRequest(id=["ws"])).tables
        assert sorted(tables) == sorted(f"t{i}" for i in range(12))


def test_managed_versioning_publishes_through_the_namespace(tmp_path):
    lance = require_pylance()
    import lance.namespace as py_ns

    ns = nl_ns.DirectoryNamespace(root=str(tmp_path), table_version_tracking_enabled="true",
                                  ops_metrics_enabled="true")
    ns.create_namespace(CreateNamespaceRequest(id=["ws"]))
    nl.write_dataset(_DATA, namespace_client=ns, table_id=["ws", "t"], mode="create")
    ds = nl.write_dataset(_DATA, namespace_client=ns, table_id=["ws", "t"], mode="append")
    ds.delete("id = 1")
    assert ns.retrieve_ops_metrics()["create_table_version"] == 3
    theirs = py_ns.DirectoryNamespace(root=str(tmp_path), table_version_tracking_enabled="true")
    assert lance.dataset(namespace_client=theirs, table_id=["ws", "t"]).count_rows() == 4
    assert lance.dataset(namespace_client=theirs, table_id=["ws", "t"], version=1).count_rows() == 3
    lance.write_dataset(_DATA, namespace_client=theirs, table_id=["ws", "t"], mode="append")
    assert nl.dataset(namespace_client=ns, table_id=["ws", "t"]).version == 4
