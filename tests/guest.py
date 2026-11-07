"""Shared real-machine QEMU harness."""

import json
import os
import select
import shutil
import socket
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
TEST = ROOT / os.environ.get("LUNA_TEST_DIR", "build/test")
TEST.mkdir(exist_ok=True)
UEFI = os.environ.get("LUNA_UEFI") == "1"
ARCH = "x86_64" if UEFI else "i386"
qemu = shutil.which("qemu-system-" + ARCH)
env = os.environ.copy()
firmware = []
if not qemu:
    qemu = str(ROOT / (".tools/usr/bin/qemu-system-" + ARCH))
    env["LD_LIBRARY_PATH"] = str(ROOT / ".tools/usr/lib/x86_64-linux-gnu")
    firmware = [
        "-L",
        str(ROOT / ".tools/usr/share/qemu"),
        "-bios",
        str(ROOT / ".tools/usr/share/seabios/bios-256k.bin"),
    ]
assert Path(qemu).exists(), "Install qemu-system-x86 (host development dependency)"


class Guest:
    def __init__(self, disk, network=True, ram=64, extra_args=None, setup=False, cdrom=False):
        self.serialpath = TEST / "serial.sock"
        self.qmppath = TEST / "qmp.sock"
        for p in [self.serialpath, self.qmppath]:
            p.unlink(missing_ok=True)
        boot_firmware = firmware
        if UEFI:
            ovmf = Path(os.environ.get("LUNA_OVMF_DIR", ROOT / ".tools/uefi/usr/share/OVMF"))
            if not (ovmf / "OVMF_CODE_4M.fd").exists(): ovmf = Path("/usr/share/OVMF")
            variables = TEST / "uefi-vars.fd"
            shutil.copyfile(ovmf / "OVMF_VARS_4M.fd", variables)
            boot_firmware = ["-L", str(ROOT / ".tools/usr/share/qemu"), "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf / 'OVMF_CODE_4M.fd'}", "-drive", f"if=pflash,format=raw,file={variables}"]
            ram = max(ram, 256)
        args = [
            qemu,
            *boot_firmware,
            *(
                ["-accel", "kvm", "-cpu", "host"]
                if os.access("/dev/kvm", os.R_OK | os.W_OK)
                else ["-accel", "tcg", "-cpu", "max"]
            ),
            "-machine",
            "pc",
            "-m",
            f"{ram}M",
            "-vga",
            "std",
            "-display",
            "none",
            "-drive",
            f"file={disk},format=raw,if=ide",
            "-serial",
            f"unix:{self.serialpath},server=on,wait=on",
            "-qmp",
            f"unix:{self.qmppath},server=on,wait=off",
            "-no-reboot",
            "-no-shutdown",
            "-audiodev",
            f"driver=wav,id=a0,path={TEST / 'audio.wav'}",
            "-device",
            "AC97,audiodev=a0",
            "-machine",
            "pcspk-audiodev=a0",
        ]
        if cdrom:
            # UEFI prepends two pflash drives. Replace the OS drive, not
            # the firmware image, when exercising the optical boot path.
            i=args.index(f"file={disk},format=raw,if=ide")-1
            args[i:i+2]=["-cdrom",str(disk),"-boot","order=d"]
        args += (
            ["-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0,romfile="]
            if network
            else ["-nic", "none"]
        )
        if extra_args:
            args += extra_args
        self.log = (TEST / "qemu.log").open("a")
        self.p = subprocess.Popen(args, stdout=self.log, stderr=self.log, env=env)
        deadline = time.monotonic() + 8
        while not self.serialpath.exists():
            if self.p.poll() is not None:
                raise RuntimeError((TEST / "qemu.log").read_text())
            assert time.monotonic() < deadline, "QEMU socket timeout"
            time.sleep(0.02)
        self.serial = socket.socket(socket.AF_UNIX)
        self.serial.connect(str(self.serialpath))
        self.serial.setblocking(False)
        self.shell_ready = False
        self.transcript = ""
        self.buf = ""
        self.qmp = socket.socket(socket.AF_UNIX)
        self.qmp.connect(str(self.qmppath))
        self.qf = self.qmp.makefile("rwb", buffering=0)
        self.qread()
        self.qcommand("qmp_capabilities")
        self.wait("READY", 120 if UEFI else 30)
        if not setup and memory(self, symbol_address("setup_active"), 1) != b"\0":
            self.key("esc")
            wait_until(lambda: memory(self, symbol_address("setup_active"), 1) == b"\0")

    def qread(self):
        while True:
            line = self.qf.readline()
            if not line:
                raise RuntimeError("QMP closed")
            data = json.loads(line)
            if "event" not in data:
                return data

    def qcommand(self, cmd, args=None):
        self.qf.write(
            json.dumps({"execute": cmd, **({"arguments": args} if args else {})}).encode() + b"\n"
        )
        r = self.qread()
        assert "error" not in r, r
        return r

    def read(self, timeout=0.1):
        if select.select([self.serial], [], [], timeout)[0]:
            b = self.serial.recv(65536)
            if not b:
                raise RuntimeError("Serial closed")
            chunk = b.decode(errors="replace").replace("\r", "")
            self.buf += chunk
            self.transcript += chunk

    def wait(self, needle, timeout=15):
        deadline = time.monotonic() + timeout
        while needle not in self.buf:
            assert time.monotonic() < deadline, (
                f"Timeout waiting for {needle!r}; serial:\n{self.buf[-3000:]}"
            )
            self.read()
        assert "FATAL" not in self.buf, self.buf

    def command(self, cmd, expected=None, timeout=15):
        if not self.shell_ready:
            self.open_app("terminal")
            self.wait("luna> ", 20)
            self.shell_ready = True
        self.buf = ""
        # Echo handshake avoids UART receive overruns during software rendering.
        for c in cmd:
            mark = len(self.buf)
            self.serial.send(c.encode())
            deadline = time.monotonic() + 12
            while len(self.buf) == mark:
                assert time.monotonic() < deadline, f"No echo for {c!r}"
                self.read()
        self.serial.send(b"\n")
        self.buf = ""
        self.wait("luna> ", timeout)
        result = self.buf
        if expected is not None:
            assert expected in result, f"{cmd!r}: expected {expected!r}, got {result!r}"
        print("PASS:", cmd, flush=True)
        return result

    def key(self, key):
        self.qcommand("send-key", {"keys": [{"type": "qcode", "data": key}]})
        time.sleep(0.06)

    def open_app(self, name):
        launcher = symbol_address("launcher_open")
        if memory(self, launcher, 1) == b"\0":
            self.key("meta_l")
        wait_until(lambda: memory(self, launcher, 1) != b"\0")
        for c in name:
            self.key("spc" if c == " " else c)
        self.key("ret")
        wait_until(lambda: memory(self, launcher, 1) == b"\0")
        expected = {"home": 0, "terminal": 1, "files": 2, "browser": 3, "settings": 4, "tasks": 5, "notes": 6}
        if name in expected:
            address = symbol_address("active_app")
            wait_until(
                lambda: (
                    int.from_bytes(memory(self, address, 4), "little", signed=True)
                    == expected[name]
                )
            )

    def screenshot(self, name):
        wait_until(lambda: memory(self, symbol_address("ui_dirty"), 1) == b"\0", 30)
        self.qcommand("screendump", {"filename": str(TEST / f"{name}.ppm")})
        from PIL import Image

        im = Image.open(TEST / f"{name}.ppm")
        assert im.size == (1024, 768)
        assert len(im.getcolors(1024 * 768)) > 100
        im.save(ROOT / "dist" / f"{name}.png")

    def close(self):
        if self.p.poll() is None:
            self.qcommand("quit")
            self.p.wait(timeout=5)
        with (TEST / "transcript.log").open("a") as f:
            f.write(self.transcript + "\n--- GUEST END ---\n")
        self.serial.close()
        self.qf.close()
        self.qmp.close()
        self.log.close()


def symbol_address(name):
    for row in subprocess.check_output(["nm", "-n", "build/kernel.elf"], text=True).splitlines():
        fields = row.split()
        if len(fields) == 3 and fields[2] == name:
            return int(fields[0], 16)
    raise AssertionError("symbol not found: " + name)


def memory(guest, address, size):
    dest = TEST / "memory.bin"
    guest.qcommand("pmemsave", {"val": address, "size": size, "filename": str(dest)})
    return dest.read_bytes()


def wait_until(predicate, timeout=20):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, "guest condition timed out"
        time.sleep(0.1)
