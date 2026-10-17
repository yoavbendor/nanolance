#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Yoav Bendor
"""A static file server with HTTP Range support, for the weblance browser tests.

  GET/HEAD with `Range: bytes=a-b`, `bytes=a-` or `bytes=-n` answers 206 with Content-Range (416 when
  the start is past the end). Without a Range header, 200 with the whole file.
  `/__tree/<dir>` lists a directory the way the Hugging Face tree API does:
  [{"type": "file" | "directory", "path": "<path from the root>", "size": n}].
  Every response allows any origin and exposes the headers a browser needs for Range reads.
  Each request is logged to stderr as one line: METHOD path range status bytes.

Usage: range_server.py ROOT [PORT]     (PORT 0, the default, picks a free port; it prints "PORT n")
"""

import json
import mimetypes
import os
import re
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = ""
RANGE_RE = re.compile(r"^bytes=(\d*)-(\d*)$")
CHUNK = 1 << 16


def safe_join(path):
    rel = urllib.parse.unquote(path).lstrip("/")
    full = os.path.normpath(os.path.join(ROOT, rel))
    root = os.path.normpath(ROOT)
    if full != root and not full.startswith(root + os.sep):
        return None
    return full


class Handler(BaseHTTPRequestHandler):
    server_version = "weblance-range/1"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # replaced by the one-line log in _send_status
        pass

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Expose-Headers", "Content-Range, Content-Length, Accept-Ranges, Link")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Range")
        self.send_header("Content-Length", "0")
        self.end_headers()
        self._status = 204
        self._log(0)

    def do_GET(self):
        self._serve(send_body=True)

    def do_HEAD(self):
        self._serve(send_body=False)

    def _log(self, nbytes, status=None):
        rng = self.headers.get("Range", "-")
        code = status if status is not None else getattr(self, "_status", 0)
        sys.stderr.write(f"{self.command} {self.path} {rng} {code} {nbytes}\n")
        sys.stderr.flush()

    def _error(self, status, message=b""):
        self._status = status
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(message)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(message)
        self._log(len(message))

    def _serve(self, send_body):
        path = urllib.parse.urlsplit(self.path).path
        if path.startswith("/__tree/"):
            return self._tree(path[len("/__tree/"):].strip("/"))
        full = safe_join(path)
        if full is None or not os.path.isfile(full):
            return self._error(404, b"not found")
        size = os.path.getsize(full)
        ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
        if full.endswith(".wasm"):
            ctype = "application/wasm"
        rng = self.headers.get("Range")
        start, end, status = 0, size - 1, 200
        if rng is not None:
            m = RANGE_RE.match(rng.strip())
            if not m or (m.group(1) == "" and m.group(2) == ""):
                return self._error(416, b"bad range")
            if m.group(1) == "":  # suffix: the last n bytes
                n = int(m.group(2))
                start, end = max(0, size - n), size - 1
            else:
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) != "" else size - 1
                end = min(end, size - 1)
            if start >= size or start > end:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                self._status = 416
                return self._log(0)
            status = 206
        length = end - start + 1 if size else 0
        self._status = status
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(length))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        sent = 0
        if send_body and length:
            with open(full, "rb") as f:
                f.seek(start)
                remaining = length
                while remaining > 0:
                    chunk = f.read(min(CHUNK, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    sent += len(chunk)
                    remaining -= len(chunk)
        self._log(sent)

    def _tree(self, rel):
        full = safe_join("/" + rel)
        if full is None or not os.path.isdir(full):
            return self._error(404, b"not found")
        entries = []
        for name in sorted(os.listdir(full)):
            child = os.path.join(full, name)
            path = f"{rel}/{name}" if rel else name
            if os.path.isdir(child):
                entries.append({"type": "directory", "path": path, "size": 0})
            elif os.path.isfile(child):
                entries.append({"type": "file", "path": path, "size": os.path.getsize(child)})
        body = json.dumps(entries).encode()
        self._status = 200
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        self._log(len(body))


def main():
    global ROOT
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ROOT = os.path.abspath(sys.argv[1])
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"PORT {server.server_address[1]}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
