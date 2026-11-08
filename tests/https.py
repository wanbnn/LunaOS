#!/usr/bin/env python3
"""Native verified TLS; the temporary private CA is never part of the release image."""

import hashlib
import http.server
import json
import shutil
import ssl
import subprocess
import threading

from guest import ROOT, TEST, Guest, memory, symbol_address, wait_until


def openssl(args):
    subprocess.run(
        ["openssl", *args], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )


openssl(
    [
        "req",
        "-x509",
        "-newkey",
        "rsa:2048",
        "-nodes",
        "-keyout",
        str(TEST / "root.key"),
        "-out",
        str(TEST / "root.pem"),
        "-days",
        "2",
        "-subj",
        "/CN=Luna temporary test CA",
        "-addext",
        "basicConstraints=critical,CA:TRUE",
    ]
)
openssl(
    [
        "req",
        "-newkey",
        "rsa:2048",
        "-nodes",
        "-keyout",
        str(TEST / "server.key"),
        "-out",
        str(TEST / "server.csr"),
        "-subj",
        "/CN=luna.test",
    ]
)
(TEST / "extensions").write_text("subjectAltName=DNS:luna.test\nbasicConstraints=CA:FALSE\n")
openssl(
    [
        "x509",
        "-req",
        "-in",
        str(TEST / "server.csr"),
        "-CA",
        str(TEST / "root.pem"),
        "-CAkey",
        str(TEST / "root.key"),
        "-CAcreateserial",
        "-out",
        str(TEST / "server.pem"),
        "-days",
        "2",
        "-extfile",
        str(TEST / "extensions"),
    ]
)


large_css = b"/*" + b" " * 350000 + b"*/\n#network-proof { color: #123456; }"
extra_css = b"p { margin: 1px; }"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        print("Fixture GET:", self.path, flush=True)
        if self.path == "/style-page":
            self.send_response(302)
            self.send_header("Location", f"//luna.test:{self.server.server_port}/nested/style-page")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        body = (
            (TEST / "root.pem").read_bytes()
            if self.path == "/ca.pem"
            else b"<h1>NATIVE VERIFIED TLS WORKS</h1>"
        )
        if self.path == "/nested/style-page":
            body = (f'<html><head><link rel="stylesheet" href="large.css"><link rel="stylesheet" href="//luna.test:{self.server.server_port}/extra.css"></head><body><p id="network-proof">Large CSS received</p></body></html>').encode()
        elif self.path == "/nested/large.css":
            body = large_css
        elif self.path == "/extra.css":
            body = extra_css
        self.send_response(200)
        if self.path == "/chunked":
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header(
                "Content-Length", str(len(body) + (10 if self.path == "/truncated" else 0))
            )
        self.end_headers()
        if self.path == "/chunked":
            self.wfile.write(f"{len(body):X}\r\n".encode() + body + b"\r\n0\r\n\r\n")
        else:
            self.wfile.write(body)

    def log_message(self, *a):
        pass


plain = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
secured = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.maximum_version = ssl.TLSVersion.TLSv1_2
ctx.load_cert_chain(TEST / "server.pem", TEST / "server.key")
secured.socket = ctx.wrap_socket(secured.socket, server_side=True)
for server in [plain, secured]:
    threading.Thread(target=server.serve_forever, daemon=True).start()
shutil.copyfile(ROOT / "dist/LunaOS.img", TEST / "tls.img")
g = Guest(TEST / "tls.img")
try:
    g.command('write /etc/hosts "10.0.2.2 luna.test"')
    g.command(f"wget https://luna.test:{secured.server_port}/ /tmp/untrusted", "failed", timeout=35)
    g.command(f"wget http://10.0.2.2:{plain.server_port}/ca.pem /etc/tls/custom.pem", "Saved")
    g.command(f"wget https://luna.test:{secured.server_port}/ /tmp/secure", "Saved", timeout=35)
    g.command("cat /tmp/secure", "NATIVE VERIFIED TLS WORKS")
    g.command(f"wget https://10.0.2.2:{secured.server_port}/ /tmp/wrong-host", "failed", timeout=35)
    g.command(
        f"wget https://luna.test:{secured.server_port}/chunked /tmp/chunk", "Saved", timeout=35
    )
    g.command("cat /tmp/chunk", "NATIVE VERIFIED TLS WORKS")
    style_url = f"https://luna.test:{secured.server_port}/style-page"
    g.command(f"browse {style_url}")
    final_url = f"https://luna.test:{secured.server_port}/nested/style-page"
    wait_until(lambda: memory(g, symbol_address("browser_url"), 256).split(b"\0")[0].decode() == final_url, 60)
    wait_until(lambda: memory(g, symbol_address("browser_loading"), 1) == b"\0", 60)
    css_size = int.from_bytes(memory(g, symbol_address("sheet_length"), 4), "little")
    css_ptr = int.from_bytes(memory(g, symbol_address("sheet_text"), 4), "little")
    expected_css = large_css + b"\n" + extra_css + b"\n"
    if css_size != len(expected_css):
        print(g.command("net"), flush=True)
    assert css_size == len(expected_css), css_size
    assert memory(g, css_ptr, len(expected_css)) == expected_css
    print("PASS: browser receives a complete 350 KB linked stylesheet over TLS", flush=True)
    g.command(f"wget https://luna.test:{secured.server_port}/ /tmp/after-css", "Saved", timeout=35)
    g.command("ping 10.0.2.2", "Reply", timeout=15)
    g.command(
        f"wget https://luna.test:{secured.server_port}/truncated /tmp/bad", "incomplete", timeout=35
    )
    g.command("js \"[1,2,3].map(x=>x*2).join(',')\"", "2,4,6")
    g.command('js "123n ** 3n"', "1860867")
    g.command("js \"Object.fromEntries([['ok',42]]).ok\"", "42")
    g.command('js "while(true){}"', "interrupted")
    report = {
        "result": "PASS",
        "image_sha256": hashlib.sha256((ROOT / "dist/LunaOS.img").read_bytes()).hexdigest(),
        "verified": [
            "native TLS 1.2 certificate chain and DNS-name validation",
            "untrusted CA rejected",
            "hostname mismatch rejected",
            "authenticated HTTPS download",
            "chunked transfer decoded",
            "truncated HTTP body rejected",
            "complete 350 KB linked CSS transferred to the browser over TLS",
            "subsequent HTTPS and ICMP after large CSS",
            "QuickJS native ECMAScript runtime",
            "BigInt and modern JS syntax",
            "bounded execution of infinite script",
        ],
        "not_implemented": ["TLS 1.3", "full browser standards compliance", "production browser sandbox"],
        "not_validated": ["physical RTL8822BE ACK delivery and MitraStar link stability"],
    }
    (ROOT / "dist/web-foundation-report.json").write_text(json.dumps(report, indent=2) + "\n")
finally:
    g.close()
    plain.shutdown()
    secured.shutdown()
