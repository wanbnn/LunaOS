RTL8822BE firmware is shipped as rtw88/rtw8822b_fw.bin, unmodified from
linux-firmware. See provenance.json for the pinned source and SHA-256, and
../licenses/Realtek-Firmware.txt for redistribution terms. The license is also
installed at /etc/licenses/Realtek-Firmware.txt in LunaOS.

A packaged firmware is NOT a working radio driver: upload, initialization, scan,
association and WPA2 are not implemented.

.bin, .ucode and .fw files are packed byte-for-byte by tools/firmwarepack.py.
Limits: 16 entries, 512 KiB per file, 2 MiB total.
