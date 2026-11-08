#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
arch=i386
if [[ "${LUNA_UEFI:-0}" == 1 ]]; then arch=x86_64; fi
if command -v "qemu-system-$arch" >/dev/null; then
  qemu="qemu-system-$arch"
  firmware=()
else
  qemu="$PWD/.tools/usr/bin/qemu-system-$arch"
  export LD_LIBRARY_PATH="$PWD/.tools/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  export QEMU_MODULE_DIR="$PWD/.tools/usr/lib/x86_64-linux-gnu/qemu"
  firmware=(-L "$PWD/.tools/usr/share/qemu" -bios "$PWD/.tools/usr/share/seabios/bios-256k.bin")
fi
ram=4096
if [[ "${LUNA_UEFI:-0}" == 1 ]]; then
  ovmf="${LUNA_OVMF_DIR:-$PWD/.tools/uefi/usr/share/OVMF}"
  if [[ ! -f "$ovmf/OVMF_CODE_4M.fd" ]]; then ovmf=/usr/share/OVMF; fi
  if [[ ! -f "$ovmf/OVMF_CODE_4M.fd" || ! -f "$ovmf/OVMF_VARS_4M.fd" ]]; then
    echo "Instale ovmf ou configure LUNA_OVMF_DIR (OVMF_CODE_4M.fd e OVMF_VARS_4M.fd)." >&2; exit 1
  fi
  vars=$(mktemp "$PWD/build/uefi-vars-XXXXXX.fd")
  cp "$ovmf/OVMF_VARS_4M.fd" "$vars"
  firmware=(-L "$PWD/.tools/usr/share/qemu" -drive "if=pflash,format=raw,readonly=on,file=$ovmf/OVMF_CODE_4M.fd" -drive "if=pflash,format=raw,file=$vars")
fi
image_path="${LUNA_IMAGE:-dist/LunaOS.img}"
if [[ ! -f "$image_path" ]]; then echo "LunaOS image not found: $image_path" >&2; exit 1; fi
case "$image_path" in
  *.iso) boot_media=(-cdrom "$image_path" -boot order=d) ;;
  *) boot_media=(-drive "file=$image_path,format=raw,if=ide") ;;
esac
display_backend="${LUNA_DISPLAY:-gtk}"
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then display_backend=none; fi
accelerator=(-accel tcg -cpu max)
if [[ -r /dev/kvm && -w /dev/kvm ]]; then accelerator=(-accel kvm -cpu host); fi
exec "$qemu" "${accelerator[@]}" "${firmware[@]}" -display "$display_backend" -machine pc -m "${LUNA_RAM:-$ram}M" -vga std "${boot_media[@]}" -netdev user,id=n0 -device rtl8139,netdev=n0,romfile= -serial "${LUNA_SERIAL:-stdio}" -audiodev "${LUNA_AUDIO:-driver=none,id=audio0}" -device AC97,audiodev=audio0 -machine pcspk-audiodev=audio0 "$@"
