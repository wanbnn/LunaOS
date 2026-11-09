#!/usr/bin/env python3
"""Native verified TLS; the temporary private CA is never part of the release image."""

import hashlib
import http.server
import json
import os
import shutil
import ssl
import subprocess
import threading
import time

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
large_download = b"<html>" + b"x" * 64000 + b"</html>"
text_tools = b"a" * 65534 + b"needle\n" + b"".join(f"line-{i:02d}\n".encode() for i in range(1, 21))
stream_bytes = int(os.environ.get("LUNA_DOWNLOAD_BYTES", 3 * 1024 * 1024))
stream_ram = int(os.environ.get("LUNA_DOWNLOAD_RAM", 64))


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
            body = (
                f'<html><head><link rel="stylesheet" href="large.css"><link rel="stylesheet" href="//luna.test:{self.server.server_port}/extra.css"></head><body><p id="network-proof">Large CSS received</p></body></html>'
            ).encode()
        elif self.path == "/nested/large.css":
            body = large_css
        elif self.path == "/extra.css":
            body = extra_css
        elif self.path == "/nested/script-page?q=luna&mode=test":
            body = b"""
                <style>#proof::after { content: "ready" }</style>
                <p id="proof">before</p>
                <script>var order = "inline";</script>
                <script src="first.js"></script>
                <script src="missing.js"></script>
                <script src="/last.js"></script>
                <script>
                    document.addEventListener("DOMContentLoaded", function ready() {
                        document.removeEventListener("DOMContentLoaded", ready);
                        var proof = document.getElementById("proof");
                        if (!matchMedia("screen and (min-width: 100px)").matches ||
                            !getComputedStyle(proof, "::after").getPropertyValue("content").includes("ready"))
                            throw new Error("screen CSS after load");
                        var interval = setInterval(() => {
                            clearInterval(interval);
                            setTimeout(() => console.log(order + ":" + proof.textContent), 10);
                        }, 10);
                    });
                </script>"""
        elif self.path == "/nested/first.js":
            body = b"""
                if (window.document !== document || !document.currentScript)
                    throw new Error("script context");
                var holder = document.createElement("div");
                holder.innerHTML = " <a>first</a><a>second</a>";
                if (holder.getElementsByTagName("a").length !== 2 ||
                    holder.querySelector("a").parentNode !== holder)
                    throw new Error("fragment DOM");
                order += "-external";
            """
        elif self.path == "/nested/missing.js":
            self.send_error(404)
            return
        elif self.path == "/last.js":
            body = b'document.getElementById("proof").textContent = "updated"; order += "-after-error";'
        elif self.path == "/large-chunked":
            body = large_download
        elif self.path == "/text-tools":
            body = text_tools
        elif self.path == "/oversized-chunked":
            body = b"x" * stream_bytes
        chunked = self.path in {
            "/chunked",
            "/large-chunked",
            "/oversized-chunked",
            "/truncated-chunked",
            "/truncated-trailers",
        }
        self.send_response(200)
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header(
                "Content-Length", str(len(body) + (10 if self.path == "/truncated" else 0))
            )
        self.end_headers()
        if self.path == "/truncated-chunked":
            self.wfile.write(f"{len(body) + 10:X}\r\n".encode() + body)
        elif self.path == "/truncated-trailers":
            self.wfile.write(f"{len(body):X}\r\n".encode() + body + b"\r\n0\r\n")
        elif chunked:
            for offset in range(0, len(body), 17000):
                part = body[offset : offset + 17000]
                self.wfile.write(f"{len(part):X};fixture=test\r\n".encode() + part + b"\r\n")
            self.wfile.write(b"0\r\nX-Fixture: complete\r\n\r\n")
        elif self.path == "/nested/large.css":
            self.wfile.write(body[:1024])
            self.wfile.flush()
            time.sleep(2.5)
            self.wfile.write(body[1024:])
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
with (TEST / "tls.img").open("r+b") as disk:
    disk.truncate(max(128 * 1024 * 1024, 66 * 1024 * 1024 + stream_bytes * 2 + 16 * 1024 * 1024))
g = Guest(TEST / "tls.img", ram=stream_ram)
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
    g.command(
        f"wget https://luna.test:{secured.server_port}/large-chunked /tmp/large.html",
        f"Saved {len(large_download)} bytes",
        timeout=35,
    )
    g.command("wc /tmp/large.html", str(len(large_download)))
    g.command(
        f"wget https://luna.test:{secured.server_port}/oversized-chunked /tmp/multi-megabyte",
        f"Saved {stream_bytes} bytes",
        timeout=max(120, stream_bytes // 32768),
    )
    g.command("wc /tmp/multi-megabyte", str(stream_bytes), timeout=120)
    g.command(
        f"wget https://luna.test:{secured.server_port}/text-tools /tmp/tools", "Saved", timeout=60
    )
    g.command("head /tmp/tools", "aaaa")
    g.command("grep needle /tmp/tools > /tmp/grep-result && echo GREP_OK", "\nGREP_OK\n")
    g.command("tail /tmp/tools", "line-11\n")
    g.command("mv /tmp/tools /tmp/renamed")
    g.command("cp /tmp/renamed /tmp/copied", timeout=60)
    g.command("tail /tmp/copied", "line-11\n")

    g.command(
        "write /home/luna/grow.ljs \"for(let i=0;i<100;i++) luna.fs.write('entry-'+i,'persisted'); luna.fs.read('entry-99')\""
    )
    g.command("dev run /home/luna/grow.ljs", "persisted")
    capacity = int.from_bytes(memory(g, symbol_address("fs_file_capacity"), 4), "little")
    assert capacity > 64, capacity
    g.command("sync", "committed", timeout=max(60, stream_bytes // 32768))
    g.command("write /tmp/protected original")
    for path, expected in [
        ("truncated-chunked", "Incomplete chunked response"),
        ("truncated-trailers", "Incomplete chunked response trailers"),
    ]:
        g.command(
            f"wget https://luna.test:{secured.server_port}/{path} /tmp/protected",
            expected,
            timeout=35,
        )
        g.command("cat /tmp/protected", "original")
    g.command(
        f"wget https://luna.test:{secured.server_port}/ /tmp/after-limit", "Saved", timeout=35
    )
    g.command("ping 10.0.2.2", "Reply", timeout=15)
    style_url = f"https://luna.test:{secured.server_port}/style-page"
    g.command(f"browse {style_url}")
    final_url = f"https://luna.test:{secured.server_port}/nested/style-page"
    wait_until(
        lambda: memory(g, symbol_address("browser_url"), 256).split(b"\0")[0].decode() == final_url,
        60,
    )
    wait_until(lambda: memory(g, symbol_address("browser_loading"), 1) == b"\0", 60)
    css_size = int.from_bytes(memory(g, symbol_address("sheet_length"), 4), "little")
    css_ptr = int.from_bytes(memory(g, symbol_address("sheet_text"), 4), "little")
    expected_css = large_css + b"\n" + extra_css + b"\n"
    if css_size != len(expected_css):
        print(g.command("net"), flush=True)
    assert css_size == len(expected_css), css_size
    assert memory(g, css_ptr, len(expected_css)) == expected_css
    print("PASS: browser receives a complete 350 KB linked stylesheet over TLS", flush=True)
    g.command(f"browse https://luna.test:{secured.server_port}/nested/script-page?q=luna&mode=test")
    g.wait("[page] inline-external-after-error:updated", 60)
    print("PASS: browse preserves query arguments and executes linked scripts in order", flush=True)
    g.command(f"wget https://luna.test:{secured.server_port}/ /tmp/after-css", "Saved", timeout=35)
    g.command("ping 10.0.2.2", "Reply", timeout=15)
    g.command(
        f"wget https://luna.test:{secured.server_port}/truncated /tmp/bad", "incomplete", timeout=35
    )
    status = g.command("net")
    metrics = {}
    for line in status.splitlines():
        if ": " in line:
            name, value = line.split(": ", 1)
            if value.isdigit():
                metrics[name] = int(value)
    assert metrics["TCP receive buffer bytes"] == 0, metrics
    assert metrics["TCP reorder memory bytes"] == 0, metrics
    assert 16384 < metrics["TCP peak buffer bytes"] <= metrics["TCP receive budget bytes"], metrics
    if not metrics["TCP receive scale"]:
        assert metrics["TCP peak buffer bytes"] <= 65535, metrics
    assert 1000 <= metrics["TCP RTO ms"] <= 60000, metrics
    g.command("js \"[1,2,3].map(x=>x*2).join(',')\"", "2,4,6")
    g.command('js "123n ** 3n"', "1860867")
    g.command("js \"Object.fromEntries([['ok',42]]).ok\"", "42")
    g.command('js "while(true){}"', "interrupted")
    # Restart the real guest and verify that the expanded snapshot is mounted.
    g.close()
    g = Guest(TEST / "tls.img", network=False, ram=stream_ram)
    g.command("wc /tmp/multi-megabyte", str(stream_bytes), timeout=120)
    g.command("cat /home/luna/entry-99", "persisted")
    report = {
        "result": "PASS",
        "image_sha256": hashlib.sha256((ROOT / "dist/LunaOS.img").read_bytes()).hexdigest(),
        "verified": [
            "native TLS 1.2 certificate chain and DNS-name validation",
            "adaptive TCP buffer growth and release after close",
            "RTT-derived retransmission timer within safety bounds",
            "untrusted CA rejected",
            "hostname mismatch rejected",
            "authenticated HTTPS download",
            "chunked transfer decoded",
            "64 KB chunked wget saved beyond the old 8 KiB command buffer",
            f"{stream_bytes} byte chunked wget streamed to disk with {stream_ram} MiB guest RAM",
            "more than 64 directory entries saved",
            "head, tail, cross-block grep, binary copy and rename of disk-backed files",
            "expanded snapshot and multi-megabyte file survive guest restart",
            "incomplete chunk trailers rejected",
            "existing file preserved on failed downloads",
            "HTTPS and ping still work after multi-megabyte download",
            "truncated HTTP body rejected",
            "complete 350 KB linked CSS transferred to the browser over TLS after a 2.5-second receive pause",
            "subsequent HTTPS and ICMP after large CSS",
            "QuickJS native ECMAScript runtime",
            "BigInt and modern JS syntax",
            "bounded execution of infinite script",
        ],
        "not_implemented": [
            "TLS 1.3",
            "full browser standards compliance",
            "production browser sandbox",
        ],
        "not_validated": ["physical RTL8822BE ACK delivery and MitraStar link stability"],
    }
    (ROOT / "dist/web-foundation-report.json").write_text(json.dumps(report, indent=2) + "\n")
finally:
    g.close()
    plain.shutdown()
    secured.shutdown()
