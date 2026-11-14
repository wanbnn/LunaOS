#!/usr/bin/env python3
"""Exercise curl-luna HTTP framing on the host; this does NOT validate LunaOS TLS/QEMU."""
from __future__ import annotations
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[1]


class Fixture(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def respond(self, payload: bytes, status: int = 200, **headers):
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(payload)))
        for key, value in headers.items():
            self.send_header(key.replace("_", "-"), value)
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        if self.path in ("/ok?value=1", "/nested/ok", "/ok"):
            self.respond(b"OK")
        elif self.path == "/nested/redirect":
            self.respond(b"", status=302, Location="ok")
        elif self.path == "/auth-redirect":
            # Different origin (hostname changes); Basic credentials must NOT follow.
            self.respond(b"", status=302,
                         Location=f"http://localhost:{self.server.server_port}/capture")
        elif self.path == "/capture":
            self.respond(b"LEAK" if self.headers.get("Authorization") else b"NO-LEAK",
                         status=400 if self.headers.get("Authorization") else 200)
        elif self.path == "/userpwd":
            import base64
            expected = "Basic " + base64.b64encode(b"tester:secret").decode("ascii")
            self.respond(b"USERPWD-OK" if self.headers.get("Authorization") == expected
                         else b"INVALID AUTH",
                         status=200 if self.headers.get("Authorization") == expected else 401)
        elif self.path == "/informational":
            self.send_response_only(103, "Early Hints")
            self.end_headers()
            self.respond(b"OK")
        else:
            self.respond(b"NOT FOUND", status=404)

    def do_POST(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            payload = bytearray()
            while True:
                line = self.rfile.readline().strip().split(b";", 1)[0]
                amount = int(line, 16)
                if not amount:
                    # The terminating chunk is followed by an empty trailer line.
                    while self.rfile.readline().strip():
                        pass
                    break
                payload += self.rfile.read(amount)
                assert self.rfile.read(2) == b"\r\n"
            body = bytes(payload)
        else:
            size = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(size)
        if self.path == "/binary" and body == b"a\x00bc":
            self.respond(b"BINARY-OK")
        elif self.path == "/sized" and body == b"stream-data" and \
                self.headers.get("Content-Length") == "11":
            self.respond(b"SIZED-OK")
        elif self.path == "/chunked" and body == b"stream-data" and \
                self.headers.get("Transfer-Encoding") == "chunked":
            self.respond(b"CHUNKED-OK")
        else:
            self.respond(b"INVALID POST", status=400)


def main():
    with TemporaryDirectory(prefix="luna-curl-host-") as tmp:
        binary = Path(tmp) / "curl-luna-tests"
        subprocess.run(
            ["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra",
             "-DLUNA_CURL_HOST_TEST",
             "-I", str(ROOT / "sdk/curl-luna/include"),
             str(ROOT / "sdk/curl-luna/curl-luna.c"),
             str(ROOT / "tests/curl_luna_host.c"),
             "-lz", "-o", str(binary)],
            check=True, cwd=ROOT)
        server = ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            subprocess.run([str(binary), str(server.server_port)],
                           check=True, cwd=ROOT, timeout=30)
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)


if __name__ == "__main__":
    main()
