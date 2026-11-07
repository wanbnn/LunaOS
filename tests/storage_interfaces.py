#!/usr/bin/env python3
"""Read-only smoke: real EHCI/BOT, SDHCI PIO and MFI DMA in QEMU."""
import hashlib
import os
import re
import shutil
import subprocess

from guest import ROOT, TEST, Guest, env, qemu


def device_args(kind, path, readonly=False):
    args = ["-drive", f"file={path},format=raw,if=none,id=target" + (",readonly=on" if readonly else "")]
    if kind == "usb":
        args += ["-device", "usb-ehci,id=hc", "-device", "usb-storage,drive=target,bus=hc.0"]
    elif kind in ("sdhci", "emmc"):
        args += ["-device", "sdhci-pci,id=hc", "-device", f"{'emmc' if kind == 'emmc' else 'sd-card'},drive=target"]
    elif kind in ("raid", "raid2"):
        args += ["-device", f"{'megasas-gen2' if kind == 'raid2' else 'megasas'},id=hc",
                 "-device", "scsi-hd,drive=target,bus=hc.0,scsi-id=0,lun=0"]
    else:
        raise ValueError(kind)
    return args


def supports_kind(kind):
    if kind != "emmc":
        return True
    available = subprocess.check_output([qemu, "-device", "help"], env=env, text=True)
    if 'name "emmc"' in available:
        return True
    if "LUNA_STORAGE_KINDS" in os.environ:
        raise RuntimeError("Requested eMMC test requires QEMU with the emmc device")
    print("SKIP: eMMC model unavailable in this QEMU (validated with QEMU 10.2)", flush=True)
    return False


def image_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def readonly_case(kind):
    source = TEST / f"interfaces-{kind}-readonly-boot.img"
    target = TEST / f"interfaces-{kind}-readonly-target.img"
    shutil.copyfile(ROOT / "dist/LunaOS.img", source)
    with target.open("wb") as stream:
        stream.truncate(128 * 1024 * 1024)
    before = image_digest(target)
    g = Guest(source, extra_args=device_args(kind, target, readonly=True), ram=256)
    try:
        status = g.command("storage", "Storage:")
        assert re.search(r"disk5 (USB|RAID) .*\[read-only/unsupported\]", status), status
        assert "[installable]" not in status, status
    finally:
        g.close()
    assert image_digest(target) == before, f"{kind} read-only image changed"
    print(f"PASS: {kind} write-protected media excluded from installation", flush=True)


def main():
    for kind, label in [("usb", "USB"), ("sdhci", "eMMC/SDHCI"), ("emmc", "eMMC/SDHCI"), ("raid", "RAID"), ("raid2", "RAID")]:
        if kind not in os.environ.get("LUNA_STORAGE_KINDS", "usb sdhci emmc raid raid2").split():
            continue
        if not supports_kind(kind):
            continue
        source = TEST / f"interfaces-{kind}-boot.img"
        target = TEST / f"interfaces-{kind}-target.img"
        shutil.copyfile(ROOT / "dist/LunaOS.img", source)
        with target.open("wb") as stream:
            stream.truncate((4096 if kind in ("sdhci", "emmc") else 128) * 1024 * 1024)
            stream.write(b"READ ONLY NATIVE STORAGE TEST " + kind.encode())
        before = image_digest(target)
        g = Guest(source, extra_args=device_args(kind, target), ram=256)
        try:
            status = g.command("storage", "Storage:")
            match = re.search(r"disk(\d+) " + re.escape(label) + r" .*\[installable\]", status)
            assert match, status + "\n" + g.transcript[-4000:]
            g.command("storage verify " + match.group(1), "two consistent physical reads", timeout=40)
        finally:
            g.close()
        assert image_digest(target) == before, f"{kind} target modified"
        print(f"PASS: {kind} physical sector reads; target unchanged", flush=True)
        if kind in ("usb", "raid", "raid2"):
            readonly_case(kind)


if __name__ == "__main__":
    main()
