# Linux wireless port — phase 1 (LunaOS)

**Status: PARTIAL. NO WIFI RADIO IS OPERATIONAL.** This phase adds independently
implemented, freestanding IEEE 802.11 frame primitives. It does **not** compile
Linux `mac80211`, `cfg80211`, drivers or IPv6, and does not claim ABI compatibility.

## Existing foundations

LunaOS already has native PCI/MMIO/DMA/INTx, deferred work, packet buffers, and a
firmware loader (`kernel/driver.h`, `docs/driver-services.md`). The upstream Linux
wireless source snapshot is already shipped as an **uncompiled source archive** in
`assets/driver-sources/` (see pinned `manifest.json`); it is not a driver port.
The `drivers/wireless.c` enumerator still only identifies supported target PCI IDs.

## Implemented in this branch

- `include/net/luna80211.h` + `net/wireless/frames.c`: bounded 802.11 management
  and data header parsing, BSSID/source/destination selection, variable QoS/WDS
  header lengths, beacon/probe-response SSID and DS channel parsing, RFC1042
  LLC/SNAP decapsulation to Ethernet, and **unprotected** station-to-AP frame
  assembly (for future use by an authorized association/encryption layer).
- Strict error returns for protected MPDUs, unsupported control frames, invalid
  information elements, undersized buffers and malformed frames.
- A freestanding 32-bit compile check and host-side functional frame tests.
- The LunaOS Makefile compiles the component into the kernel. There is no
  automatic device probing/activation or Wi-Fi status change.

Run `make test-wireless-frames` on the host. This does **not** test a PCI radio.
Full image build and physical hardware validation remain separate milestones.

## Integration sequence (required; not delivered)

1. Define stable 802.11 device registration/teardown and link-state callbacks
   with strict ownership across the existing driver services. Avoid importing
   Linux `sk_buff`/`net_device` APIs as dummy success stubs.
2. Port core `cfg80211` policy: channel constraints/country/regulatory,
   supported rates/bands, scan requests/results and network configuration.
3. Port minimum `mac80211` SoftMAC: STA state machine, management TX/RX,
   authentication, association, power management, queues, aggregation and replay
   protection. Add BSS cache, timeout handling and key lifecycle.
4. Integrate WPA2/CCMP with verified EAPOL/4-way handshake and secure key
   storage/erasure; add WPA3/SAE only with a tested crypto/auth implementation.
5. Complete per-device firmware, PCIe resource setup, interrupts, TX/RX DMA,
   efuse, calibration and PHY; choose **RTL8723BE** first (rtlwifi +
   rtl8723com + btcoexist), then RTL8188EE. `rtw88` is a separate Realtek
   family for RTL8723DE/RTL8821CE; `mt76` for MT7612; `ath10k` for QCA9377.
6. Refactor existing `drivers/net.c` which currently mixes RTL8139 and a static
   IPv4/TCP profile. Introduce a multi-interface link/ARP/IP service, DHCP/DNS
   per interface, routing and IPv6 independently. Do not assume QEMU's
   10.0.2.15 settings are valid for a physical Wi-Fi network.
7. Verify real hardware: identify PCI revision/subsystem; firmware versions,
   scan, open/WPA2 AP association, DHCP, DNS, bidirectional TCP, reconnect,
   IRQ faults, removal, RF kill, lawful channels/country and security negatives.

## Licensing and provenance

The Linux snapshot in `assets/driver-sources` retains its original upstream
licenses. If Linux kernel code is copied/adapted into the compiled LunaOS kernel,
review the GPL-2.0 licensing obligations of the combined distribution and
preserve per-file SPDX and notices. This new minimal frame layer is original
LunaOS code; it is not a verbatim copy of Linux source.

Do not mark any target in `docs/reference/wifi-targets.json` as operational until
verified on real hardware. Bluetooth needs a distinct USB/HCI stack.
