#!/usr/bin/env python3
"""Build the pinned test-only QEMU fixture locally; never install host packages."""
from pathlib import Path
import glob
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / ".tools"
VERSION = "10.2.0"
SHA256 = "9e30ad1b8b9f7b4463001582d1ab297f39cfccea5d08540c0ca6d6672785883a"
source = TOOLS / "qemu-storage-src"
build = TOOLS / "qemu-storage-build"
archive = TOOLS / f"qemu-{VERSION}.tar.xz"
patch = ROOT / "tests/fixtures/storage/qemu-10.2.0-acpi-emmc.patch"
env = os.environ.copy()
env["PYTHONPATH"] = str(TOOLS / "python")
env["LD_LIBRARY_PATH"] = str(TOOLS / "usr/lib/x86_64-linux-gnu")
build.mkdir(parents=True, exist_ok=True)


def run(args, **kwargs):
    subprocess.run(args, env=env, check=True, **kwargs)


if not (TOOLS / "usr/bin/iasl").exists() and not shutil.which("iasl"):
    dependencies = ["acpica-tools"]
else:
    dependencies = []
local_pc = TOOLS / "usr/lib/x86_64-linux-gnu/pkgconfig/glib-2.0.pc"
if subprocess.run(["pkgconf", "--exists", "glib-2.0"], env=env).returncode:
    if not local_pc.exists():
        dependencies += ["libglib2.0-dev", "libpcre2-dev", "libsysprof-capture-4-dev"]
    env["PKG_CONFIG_SYSROOT_DIR"] = str(TOOLS)
    env["PKG_CONFIG_LIBDIR"] = str(local_pc.parent)
# The installation matrix captures PPM screenshots using QMP screendump.
local_pixman = None
if subprocess.run(["pkgconf", "--exists", "pixman-1"], env=env).returncode:
    local_pixman = local_pc.with_name("pixman-1.pc")
    if not local_pixman.exists():
        dependencies += ["libpixman-1-dev", "libpixman-1-0"]
if dependencies:
    debs = TOOLS / "debs"
    debs.mkdir(parents=True, exist_ok=True)
    run(["apt-get", "download", *dependencies], cwd=debs)
    for package in dependencies:
        for deb in glob.glob(str(debs / (package + "_*.deb"))):
            run(["dpkg-deb", "-x", deb, str(TOOLS)])
    # Ubuntu 26 split the headers out of the development metapackage.
    if "libglib2.0-dev" in dependencies and not local_pc.exists():
        run(["apt-get", "download", "libgio-2.0-dev", "libglib2.0-0t64"], cwd=debs)
        for pattern in ["libgio-2.0-dev_*.deb", "libglib2.0-0t64_*.deb"]:
            for deb in debs.glob(pattern):
                run(["dpkg-deb", "-x", str(deb), str(TOOLS)])
if local_pixman and not env.get("PKG_CONFIG_SYSROOT_DIR"):
    # Use local Pixman alongside system GLib without relocating GLib's paths.
    pkgconfig = build / "local-pkgconfig"
    pkgconfig.mkdir(exist_ok=True)
    relocated = local_pixman.read_text().replace("prefix=/usr\n", f"prefix={TOOLS / 'usr'}\n", 1)
    (pkgconfig / local_pixman.name).write_text(relocated)
    env["PKG_CONFIG_PATH"] = os.pathsep.join(filter(None, (str(pkgconfig), env.get("PKG_CONFIG_PATH"))))
if not archive.exists():
    urllib.request.urlretrieve(f"https://download.qemu.org/qemu-{VERSION}.tar.xz", archive)
with archive.open("rb") as stream:
    digest = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(chunk)
assert digest.hexdigest() == SHA256, "QEMU source checksum mismatch"
if not source.exists():
    with tarfile.open(archive) as package:
        package.extractall(TOOLS)
    (TOOLS / f"qemu-{VERSION}").rename(source)
# Restore only the patched sources so rerunning after a fixture change is safe.
with tarfile.open(archive) as package:
    for name in ["hw/i386/pc_piix.c", "hw/sd/sdhci.c", "hw/sd/sd.c"]:
        (source / name).write_bytes(package.extractfile(f"qemu-{VERSION}/" + name).read())
run(["patch", "-p1", "--batch", "-i", str(patch)], cwd=source)
python = TOOLS / "python"
python.mkdir(exist_ok=True)
for wheel in (source / "python/wheels").glob("meson-*.whl"):
    with zipfile.ZipFile(wheel) as package:
        package.extractall(python)
if not (build / "build.ninja").exists():
    run([str(source / "configure"), "--target-list=i386-softmmu,x86_64-softmmu",
         "--without-default-features", "--enable-system", "--enable-tcg", "--enable-kvm",
         "--disable-tools", "--disable-docs", "--disable-werror", "--enable-pixman"], cwd=build)
elif "#define CONFIG_PIXMAN 1" not in (build / "config-host.h").read_text():
    run([str(build / "pyvenv/bin/meson"), "configure", "-Dpixman=enabled"], cwd=build)
run(["ninja", "-j4", "qemu-system-i386", "qemu-system-x86_64"], cwd=build)
metadata = {"qemu": VERSION, "source_sha256": SHA256,
            "patch_sha256": hashlib.sha256(patch.read_bytes()).hexdigest(),
            "purpose": "test-only non-PCI SDHCI MMIO device with ACPI SSDT"}
(build / "fixture.json").write_text(json.dumps(metadata, indent=2) + "\n")
print("PASS: pinned QEMU ACPI eMMC fixture built locally", flush=True)
