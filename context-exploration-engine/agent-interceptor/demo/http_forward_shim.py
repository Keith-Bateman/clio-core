#!/usr/bin/env python3
"""Minimal HTTP -> Chimaera IPC forward shim for the dt_provenance proxy pool.

The proxy ChiMod's own HTTP listener (HttpProxyServer / cpp-httplib) is
legacy/test-only -- production Create() never calls Start(). The real,
intended path (per proxy_runtime.h's own docstring) is: an HTTP front-end
translates agent requests into a Monitor() query against pool 800.0 encoding
`{"action": "forward", ...}`, which proxy_runtime.cc's HandleForwardAction
dispatches to ForwardHttpTask (method kForwardHttp) and returns a
msgpack-encoded {"status", "headers", "body"} response. It also stores an
InteractionRecord via the tracker automatically -- no extra step needed here.

This is a deliberately minimal, single-purpose implementation of just that
forward path (no checkpoint/rollback/semantic-check routes, unlike the full
983-line context-visualizer/context_visualizer/api/llm_dispatch.py bridge in
clio-core-dtio-old). If those features are wanted later, port them as
additional routes on top of this -- the IPC contract is identical.

Usage:
    LD_LIBRARY_PATH=<build>/bin PYTHONPATH=<build>/bin \
        python3 http_forward_shim.py [--port 9090] [--provider ollama]

Route contract (matches dt_provenance::protocol::ExtractSession):
    POST /_session/{session_id}/<rest-of-path>
    -> forwarded to the configured upstream provider at <rest-of-path>,
       response relayed back verbatim (status/headers/body).
"""
from __future__ import annotations

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import msgpack

PROVIDER = "ollama"  # only provider SWE-bench's harness needs; see SelectUpstream()
MONITOR_TIMEOUT_S = 300  # LLM forwards can be slow -- match _monitor_nonblocking's default


def _ensure_init():
    import clio_runtime_ext as chi  # noqa: local import -- must happen after LD_LIBRARY_PATH is set
    if not getattr(_ensure_init, "_done", False):
        ok = chi.clio_init(0)  # 0 == kClient
        if not ok:
            raise RuntimeError("clio_init(kClient) failed -- is dt_demo_server running?")
        _ensure_init._done = True
    return chi


def _extract_session(path: str):
    """Mirror dt_provenance::protocol::ExtractSession (protocol/src/session.cc)."""
    prefix = "/_session/"
    if not path.startswith(prefix):
        return None
    remainder = path[len(prefix):]
    if not remainder:
        return None
    slash = remainder.find("/")
    if slash == -1:
        return remainder, "/"
    return remainder[:slash], remainder[slash:]


class ForwardHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quieter default logging
        sys.stderr.write("[shim] " + (fmt % args) + "\n")

    def do_POST(self):
        session = _extract_session(self.path)
        if session is None:
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b'{"error":"expected /_session/{id}/... path"}')
            return
        session_id, stripped_path = session

        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8", errors="replace") if length else ""
        headers = {k: v for k, v in self.headers.items() if k.lower() != "content-length"}

        query = {
            "action": "forward",
            "session_id": session_id,
            "provider": PROVIDER,
            "path": stripped_path,
            "headers": headers,
            "body": body,
        }
        query_json = json.dumps(query)

        try:
            chi = _ensure_init()
            task = chi.async_monitor("local", f"pool_stats://800.0:local:{query_json}")
            results = task.wait(MONITOR_TIMEOUT_S)
        except Exception as e:  # noqa: broad -- surface as a 502 to the agent, not a crash
            self.send_response(502)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"error": f"shim/IPC failure: {e}"}).encode())
            return

        if not results:
            self.send_response(502)
            self.end_headers()
            self.wfile.write(b'{"error":"empty Monitor() result"}')
            return

        raw = next(iter(results.values()))
        decoded = msgpack.unpackb(raw, raw=False) if isinstance(raw, (bytes, bytearray)) else raw

        status = decoded.get("status", 502)
        resp_body = decoded.get("body", "")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        body_bytes = resp_body.encode("utf-8") if isinstance(resp_body, str) else resp_body
        self.send_header("Content-Length", str(len(body_bytes)))
        self.end_headers()
        self.wfile.write(body_bytes)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9090)
    ap.add_argument("--provider", default="ollama")
    args = ap.parse_args()

    global PROVIDER
    PROVIDER = args.provider

    _ensure_init()  # fail fast if the runtime isn't reachable, before binding the socket

    server = ThreadingHTTPServer(("127.0.0.1", args.port), ForwardHandler)
    print(f"[shim] listening on 127.0.0.1:{args.port}, forwarding provider={PROVIDER}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
