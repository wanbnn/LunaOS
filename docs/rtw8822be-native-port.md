# RTL8822BE native passive scan — experimental

The release now contains a native RTL8822BE (`10ec:b822`) initialization path.
This is an **untested-on-hardware experimental passive scanner**, not a working
association/WPA2 driver. Setup attempts scanning automatically on its network
page. No simulated SSIDs are compiled into the image.

## Runtime

`kernel/main.c` calls `rtw8822be_init()` after native driver services start.
`drivers/rtw8822be_pcie.c` claims BAR2, masks device interrupts, allocates native
32-bit coherent DMA rings, powers the MAC, sends the packaged firmware through
the PCIe beacon/reserved-page queue, executes device DDMA copies and waits for
MCU readiness. It reads efuse, applies PHY tables, configures 20-MHz RX, enables
bus mastering and registers `drivers/wifi_scan.c` with `WifiDriverOps`.

The scanner switches channels, consumes actual RX DMA descriptors, validates
RX tags/lengths/CRC status, strips FCS, extracts RSSI and ingests beacon/probe
response SSIDs. RX is polled, so this initial path does not depend on PCI IRQ
routing. Channels 1–11 and 36/40/44/48 are scanned passively; no probe requests
are transmitted. RFE types 2, 3 and 5 have upstream tables and are accepted;
other efuse front-end types fail explicitly rather than guessing configuration.

Setup's hardware page and `wifidiag` report the initialization stage/error and
RX/drop counters. No presence-only preflight resizes BARs of the active native
driver. Cleanup disables and reads back PCI bus mastering before releasing DMA
memory; when that cannot be confirmed, mappings/allocations are retained.

The current backend has `can_connect=false`. Setup displays that it is a
passive scan and disables Connect. Authentication, association, WPA2/CCMP,
DHCP and general Wi-Fi IP traffic are **not implemented**. Receiving an SSID
must not be interpreted as a network connection.

## Source and license

Selected Linux rtw88 sources are reused from pinned commit
`d48723711bae9b8cdc56719c448ba42fef28cbee`, already archived under
`assets/driver-sources/linux-wireless.tar.gz`:

- `rtw8822b.c`: MAC power transitions, RFE/CCA, channel and TRX path routines.
- `rtw8822b_table.c`: MAC, BB, AGC and RF A/B register tables (unmodified arrays).
- `rtw8822b.h` / `reg.h`: PHY register definitions.
- `pci.c`, `fw.c`, `tx.c`, `mac.c`, `efuse.c`, `phy.c`: adapted DMA descriptors,
  reserved-page transfer, firmware DDMA flow, efuse parsing and PHY table logic.

The imported files permit `GPL-2.0 OR BSD-3-Clause`; this adaptation selects
BSD-3-Clause and preserves Realtek copyright. The full notice is in
`assets/licenses/Realtek-rtw88-BSD.txt` and guest `/etc/licenses/`.
Firmware has a separate license and pinned SHA-256 in
`assets/firmware/provenance.json`. LunaOS does not run a Linux kernel.

## Validation limits

A host simulation checks real scanner logic with generated RTL8822B RX packets;
a second focused simulation checks the native RX ring's tag progression,
wraparound, rearming and CRC rejection. The firmware/DDMA component has an
independent memory model. These models are **not** evidence that efuse, RF
calibration, PCIe timing or firmware execution works on physical RTL8822BE.
The new end-to-end hardware initialization path has not yet been tested on a
physical card; the user's separate test machine provides that validation.

Build: `make -j2 all`. Focused RX model: `make test-rtw8822be-rx-dma`.
