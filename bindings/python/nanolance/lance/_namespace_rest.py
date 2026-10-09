# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""Lance's REST namespace: the client (lance-namespace-impls 12, ``rest.rs``) and the adapter that
serves any namespace over the REST spec (``rest_adapter.rs``), in Python.

Both speak the Lance Namespace REST spec: ``/v1/namespace/{id}/...`` and ``/v1/table/{id}/...``, the
id's parts joined by the delimiter (``$``; the delimiter alone is the root) and percent-encoded, JSON
bodies keyed as the spec's models alias them, Arrow IPC bodies for table data, and errors as the
spec's ErrorResponse ``{"error": message, "code": n}`` with Lance's HTTP statuses. So a nanolance
client talks to Lance's adapter and Lance's client to nanolance's.
"""

from __future__ import annotations

import http.client
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import parse_qs, quote, unquote, urlparse

from nanolance.lance._namespace_dir import _as_dict, ns_error

# operation -> (HTTP method, path under /v1 with {id} / {index}, body: json | none | binary | null,
#               query parameters taken from the request, request model, response kind)
# Response kind: "json" (a model), "none" (204), "bytes" (Arrow IPC), "count", "dict" (a plain map).
_OPS: Dict[str, Tuple[str, str, str, Tuple[str, ...], Optional[str], str]] = {
    "list_namespaces": ("GET", "namespace/{id}/list", "none", ("page_token", "limit"), "ListNamespacesRequest", "json"),
    "describe_namespace": ("POST", "namespace/{id}/describe", "json", (), "DescribeNamespaceRequest", "json"),
    "create_namespace": ("POST", "namespace/{id}/create", "json", (), "CreateNamespaceRequest", "json"),
    "drop_namespace": ("POST", "namespace/{id}/drop", "json", (), "DropNamespaceRequest", "json"),
    "namespace_exists": ("POST", "namespace/{id}/exists", "json", (), "NamespaceExistsRequest", "none"),
    "list_tables": ("GET", "namespace/{id}/table/list", "none", ("page_token", "limit", "include_declared"),
                    "ListTablesRequest", "json"),
    "describe_table": ("POST", "table/{id}/describe", "json", ("with_table_uri", "load_detailed_metadata",
                                                                "check_declared"), "DescribeTableRequest", "json"),
    "register_table": ("POST", "table/{id}/register", "json", (), "RegisterTableRequest", "json"),
    "table_exists": ("POST", "table/{id}/exists", "json", (), "TableExistsRequest", "none"),
    "drop_table": ("POST", "table/{id}/drop", "json", (), "DropTableRequest", "json"),
    "deregister_table": ("POST", "table/{id}/deregister", "json", (), "DeregisterTableRequest", "json"),
    "count_table_rows": ("GET", "table/{id}/count_rows", "none", (), "CountTableRowsRequest", "count"),
    "create_table": ("POST", "table/{id}/create", "binary", ("mode", "properties", "storage_options"),
                     "CreateTableRequest", "json"),
    "declare_table": ("POST", "table/{id}/declare", "json", (), "DeclareTableRequest", "json"),
    "insert_into_table": ("POST", "table/{id}/insert", "binary", ("mode",), "InsertIntoTableRequest", "json"),
    "merge_insert_into_table": ("POST", "table/{id}/merge_insert", "binary",
                                ("on", "when_matched_update_all", "when_matched_update_all_filt",
                                 "when_not_matched_insert_all", "when_not_matched_by_source_delete",
                                 "when_not_matched_by_source_delete_filt", "timeout", "use_index"),
                                "MergeInsertIntoTableRequest", "json"),
    "update_table": ("POST", "table/{id}/update", "json", (), "UpdateTableRequest", "json"),
    "delete_from_table": ("POST", "table/{id}/delete", "json", (), "DeleteFromTableRequest", "json"),
    "query_table": ("POST", "table/{id}/query", "json", (), "QueryTableRequest", "bytes"),
    "create_table_index": ("POST", "table/{id}/create_index", "json", (), "CreateTableIndexRequest", "json"),
    "create_table_scalar_index": ("POST", "table/{id}/create_scalar_index", "json", (), "CreateTableIndexRequest",
                                  "json"),
    "list_table_indices": ("POST", "table/{id}/index/list", "json", (), "ListTableIndicesRequest", "json"),
    "describe_table_index_stats": ("POST", "table/{id}/index/{index}/stats", "json", (),
                                   "DescribeTableIndexStatsRequest", "json"),
    "drop_table_index": ("POST", "table/{id}/index/{index}/drop", "json", (), "DropTableIndexRequest", "json"),
    "describe_transaction": ("POST", "transaction/{id}/describe", "json", (), "DescribeTransactionRequest", "json"),
    "alter_transaction": ("POST", "transaction/{id}/alter", "json", (), "AlterTransactionRequest", "json"),
    "list_all_tables": ("GET", "table", "none", ("page_token", "limit", "include_declared"), "ListTablesRequest",
                        "json"),
    "restore_table": ("POST", "table/{id}/restore", "json", (), "RestoreTableRequest", "json"),
    "rename_table": ("POST", "table/{id}/rename", "json", (), "RenameTableRequest", "json"),
    "list_table_versions": ("POST", "table/{id}/version/list", "null",
                            ("page_token", "limit", "descending", "branch"), "ListTableVersionsRequest", "json"),
    "create_table_version": ("POST", "table/{id}/version/create", "json", (), "CreateTableVersionRequest", "dict"),
    "describe_table_version": ("POST", "table/{id}/version/describe", "json", (), "DescribeTableVersionRequest",
                               "dict"),
    "batch_delete_table_versions": ("POST", "table/{id}/version/delete", "json", (),
                                    "BatchDeleteTableVersionsRequest", "dict"),
    "update_table_schema_metadata": ("POST", "table/{id}/schema_metadata/update", "metadata", (),
                                     "UpdateTableSchemaMetadataRequest", "metadata"),
    "get_table_stats": ("POST", "table/{id}/stats", "json", (), "GetTableStatsRequest", "json"),
    "explain_table_query_plan": ("POST", "table/{id}/explain_plan", "json", (), "ExplainTableQueryPlanRequest",
                                 "text"),
    "analyze_table_query_plan": ("POST", "table/{id}/analyze_plan", "json", (), "AnalyzeTableQueryPlanRequest",
                                 "text"),
    "alter_table_add_columns": ("POST", "table/{id}/add_columns", "json", (), "AlterTableAddColumnsRequest", "json"),
    "alter_table_alter_columns": ("POST", "table/{id}/alter_columns", "json", (), "AlterTableAlterColumnsRequest",
                                  "json"),
    "alter_table_drop_columns": ("POST", "table/{id}/drop_columns", "json", (), "AlterTableDropColumnsRequest",
                                 "json"),
    "list_table_tags": ("GET", "table/{id}/tags/list", "none", ("page_token", "limit"), "ListTableTagsRequest",
                        "json"),
    "get_table_tag_version": ("POST", "table/{id}/tags/version", "json", (), "GetTableTagVersionRequest", "json"),
    "create_table_tag": ("POST", "table/{id}/tags/create", "json", (), "CreateTableTagRequest", "json"),
    "delete_table_tag": ("POST", "table/{id}/tags/delete", "json", (), "DeleteTableTagRequest", "json"),
    "update_table_tag": ("POST", "table/{id}/tags/update", "json", (), "UpdateTableTagRequest", "json"),
    "create_table_branch": ("POST", "table/{id}/branches/create", "json", (), "CreateTableBranchRequest", "json"),
    "list_table_branches": ("POST", "table/{id}/branches/list", "json", ("page_token", "limit"),
                            "ListTableBranchesRequest", "json"),
    "delete_table_branch": ("POST", "table/{id}/branches/delete", "json", (), "DeleteTableBranchRequest", "json"),
}

# Statuses for lance_namespace's error codes (rest_adapter.rs's error_code_to_status).
_STATUS = {0: 406, 1: 404, 2: 409, 3: 409, 4: 404, 5: 409, 6: 404, 7: 409, 8: 404, 9: 409, 10: 404, 11: 404,
           12: 404, 13: 400, 14: 409, 15: 403, 16: 401, 17: 503, 18: 500, 19: 409, 20: 400, 21: 429, 22: 404,
           23: 409}
_CREATED = {"create_namespace", "create_table", "declare_table"}


def _model(name: Optional[str]):
    import lance_namespace

    return getattr(lance_namespace, name) if name else None


def _to_wire(model_name: Optional[str], request: Dict[str, Any]) -> Dict[str, Any]:
    """A request dict (field names) as the spec's JSON (aliases, nulls left out)."""
    cls = _model(model_name)
    if cls is None:
        return {k: v for k, v in request.items() if v is not None}
    try:
        return cls.model_validate(request).to_dict()
    except Exception:  # noqa: BLE001 -- a dict a model does not take: send it as given
        return {k: v for k, v in request.items() if v is not None}


def _query_value(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (dict, list)):
        return json.dumps(value, separators=(",", ":"))
    return str(value)


class PyRestNamespace:
    """Lance's RestNamespace client."""

    DEFAULT_DELIMITER = "$"

    def __init__(self, context_provider=None, **properties):
        props = {str(k): str(v) for k, v in properties.items()}
        uri = props.get("uri")
        if uri is None:
            raise ns_error("InvalidInput", "Missing required property 'uri' for REST namespace")
        self._uri = uri.rstrip("/")
        self._delimiter = props.get("delimiter", self.DEFAULT_DELIMITER)
        self._headers = {}
        for key, value in props.items():
            for prefix in ("header.", "headers."):
                if key.startswith(prefix):
                    self._headers[key[len(prefix):]] = value
        self._context_provider = context_provider
        from nanolance.lance._namespace_dir import _str_to_bool

        self._metrics: Optional[Dict[str, int]] = {} if _str_to_bool(props.get("ops_metrics_enabled")) else None
        self._metrics_lock = threading.Lock()
        parsed = urlparse(self._uri)
        self._scheme = parsed.scheme or "http"
        self._host = parsed.hostname or "localhost"
        self._port = parsed.port
        self._base_path = parsed.path.rstrip("/")

    def namespace_id(self) -> str:
        return f"RestNamespace {{ endpoint: {json.dumps(self._uri)}, delimiter: {json.dumps(self._delimiter)} }}"

    def retrieve_ops_metrics(self) -> Dict[str, int]:
        if self._metrics is None:
            return {}
        with self._metrics_lock:
            return dict(self._metrics)

    def reset_ops_metrics(self) -> None:
        if self._metrics is not None:
            with self._metrics_lock:
                self._metrics.clear()

    def _object_id(self, parts) -> str:
        if parts is None:
            raise ns_error("InvalidInput", "Object ID is required")
        return self._delimiter.join(parts) if parts else self._delimiter

    def _call(self, operation: str, request, data: Optional[bytes] = None):
        method, template, body_kind, query_keys, model_name, response_kind = _OPS[operation]
        req = _as_dict(request)
        if self._metrics is not None:
            with self._metrics_lock:
                self._metrics[operation] = self._metrics.get(operation, 0) + 1
        object_id = self._object_id(req.get("id")) if "{id}" in template else ""
        path = template.replace("{id}", quote(object_id, safe=""))
        if "{index}" in path:
            path = path.replace("{index}", quote(req.get("index_name") or "", safe=""))
        query = [("delimiter", self._delimiter)]
        for key in query_keys:
            if req.get(key) is not None:
                query.append((key, _query_value(req[key])))
        if body_kind == "json":
            body = json.dumps(_to_wire(model_name, req)).encode()
            content_type = "application/json"
        elif body_kind == "null":
            body, content_type = b"null", "application/json"
        elif body_kind == "metadata":
            body, content_type = json.dumps(req.get("metadata") or {}).encode(), "application/json"
        elif body_kind == "binary":
            body, content_type = bytes(data or b""), "application/vnd.apache.arrow.stream"
        else:
            body, content_type = None, None
        headers = dict(self._headers)
        if self._context_provider is not None:
            context = self._context_provider.provide_context({"operation": operation, "object_id": object_id})
            for key, value in (context or {}).items():
                if key.startswith("headers."):
                    headers[key[len("headers."):]] = str(value)
        if content_type is not None:
            headers["Content-Type"] = content_type
        url = f"{self._base_path}/v1/{path}?" + "&".join(f"{k}={quote(v, safe='')}" for k, v in query)
        status, payload = self._send(method, url, body, headers)
        if not 200 <= status < 300:
            raise self._error(status, payload)
        if response_kind == "none":
            return None
        if response_kind == "bytes":
            return payload
        text = payload.decode("utf-8") if payload else ""
        if response_kind == "text":
            try:
                value = json.loads(text)
                return value if isinstance(value, str) else text
            except ValueError:
                return text
        try:
            value = json.loads(text) if text else {}
        except ValueError as exc:
            raise ns_error("Internal", f"Failed to parse response: {exc}") from None
        if response_kind == "count":
            return int(value["count"]) if isinstance(value, dict) else int(value)
        if response_kind == "metadata":
            return {"metadata": value}
        return value

    def _send(self, method: str, url: str, body: Optional[bytes], headers: Dict[str, str]) -> Tuple[int, bytes]:
        cls = http.client.HTTPSConnection if self._scheme == "https" else http.client.HTTPConnection
        conn = cls(self._host, self._port, timeout=600)
        try:
            conn.request(method, url, body=body, headers=headers)
            response = conn.getresponse()
            return response.status, response.read()
        except (ConnectionError, TimeoutError, OSError) as exc:
            raise ns_error("ServiceUnavailable",
                           f"Failed to execute request: {exc}") from None
        finally:
            conn.close()

    @staticmethod
    def _error(status: int, payload: bytes) -> Exception:
        from lance_namespace.errors import ErrorCode

        from nanolance.lance._namespace_dir import _KINDS

        content = payload.decode("utf-8", "replace") if payload else ""
        try:
            parsed = json.loads(content)
            code = int(parsed["code"])
            message = parsed.get("error") or content
        except Exception:  # noqa: BLE001
            return ns_error("Internal", f"Failed to parse error response: status={status}, body={content}")
        names = {
            ErrorCode.UNSUPPORTED: "Unsupported", ErrorCode.NAMESPACE_NOT_FOUND: "NamespaceNotFound",
            ErrorCode.NAMESPACE_ALREADY_EXISTS: "NamespaceAlreadyExists",
            ErrorCode.NAMESPACE_NOT_EMPTY: "NamespaceNotEmpty", ErrorCode.TABLE_NOT_FOUND: "TableNotFound",
            ErrorCode.TABLE_ALREADY_EXISTS: "TableAlreadyExists",
            ErrorCode.TABLE_INDEX_NOT_FOUND: "TableIndexNotFound",
            ErrorCode.TABLE_INDEX_ALREADY_EXISTS: "TableIndexAlreadyExists",
            ErrorCode.TABLE_TAG_NOT_FOUND: "TableTagNotFound", ErrorCode.TABLE_TAG_ALREADY_EXISTS: "TableTagAlreadyExists",
            ErrorCode.TRANSACTION_NOT_FOUND: "TransactionNotFound",
            ErrorCode.TABLE_VERSION_NOT_FOUND: "TableVersionNotFound",
            ErrorCode.TABLE_COLUMN_NOT_FOUND: "TableColumnNotFound", ErrorCode.INVALID_INPUT: "InvalidInput",
            ErrorCode.CONCURRENT_MODIFICATION: "ConcurrentModification", ErrorCode.INTERNAL: "Internal",
            ErrorCode.INVALID_TABLE_STATE: "InvalidTableState",
            ErrorCode.TABLE_BRANCH_NOT_FOUND: "TableBranchNotFound",
            ErrorCode.TABLE_BRANCH_ALREADY_EXISTS: "TableBranchAlreadyExists",
            ErrorCode.PERMISSION_DENIED: "PermissionDenied", ErrorCode.UNAUTHENTICATED: "Unauthenticated",
            ErrorCode.SERVICE_UNAVAILABLE: "ServiceUnavailable", ErrorCode.THROTTLING: "Throttling",
            ErrorCode.TABLE_SCHEMA_VALIDATION_ERROR: "TableSchemaValidationError",
        }
        kind = names.get(code)
        if kind is not None and kind in _KINDS:
            return ns_error(kind, message)
        from lance_namespace.errors import from_error_code

        return from_error_code(code, message)

    def __getattr__(self, name: str):
        if name.startswith("_") or name not in _OPS:
            raise AttributeError(name)
        if _OPS[name][2] == "binary":
            return lambda request, request_data=b"": self._call(name, request, request_data)
        return lambda request: self._call(name, request)


class _Handler(BaseHTTPRequestHandler):
    server_version = "nanolance-rest-adapter"
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):  # noqa: A002 -- quiet, as the Rust adapter is
        pass

    def do_GET(self):  # noqa: N802
        self._dispatch("GET")

    def do_POST(self):  # noqa: N802
        self._dispatch("POST")

    def _reply(self, status: int, body: bytes = b"", content_type: str = "application/json") -> None:
        self.send_response(status)
        if body or status != 204:
            self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _error(self, exc: BaseException) -> None:
        from lance_namespace.errors import LanceNamespaceError

        if isinstance(exc, LanceNamespaceError):
            from nanolance.lance._namespace_dir import _KINDS

            code = int(exc.code)
            message = getattr(exc, "ns_message", None)
            if message is None:  # another implementation's error: send its message without the Display prefix
                message = str(exc)
                for name, prefix in _KINDS.values():
                    if type(exc).__name__ == name and message.startswith(prefix + ": "):
                        message = message[len(prefix) + 2:]
                        break
        else:
            code, message = 18, str(exc)
        self._reply(_STATUS.get(code, 500), json.dumps({"error": message, "code": code}).encode())

    def _dispatch(self, method: str) -> None:
        parsed = urlparse(self.path)
        params = {k: v[-1] for k, v in parse_qs(parsed.query, keep_blank_values=True).items()}
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        parts = [p for p in parsed.path.rstrip("/").split("/") if p]
        found = self._route(method, parts)
        if found is None:
            self._reply(404, b"")
            return
        operation, raw_id, index_name = found
        try:
            self._run(operation, raw_id, index_name, params, body)
        except Exception as exc:  # noqa: BLE001
            self._error(exc)

    @staticmethod
    def _route(method: str, parts: List[str]):
        if len(parts) < 2 or parts[0] != "v1":
            return None
        rest = parts[1:]
        for operation, (op_method, template, *_unused) in _OPS.items():
            pattern = template.split("/")
            if len(pattern) != len(rest):
                continue
            raw_id = index_name = None
            for p, r in zip(pattern, rest):
                if p == "{id}":
                    raw_id = unquote(r)
                elif p == "{index}":
                    index_name = unquote(r)
                elif p != r:
                    break
            else:
                if operation == "get_table_stats" or op_method == method:
                    return operation, raw_id, index_name
        return None

    def _run(self, operation: str, raw_id: Optional[str], index_name: Optional[str], params: Dict[str, str],
             body: bytes) -> None:
        _, _, body_kind, query_keys, model_name, response_kind = _OPS[operation]
        backend = self.server.backend
        delimiter = params.get("delimiter") or "$"
        request: Dict[str, Any] = {}
        if body_kind == "json" and body:
            request = json.loads(body)
            if not isinstance(request, dict):
                request = {}
        elif body_kind == "metadata" and body:
            request = {"metadata": json.loads(body)}
        for key in query_keys:
            if key in params:
                value = params[key]
                if key in ("properties", "storage_options"):
                    value = json.loads(value)
                elif key in ("limit", "timeout"):
                    value = int(value)
                elif value in ("true", "false"):
                    value = value == "true"
                request[key] = value
        if raw_id is not None:
            request["id"] = [] if raw_id == delimiter else [p for p in raw_id.split(delimiter) if p]
        if index_name is not None:
            request["index_name"] = index_name
        cls = _model(model_name)
        typed = cls.from_dict(request) if cls is not None else request
        fn = getattr(backend, operation)
        if response_kind == "dict":
            result = fn(dict(request))
        elif body_kind == "binary":
            result = fn(typed, body)
        else:
            result = fn(typed)
        if response_kind == "none":
            self._reply(204)
            return
        if response_kind == "bytes":
            data = getattr(result, "data", result)
            self._reply(200, bytes(data), "application/vnd.apache.arrow.file")
            return
        if response_kind == "count":
            count = getattr(result, "count", result)
            self._reply(200, json.dumps({"count": int(count)}).encode())
            return
        if response_kind == "text":
            self._reply(200, json.dumps(result).encode())
            return
        if response_kind == "metadata":
            self._reply(200, json.dumps(getattr(result, "metadata", None) or {}).encode())
            return
        if hasattr(result, "to_dict"):
            payload = result.to_dict()
        elif isinstance(result, dict):
            payload = {k: v for k, v in result.items() if v is not None}
        else:
            payload = {}
        self._reply(201 if operation in _CREATED else 200, json.dumps(payload).encode())


class _Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class PyRestAdapter:
    """Lance's RestAdapter: serves a namespace (built from an implementation name and its properties)
    over the REST spec, in a background thread."""

    def __init__(self, namespace_client_impl: str, namespace_client_properties: Optional[Dict[str, str]] = None,
                 session=None, host: Optional[str] = None, port: Optional[int] = None):
        self._impl = namespace_client_impl
        self._properties = {str(k): str(v) for k, v in (namespace_client_properties or {}).items()}
        self._host = host or "127.0.0.1"
        self._requested_port = 2333 if port is None else int(port)
        self._server: Optional[_Server] = None
        self._thread: Optional[threading.Thread] = None

    @property
    def port(self) -> int:
        return self._server.server_address[1] if self._server is not None else 0

    def start(self) -> None:
        import lance_namespace

        from nanolance.lance import namespace as nanolance_namespace

        native = {"dir": nanolance_namespace.DirectoryNamespace, "rest": nanolance_namespace.RestNamespace}
        if self._impl in native:
            backend = native[self._impl](**self._properties)
        else:
            backend = lance_namespace.connect(self._impl, self._properties)
        try:
            server = _Server((self._host, self._requested_port), _Handler)
        except OSError as exc:
            raise ns_error("Internal", f"Failed to bind to {self._host}:{self._requested_port}: {exc}") from None
        server.backend = backend
        self._server = server
        self._thread = threading.Thread(target=server.serve_forever, name="nanolance-rest-adapter", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
            if self._thread is not None:
                self._thread.join()
            self._server = None
            self._thread = None
