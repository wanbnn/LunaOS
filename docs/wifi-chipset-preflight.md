# LunaOS — chipset Wi-Fi porting preflight

**STATUS: NOT OPERATIONAL.** This milestone does not implement any Wi-Fi
radio driver, firmware boot, AP scan, authentication, association, WPA or IP
traffic. Linux source files in `assets/driver-sources/` remain uncompiled
reference material.

## Delivered

- `drivers/wifi_port.h` and `.c`: C PCI-ID registry for four driver
  families and six priority chips (seven PCI IDs), PCI claim/BAR preflight,
  firmware loader diagnostics with strict resource release.
- `drivers/wireless.c`: report firmware and PCI readiness without falsely
  claiming a connection; preserve other existing device discovery.
- `tests/wifi_port.c`: mocked PCI/firmware success and failure tests.
- `make test-wifi-port`: host tests, **not hardware radio validation**.

## Focus devices

| Chip | PCI ID | Linux driver family | Firmware |
| --- | --- | --- | --- |
| RTL8723BE | 10ec:b723 | rtlwifi + rtl8723com | rtlwifi/rtl8723befw_36.bin |
| RTL8188EE | 10ec:8179 | rtlwifi | rtlwifi/rtl8188efw.bin |
| RTL8723DE | 10ec:d723 | rtw88 | rtw88/rtw8723d_fw.bin |
| RTL8821CE | 10ec:c821, 10ec:b821 | rtw88 | rtw88/rtw8821c_fw.bin |
| MT7612E PCIe | 14c3:7612 | mt76x2e | mt7662.bin and mt7662_rom_patch.bin |
| QCA9377 PCIe | 168c:0042 | ath10k | ath10k/QCA9377/hw1.0/firmware-6.bin and board-2.bin (revision-dependent candidates) |

USB MT7612U and combo Bluetooth functions are separate transport stacks.

## Required before marking any device as working

1. Adapt real driver logic from `rtlwifi`, `rtw88`, `mt76x2e`,
   `ath10k` to LunaOS PCI/MMIO/DMA/IRQ/worker/firmware services. Linux
   `sk_buff`, `ieee80211_hw`, `cfg80211` and power management APIs
   are not ABI compatible. Do not use dummy functions returning success.
2. Implement regulatory control, scan, auth/association, RX/TX, rate control,
   error paths, EAPOL, WPA2/CCMP and key/replay handling (WPA3 separately).
3. Refactor the present IPv4/Ethernet-only network interface for real
   Wi-Fi per-interface ARP, routing, DHCP and DNS (IPv6 separately).
4. Validate each physical chipset/revision for FW upload, actual network
   connectivity, interrupts, RF calibration, power and regulatory constraints.

Firmware is **not distributed**. The native firmware loader permits at most
16 entries, 512 KiB per blob, 2 MiB archive total; some images may exceed
those limits. A readable blob does not establish correct revision/API.

References: pinned Linux source in `assets/driver-sources/manifest.json`,
upstream linux-firmware WHENCE and MT76x2 driver firmware definitions.
