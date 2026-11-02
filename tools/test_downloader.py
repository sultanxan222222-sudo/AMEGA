#!/usr/bin/env python3
"""Build and exercise the PS4 downloader against a local HTTP Range test server."""

from __future__ import annotations

import hashlib
import os
import shutil
import socket
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEST_ROOT = Path(tempfile.gettempdir()) / "amega-downloader-integration"


class TestServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, handler, payload: bytes, digest: str):
        super().__init__(address, handler)
        self.payload = payload
        self.digest = digest
        self.interrupted_once = False
        self.request_offsets: list[int] = []
        self.state_lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, _format: str, *_args) -> None:
        pass

    def do_GET(self) -> None:
        server: TestServer = self.server  # type: ignore[assignment]
        if self.path != "/api/v1/file":
            self.send_error(404)
            return
        if self.headers.get("X-AMEGA-Token") != "a" * 64:
            self.send_error(401)
            return
        if self.headers.get("If-Range") != f'"{server.digest}"':
            self.send_error(412)
            return

        range_value = self.headers.get("Range", "")
        if not range_value.startswith("bytes=") or not range_value.endswith("-"):
            self.send_error(416)
            return
        try:
            start = int(range_value[6:-1])
        except ValueError:
            self.send_error(416)
            return
        if start < 0 or start >= len(server.payload):
            self.send_error(416)
            return

        with server.state_lock:
            server.request_offsets.append(start)
            interrupt_this_request = not server.interrupted_once
            if interrupt_this_request:
                server.interrupted_once = True

        end = len(server.payload) - 1
        remaining = server.payload[start:]
        self.send_response(206)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(remaining)))
        self.send_header("Content-Range", f"bytes {start}-{end}/{len(server.payload)}")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", f'"{server.digest}"')
        self.end_headers()

        try:
            if interrupt_this_request:
                # First attempt deliberately ends mid-range. The second
                # request must start at the actual partial-file size.
                first_part = remaining[:600_000]
                self.wfile.write(first_part)
                self.wfile.flush()
                self.close_connection = True
                try:
                    self.connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                return

            # Send in several chunks to exercise the streaming write path.
            for offset in range(0, len(remaining), 128 * 1024):
                self.wfile.write(remaining[offset:offset + 128 * 1024])
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            return


def main() -> int:
    payload = bytes(((index * 17 + 3) & 0xFF) for index in range(4 * 1024 * 1024 + 123))
    digest = hashlib.sha256(payload).hexdigest()

    shutil.rmtree(TEST_ROOT, ignore_errors=True)
    binary = Path(tempfile.gettempdir()) / "amega-downloader-integration-test"

    define = f'-DAMEGA_DOWNLOAD_TEST_ROOT="{TEST_ROOT.as_posix()}"'
    compile_command = [
        "g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
        define,
        str(ROOT / "tools" / "test_downloader.cpp"),
        str(ROOT / "src" / "ps4" / "receiver" / "downloader.cpp"),
        "-o", str(binary),
    ]
    subprocess.run(compile_command, cwd=ROOT, check=True)

    server = TestServer(("127.0.0.1", 0), Handler, payload, digest)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        subprocess.run(
            [str(binary), str(server.server_address[1]), str(len(payload)), digest],
            cwd=ROOT, check=True,
        )
        if len(server.request_offsets) < 2 or server.request_offsets[0] != 0:
            raise RuntimeError(f"Unexpected HTTP requests: {server.request_offsets}")
        if not (0 < server.request_offsets[1] < len(payload)):
            raise RuntimeError(f"Resume did not start at a partial offset: {server.request_offsets}")
        print(f"Range resume verified: first=0, resumed={server.request_offsets[1]}, total={len(payload)}")
        return 0
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
        try:
            binary.unlink()
        except FileNotFoundError:
            pass
        shutil.rmtree(TEST_ROOT, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
