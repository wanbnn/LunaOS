#!/usr/bin/env python3
"""Run upstream Git Smart HTTP from a real LunaOS QEMU guest.

The Linux host runs only a standard git-http-backend test server, never
the Git commands being tested in the guest. Do not mistake this for verified TLS.
Requires a freshly built dist/LunaOS.img, QEMU, and the host 'git' binary.
"""
from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from tempfile import TemporaryDirectory
from urllib.parse import urlsplit
import os
import shutil
import subprocess
import threading

from guest import Guest, ROOT, TEST


def git(*args, cwd=None):
    return subprocess.check_output(["git", *args], cwd=cwd, text=True,
                                   stderr=subprocess.STDOUT).strip()


class SmartGitBackend(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    project_root: str

    def log_message(self, fmt, *args):
        print("smart-http: " + fmt % args, flush=True)

    def do_GET(self):
        self.serve_backend()

    def do_POST(self):
        self.serve_backend()

    def read_body(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            body = bytearray()
            while True:
                line = self.rfile.readline().strip().split(b";", 1)[0]
                size = int(line, 16)
                if not size:
                    while self.rfile.readline().strip():
                        pass
                    return bytes(body)
                body += self.rfile.read(size)
                if self.rfile.read(2) != b"\r\n":
                    raise ValueError("bad chunk framing")
        length = int(self.headers.get("Content-Length", "0"))
        return self.rfile.read(length)

    def serve_backend(self):
        parsed = urlsplit(self.path)
        request = self.read_body() if self.command == "POST" else b""
        env = os.environ.copy()
        env.update({
            "GIT_PROJECT_ROOT": self.project_root,
            "GIT_HTTP_EXPORT_ALL": "1",
            "PATH_INFO": parsed.path,
            "QUERY_STRING": parsed.query,
            "REQUEST_METHOD": self.command,
            "CONTENT_TYPE": self.headers.get("Content-Type", ""),
            "CONTENT_LENGTH": str(len(request)),
            "REMOTE_ADDR": "127.0.0.1",
            "SERVER_PROTOCOL": "HTTP/1.1",
        })
        p = subprocess.run(["git", "http-backend"], input=request,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           env=env, check=False, timeout=60)
        head, sep, body = p.stdout.partition(b"\r\n\r\n")
        if not sep:
            head, sep, body = p.stdout.partition(b"\n\n")
        if not sep:
            self.send_error(502, "git-http-backend failed")
            raise RuntimeError(p.stderr.decode(errors="replace"))
        status = 200
        response_headers = []
        for raw in head.splitlines():
            key, _, value = raw.partition(b":")
            if key.lower() == b"status":
                status = int(value.strip().split(None, 1)[0])
            elif key:
                response_headers.append((key.decode("ascii"), value.strip().decode("latin-1")))
        self.send_response(status)
        for key, value in response_headers:
            if key.lower() not in ("content-length", "connection", "transfer-encoding"):
                self.send_header(key, value)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        finally:
            self.close_connection = True


def main():
    with TemporaryDirectory(prefix="luna-smart-git-") as tmp:
        root = Path(tmp)
        bare = root / "example.git"
        source = root / "source"
        git("init", "--bare", str(bare))
        git("config", "http.receivepack", "true", cwd=bare)
        git("init", "-b", "main", str(source))
        git("config", "user.name", "Test Fixture", cwd=source)
        git("config", "user.email", "fixture@example.invalid", cwd=source)
        (source / "README.md").write_text("Created by the host fixture.\n")
        git("add", "README.md", cwd=source)
        git("commit", "-m", "fixture", cwd=source)
        git("remote", "add", "origin", str(bare), cwd=source)
        git("push", "-u", "origin", "main", cwd=source)
        git("symbolic-ref", "HEAD", "refs/heads/main", cwd=bare)
        before = git("rev-parse", "main", cwd=bare)

        SmartGitBackend.project_root = str(root)
        server = ThreadingHTTPServer(("127.0.0.1", 0), SmartGitBackend)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        disk = TEST / "git-http-disk.img"
        shutil.copyfile(ROOT / "dist/LunaOS.img", disk)
        guest = None
        try:
            guest = Guest(disk, ram=int(os.environ.get("LUNA_GIT_TEST_RAM", "128")))
            url = f"http://10.0.2.2:{server.server_port}/example.git"
            guest.command(f"git ls-remote {url}", "refs/heads/main", timeout=90)
            guest.command("echo $?", "\n0\n")
            guest.command(f"git clone {url} /home/luna/git-http-copy", timeout=120)
            guest.command("echo $?", "\n0\n")
            guest.command("cat /home/luna/git-http-copy/README.md",
                          "Created by the host fixture.")
            guest.command("git -C /home/luna/git-http-copy config user.name Luna")
            guest.command("git -C /home/luna/git-http-copy config user.email luna@example.invalid")
            guest.command("write /home/luna/git-http-copy/new.txt Git-Smart-HTTP")
            guest.command("git -C /home/luna/git-http-copy add new.txt", timeout=45)
            guest.command("git -C /home/luna/git-http-copy commit -m http-push", timeout=60)
            guest.command("echo $?", "\n0\n")
            guest.command("git -C /home/luna/git-http-copy push origin main", timeout=120)
            guest.command("echo $?", "\n0\n")
            after = git("rev-parse", "main", cwd=bare)
            assert after != before, "the server did not receive the guest push"
            guest.command("git -C /home/luna/git-http-copy fetch origin", timeout=120)
            guest.command("echo $?", "\n0\n")
            guest.command("sync", timeout=60)
            print("PASS: guest Git Smart HTTP ls-remote/clone/push/fetch", flush=True)
        finally:
            if guest:
                guest.close()
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
