#!/usr/bin/env python3
"""Minimal smart-HTTP server: forwards requests to `git http-backend`.

Usage: python3 http_backend.py <port> <project_root_dir>
"""

import os
import subprocess
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.abspath(sys.argv[2])


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def _run(self, body=None):
        path = self.path.split("?")[0]
        query = ""
        if "?" in self.path:
            query = self.path.split("?", 1)[1]
        env = dict(
            os.environ,
            GIT_PROJECT_ROOT=ROOT,
            PATH_INFO=path,
            QUERY_STRING=query,
            REQUEST_METHOD=self.command,
            CONTENT_TYPE=self.headers.get("Content-Type", ""),
            CONTENT_LENGTH=str(len(body) if body else 0),
            GIT_HTTP_EXPORT_ALL="1",
            REMOTE_ADDR="127.0.0.1",
        )
        proc = subprocess.run(
            ["git", "http-backend"], input=body, env=env, capture_output=True
        )
        out = proc.stdout
        if proc.stderr:
            print("http-backend stderr:", proc.stderr.decode(errors="replace"), flush=True)
        # Split CGI headers from body.
        header, _, payload = out.partition(b"\r\n\r\n")
        status = 200
        for line in header.split(b"\r\n"):
            if line.lower().startswith(b"status:"):
                status = int(line.split(b":")[1].strip().split()[0])
        self.send_response(status)
        self.send_header("Content-Type", "application/x-git-result")
        self.send_header("Content-Length", str(len(payload)))
        # Browser clients (the WASM simulator) fetch cross-origin.
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()
        self.wfile.write(payload)
        return status, payload

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        print(f"GET {self.path}", flush=True)
        self._run()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        print(f"POST {self.path} len={length}", flush=True)
        status, payload = self._run(body)
        print(f"POST {self.path} -> {status} resp={payload[:200]!r}", flush=True)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
