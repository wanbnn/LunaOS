Firmware binaries are not shipped yet. Approved .bin, .ucode and .fw files placed
here are packed byte-for-byte into the disk firmware region by tools/firmwarepack.py.
Preserve the original redistribution license alongside each added firmware.
Relative names are preserved, e.g. rtlwifi/rtl8723befw_36.bin.
Limits: 16 entries, 512 KiB per file, 2 MiB total archive. This is independent of
LunaFS's text-file size limit. FNV detects corruption; it is not authentication.
