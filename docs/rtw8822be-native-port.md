# RTL8822BE native station TX/RX and WPA2 — hardware-experimental

The release includes native RTL8822BE (`10ec:b822`) firmware/PHY initialization,
passive scanning and an **experimental station TX/MLME/WPA2 path**. This is
**not validated on physical hardware**. Actual association, firmware TX descriptor
interpretation, RF regulation and encrypted IP traffic must be confirmed on the
target laptop before this can be marked supported. Setup enumerates observed APs;
no simulated SSIDs are shipped in the production image.

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

## Experimental 802.11 station path

The native PCIe driver supplies a real station MAC from card efuse and now has
separate polled management and best-effort PCIe TX descriptor queues. It
constructs auth and association requests for an observed BSSID/channel, processes
responses with bounded timeouts and preserves the exact RSN IE advertised by the
scanned access point. Association is followed by the existing `drivers/wifi_station.c`
controlled port: EAPOL 4-way handshake using `net/wpa2.c` (PBKDF2,
HMAC-SHA1, replay/MIC validation), software AES-CCMP, and DHCP/ARP/IP after
successful key installation. `can_connect=true` exposes the option in setup.
`WIFI_ONLINE` is reported only after a DHCP lease is configured.

**This code path is experimental**, not a verified working Wi-Fi connection.
The hardware MAC, PCIe queue IDs/pointers, TX descriptor flags and 802.11
management compatibility need on-device verification. The card may transmit
nothing or fail to associate. It is not a Linux `rtw88` implementation;
in particular we do not yet support WPA3/SAE, 802.1X, TKIP, 802.11w required
PMF, AMSDU/AMPDU, fragmentation, 802.11r roaming or advanced power management.
Data rates are deliberately legacy. Card transmit power, RF regulatory country
policy and per-device calibration are **not yet independently validated for TX**.
Do not ship this build as production WLAN support.

The Wi-Fi diagnostics report initialization, RX count, TX count, queue-full
events, and errors. Scanning an SSID is not connection proof; an online IPv4
state plus actual bidirectional traffic on the physical RTL8822BE is required.

## Authentication timeout investigation

Physical RTL8822BE testing reported working scans (27 APs), but authentication
failed with `-110`, `auth_rx=0` and `TX MGMT index=0x00000006`: six submitted
TX descriptors and no hardware read-pointer progress. This isolates the observed
failure to the pre-WPA2 path; software enqueue counts do not prove RF transmission.

The native initialization now follows the pinned rtw88 `txdma_queue_mapping()` /
`__priority_queue_cfg()` order: program queue mapping, reset/enable CR (`0 -> ff`),
then load FIFO page allocation and initialize LLT, and clear CR+3 for normal TX.
Previously CR was enabled only after FIFO/LLT setup, even though firmware download
restores the disabled pre-download CR. This ordering correction still requires
physical confirmation that the MGMT read pointer advances and auth responses arrive.
`wifidiag` now reads MGQ's actual EXTRA queue credits (`0x23e`) and public credits
(`0x242`); the former HQ counter (`0x232`) was for a different priority queue.

`make test-rtw8822be-fifo` checks the initialization order and stops on MMIO errors.
It is a register-sequence model, not a physical reproduction of the timeout.

A subsequent physical test of the FIFO-order correction reported
`TX MGMT index=0x00030003`, `MGQ/EXQ avail=61`, public credits 1739,
`txpkt_empty=0x0dff`, and no authentication response. PCIe consumed all three
requests, but three pages remained occupied in the internal management queue.
This moves the investigation beyond the host DMA ring; it does not prove RF TX.

The driver now configures WLAN-only coexistence, adapted from the pinned
`coex.c` ANT_WONLY path and RTL8822B RFE/antenna setup: WLAN owns the path,
WLAN RF/BB grants are software high, BT grants are software low, LTE arbitration
is disabled, and the external switch is selected for the active WLAN band after
every channel change. This port has no Bluetooth driver or shared-radio PTA
policy; simultaneous Bluetooth operation is not supported by this mode.
The new `RF grants before/init/now/state` and antenna fields in `wifidiag` expose
readback instead of assuming this fixes the physical blockage. The grant control
bits masked by `0xff80` must read `0x7700`. `make test-rtw8822be-coex` covers
indirect register masks, band selection, bounded readiness, MMIO faults and
readback failure. Actual authentication/RF operation still needs physical testing.

## Hardware acceptance sequence

1. Use a WPA2-Personal **AES/CCMP-only** access point with PMF not required
   and 2.4 GHz channel 1–11 (initially test channel 1 or 6). Do not begin with
   WPA2/WPA3 mixed mode. Test on an isolated AP and only where permitted by
   local radio regulations.
2. Boot the new ISO on the RTL8822BE device, open setup Wi-Fi, scan and select
   a real BSSID/SSID, then enter its WPA2 passphrase (8–63 characters).
3. Record `wifidiag` and serial logs at each state; confirm non-zero TX
   counts and that auth/association advances beyond `WIFI_ASSOCIATING`.
4. Require WPA2 4-way completion without MIC/replay errors, DHCP assignment,
   and actual bidirectional DNS/IPv4/ICMP/HTTP. `WIFI_ONLINE` alone is not
   enough to claim verified internet reachability.
5. Retry disconnect/reconnect, wrong password, lost AP, DHCP expiration,
   RF failure, ring-full and power cycle. Failure must close the port,
   clear the key state and invalidate IP.

Host tests under `make test-wifi-client test-rtw8822be-tx-dma test-wifi-ccmp
test-wifi-station` are simulations, not RF certification.

## Source and license

Selected Linux rtw88 sources are reused from pinned commit
`d48723711bae9b8cdc56719c448ba42fef28cbee`, already archived under
`assets/driver-sources/linux-wireless.tar.gz`:

- `rtw8822b.c`: MAC power transitions, RFE/CCA, channel and TRX path routines.
- `rtw8822b_table.c`: MAC, BB, AGC and RF A/B register tables (unmodified arrays).
- `rtw8822b.h` / `reg.h`: PHY register definitions.
- `pci.c`, `fw.c`, `tx.c`, `mac.c`, `efuse.c`, `phy.c`, `coex.c`, `util.c`: adapted DMA descriptors,
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

Build: `make -j2 all`. Focused DMA models: `make test-rtw8822be-rx-dma test-rtw8822be-tx-dma`.
