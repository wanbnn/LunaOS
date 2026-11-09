#!/usr/bin/env python3
import hashlib
import json
import subprocess
import struct
from pathlib import Path
from firmwarepack import build_archive, SECTOR
from efifat import make_esp, CORE_BYTES, ESP_BYTES, IMAGE_BYTES, ESP_SECTOR

# Layout shared with the kernel: the payload comes first, the image header
# follows it and the file system starts right after that.
LUNA_KERNEL_SECTORS = 16383
LUNA_IMAGE_INFO_SECTOR = 16383
VOLUME_START = 16384

build = Path("build")
out = Path("dist")
out.mkdir(exist_ok=True)
kernel = (build / "kernel.bin").read_bytes()
subprocess.run(
    [
        "gcc",
        "-m32",
        "-c",
        "-x",
        "assembler-with-cpp",
        f"-DKERNEL_SECTORS={(len(kernel) + 511) // 512}",
        "boot/stage2.S",
        "-o",
        "build/stage2.o",
    ],
    check=True,
)
subprocess.run(
    [
        "ld",
        "-m",
        "elf_i386",
        "-Ttext",
        "0x8000",
        "--oformat",
        "binary",
        "-e",
        "_start",
        "build/stage2.o",
        "-o",
        "build/stage2.bin",
    ],
    check=True,
)
stage2 = (build / "stage2.bin").read_bytes()
kernel = (build / "kernel.bin").read_bytes()
assert len(stage2) <= 32768, "stage 2 exceeds reserved space"
payload = stage2.ljust(32768, b"\0") + kernel
payload = payload.ljust((len(payload) + 511) // 512 * 512, b"\0")
assert len(payload) < LUNA_KERNEL_SECTORS * 512, "Kernel overlaps the image header"
sectors = len(payload) // 512
subprocess.run(
    [
        "gcc",
        "-m32",
        "-c",
        "-x",
        "assembler-with-cpp",
        f"-DPAYLOAD_SECTORS={sectors}",
        "boot/boot.S",
        "-o",
        "build/boot.o",
    ],
    check=True,
)
subprocess.run(
    [
        "ld",
        "-m",
        "elf_i386",
        "-Ttext",
        "0x7c00",
        "--oformat",
        "binary",
        "-e",
        "_start",
        "build/boot.o",
        "-o",
        "build/boot.bin",
    ],
    check=True,
)
boot = (build / "boot.bin").read_bytes()
assert len(boot) == 512 and boot[-2:] == b"\x55\xaa"
image = out / "LunaOS.img"
firmware_archive = build_archive(Path("assets/firmware"))
source_path = Path("assets/driver-sources/linux-wireless.tar.gz")
source_manifest = json.loads(Path("assets/driver-sources/manifest.json").read_text())
source_archive = source_path.read_bytes()
assert hashlib.sha256(source_archive).hexdigest() == source_manifest["sha256"]
image_bytes = IMAGE_BYTES
assert 32769 * 512 + len(source_archive) <= image_bytes
source_header = struct.pack("<8sI32s40s", b"LUNASRC1", len(source_archive),
    hashlib.sha256(source_archive).digest(), source_manifest["commit"].encode()).ljust(512, b"\0")
image_info = struct.pack("<8sIIIII", b"LUNAIMG1", 1, image_bytes, len(source_archive), 32768, len(kernel)).ljust(512,b"\0")
with image.open("wb") as f:
    f.write(boot)
    f.write(payload)
    f.truncate(CORE_BYTES)
    f.seek(LUNA_IMAGE_INFO_SECTOR * 512)
    f.write(image_info)
    f.seek(32768 * 512)
    f.write(source_header)
    f.write(source_archive)
    f.seek(SECTOR * 512)
    f.write(firmware_archive)

# MBR holds the native data partition plus a standard FAT32 EFI system partition.
with image.open("r+b") as f:
    f.seek(446)
    f.write(struct.pack("<BBBBBBBBII",0x80,0,1,4,0xda,63,32,31,VOLUME_START,ESP_SECTOR-VOLUME_START))
    f.write(struct.pack("<BBBBBBBBII",0,0,1,32,0xef,63,32,65,ESP_SECTOR,ESP_BYTES//512))
core = image.read_bytes()
efi_loader = Path("build/BOOTX64.EFI").read_bytes()
esp = make_esp(core, efi_loader)
with image.open("ab") as f:
    f.write(esp)
Path("build/esp.img").write_bytes(esp)
assert image.stat().st_size == image_bytes

manifest = {
    "name": "LunaOS",
    "version": "0.3.0-dev",
    "format": "raw BIOS + UEFI x64 disk",
    "bytes": image.stat().st_size,
    "kernel_bytes": len(kernel),
    "payload_sectors": sectors,
    "sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
    "boot": "native BIOS / UEFI x64 bridge to i386 kernel",
    "font": "Inter Tight, Noto Sans, Montserrat + Noto Sans Symbols 2 / SIL OFL 1.1",
    "minimum_ram_mib": 64,
    "uefi_minimum_ram_mib": 256,
    "uefi_x64": True,
    "secure_boot_signed": False,
    "esp_sector": ESP_SECTOR,
    "esp_bytes": ESP_BYTES,
    "efi_sha256": hashlib.sha256(efi_loader).hexdigest(),
    "compositor_target_fps": 60,
    "modern_dom_css_engine": False,
    "driver_services_abi": 1,
    "firmware_sector": SECTOR,
    "firmware_archive_bytes": len(firmware_archive),
    "setup_first_boot": True,
    "wireless_sources": source_manifest,
}
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
(out / "SHA256SUMS").write_text(manifest["sha256"] + "  LunaOS.img\n")
print(f"{image}: {image.stat().st_size / 1024 / 1024:.1f} MiB; kernel {len(kernel):,} bytes")
