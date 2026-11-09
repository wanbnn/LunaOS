"""Install real wheels and exercise LunaOS pip against a deterministic index."""
import base64
import hashlib
import io
import json
import shutil
import threading
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from guest import ROOT, TEST, Guest


def wheel(project, version, files, requires=""):
    info = f"{project}-{version}.dist-info"
    metadata = f"Metadata-Version: 2.1\nName: {project}\nVersion: {version}\n{requires}\n"
    members = {
        **files,
        f"{info}/METADATA": metadata,
        f"{info}/WHEEL": "Wheel-Version: 1.0\nRoot-Is-Purelib: true\nTag: py3-none-any\n",
    }
    entries = f"{info}/RECORD,,\n"
    for name, text in members.items():
        digest = base64.urlsafe_b64encode(hashlib.sha256(text.encode()).digest()).decode().rstrip("=")
        entries += f"{name},sha256={digest},{len(text.encode())}\n"
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, text in members.items():
            archive.writestr(name, text)
        archive.writestr(f"{info}/RECORD", entries)
    return output.getvalue()


class Fixture(BaseHTTPRequestHandler):
    def do_GET(self):
        data = self.server.resources.get(self.path)
        if data is None:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def main():
    disk = TEST / "python-pip.img"
    shutil.copyfile(ROOT / "dist/LunaOS.img", disk)

    with ThreadingHTTPServer(("127.0.0.1", 0), Fixture) as server:
        base = f"http://10.0.2.2:{server.server_port}"
        demo = "lunademo-1.0-py3-none-any.whl"
        corrupted = "lunademo-0.9-py3-none-any.whl"
        helper = "helper-2.0-py3-none-any.whl"
        bad = "bad-1.0-py3-none-any.whl"
        deps = "withdeps-1.0-py3-none-any.whl"
        demo_zip = wheel("lunademo", "1.0", {"lunademo/__init__.py": "answer = 42\n"})
        helper_zip = wheel("helper", "2.0", {"helper.py": "answer = 7\n"})

        def index(name, data, digest=None):
            return json.dumps({
                "urls": [{"filename": name, "url": f"{base}/{name}", "yanked": False,
                          "digests": {"sha256": digest or hashlib.sha256(data).hexdigest()}}]
            }).encode()

        server.resources = {
            f"/{demo}": demo_zip,
            f"/{helper}": helper_zip,
            f"/{corrupted}": demo_zip,
            f"/{bad}": wheel("bad", "1.0", {"staged.py": "value = 10\n", "../escape.py": "bad\n"}),
            f"/{deps}": wheel("withdeps", "1.0", {"withdeps.py": "value = 2\n"},
                             "Requires-Dist: helper\n"),
            "/api/lunademo/1.0/json": index(demo, demo_zip),
            "/api/lunademo/0.9/json": index(corrupted, demo_zip, "0" * 64),
            "/requirements.txt": f"# local and index\n\n {helper}  # fixture\n lunademo==1.0\r\n".encode(),
        }
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        guest = Guest(disk)
        try:
            guest.command("pip --version", "LunaOS pip 1")
            guest.command("python -m venv .venv", "Created Python environment")
            guest.command("venv activate .venv", "Activated /home/luna/.venv")
            guest.command("mkdir wheelhouse")
            guest.command(f"wget {base}/{helper} wheelhouse/{helper}", "Saved")
            guest.command(f"wget {base}/requirements.txt wheelhouse/requirements.txt", "Saved")
            guest.command(f"pip install --index-url {base}/api lunademo==0.9",
                          "wheel SHA-256 mismatch")
            guest.command(f"python -m pip install -r wheelhouse/requirements.txt --index-url {base}/api",
                          f"Installed {demo}")
            guest.command("python -c 'import lunademo, helper; print(lunademo.answer + helper.answer)'",
                          "49\n")
            guest.command("pip list", "lunademo 1.0")
            guest.command("pip freeze", "helper==2.0")
            guest.command("pip show LunaDemo", "Location: /home/luna/.venv/lib")
            guest.command("pip cache dir", "/home/luna/.pip-cache")
            guest.command("pip cache info", "Cached wheels: 1")
            guest.command("pip cache list", demo)
            guest.command(f"pip install {base}/{bad}", "invalid or unsupported ZIP member")
            guest.command("python -c 'import staged'", "ImportError")
            guest.command("ls .venv", "pyvenv.cfg")
            guest.command(f"pip install {base}/{deps}", "wheel declares dependencies")
            guest.command(f"pip install --no-deps {base}/{deps}", f"Installed {deps}")
            guest.command("pip uninstall -y withdeps", "Uninstalled withdeps")
            guest.command("pip uninstall -y LunaDemo", "Uninstalled lunademo")
            guest.command("python -c 'import lunademo'", "ImportError")
            guest.command(f"pip install lunademo==1.0 --index-url {base}/api", "Using cached wheel")
            guest.command("pip cache remove lunademo", "Files removed: 1")
            guest.command("pip uninstall lunademo", "Uninstalled lunademo")
            guest.command(f"pip install lunademo==1.0 --index-url {base}/api", f"Installed {demo}")
            guest.command("pip cache purge", "Files removed: 1")
            guest.command("pip cache info", "Cached wheels: 0")
            guest.command("deactivate", "deactivated")
            guest.command("pip list", "(no packages installed)")
            guest.command("python -c 'import lunademo'", "ImportError")
            guest.command("sync", "snapshot committed to disk")
        finally:
            guest.close()
            server.shutdown()
            thread.join()

    guest = Guest(disk, network=False)
    try:
        guest.command("venv activate .venv", "Activated")
        guest.command("python -c 'import lunademo; print(lunademo.answer)'", "42\n")
        guest.command("pip uninstall lunademo", "Uninstalled lunademo")
        guest.command("python -c 'import helper; print(helper.answer)'", "7\n")
    finally:
        guest.close()


if __name__ == "__main__":
    main()
