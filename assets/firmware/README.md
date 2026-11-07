Firmware is distributed unmodified from linux-firmware at the commit recorded
in provenance.json, with byte counts and SHA-256 for each file and license.

- rtw88/rtw8822b_fw.bin: Realtek license in ../licenses/Realtek-Firmware.txt.
- iwlwifi-so-a0-gf-a0-89.ucode: SO A0/GF A0 Intel candidate; license in
  ../licenses/Intel-WiFi-Firmware.txt. This image is not selected from PCI ID
  alone. Native transport preparation requires matching CSR MAC/RF identifiers.

Both licenses are installed under /etc/licenses in LunaOS.

A packaged or parsed firmware does not establish successful hardware boot.
Intel context/DMA setup, firmware ALIVE, NVM and radio operation remain pending.
RTL8822BE has an experimental passive scanner; association remains pending.

.bin, .ucode and .fw files are packed byte-for-byte by tools/firmwarepack.py.
Limits: 16 entries, at most 2 MiB per file, 2 MiB archive total INCLUDING
header/directory/padding. DMA now uses the shared E820-backed physical page allocator on demand, rather
than a dedicated 2 MiB pool. Image staging and radio queues must still handle
allocation failure and release DMA only after the hardware has stopped.
