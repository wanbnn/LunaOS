# Intel AX211 and shared Wi-Fi development

## Current status

**AX211 scan, association and physical network connectivity are not operational.**
There is no registered Intel radio backend. Shared WPA2 and IPv4 integration
now execute against simulated radios in host tests; this is not hardware validation.

The development machine reports Intel AX211 160MHz, PCI `8086:51f0`, subsystem
`8086:4090`, revision `01` through Windows `Win32_NetworkAdapter`. WSL exposes
only virtual PCI devices, so this environment cannot read AX211 MMIO or test RF.

## Intel firmware and transport preparation

- `drivers/wifi_intel.c`: read-only inventory of Intel CNVi controller candidates.
  PCI IDs alone do not uniquely establish RF type or firmware compatibility.
- `drivers/iwl_pcie.c`: boot-time PCI claim, BAR0 validation, temporary memory
  decode, MMIO mapping, reads of `CSR_HW_REV`, `CSR_HW_RF_ID`, `CSR_GP_CNTRL`,
  conservative SO A/GF A firmware candidate selection and file validation.
  No bus mastering, device reset or RF transmission occurs. Claims/mappings
  and firmware are released. Failed cleanup retains ownership until reboot.
- `drivers/iwl_firmware.c`: bounded Intel TLV parser, sections/separators, CPU
  count, paging, version, flags and command version table. This is structural
  validation, not firmware API compatibility or successful device boot.
- `assets/firmware/iwlwifi-so-a0-gf-a0-89.ucode`: unmodified 1,737,012-byte
  linux-firmware image, with pinned commit, SHA-256 and redistribution license
  in `assets/firmware/provenance.json`. Actual image: 60 sections, 58 runtime
  payload sections, two CPUs, 794,624 bytes of paging.
- The binary archive now accepts files above 512 KiB, with a 2 MiB per-file
  bound and unchanged 2 MiB total archive bound. Current archive is 1,899,520
  bytes. Header/directory/padding count toward the total.
- Run `wifidiag intel` in LunaOS to see the actual PCI/CSR/firmware stage. Missing
  hardware, unknown RF/stepping, invalid files and cleanup failures are explicit
  errors. A validated file still ends in ENOTSUP at Gen3 boot setup.

The former fixed 2 MiB DMA pool has been replaced by demand allocation from
E820-backed physical memory, shared with the growable runtime heap. Staging,
paging and RX/TX allocations can now grow with actual available RAM and the
device address mask. Preparation still releases the firmware file; the Gen3
boot/RF implementation remains pending. See [physical memory](physical-memory.md).

## Shared WPA2 and controlled port

`net/wpa2.c` implements the initial RSN WPA2-PSK/CCMP four-way handshake using
the existing BearSSL cryptographic primitives:

- PBKDF2-HMAC-SHA1 (4096 rounds), PTK expansion, HMAC-SHA1-128 EAPOL MIC,
  AES-128 Key Wrap unwrapping and integrity verification (RFC 3394).
- Cryptographic SNonce callback with explicit failure; no timing-based fallback.
- BSSID/source, descriptor, flags, lengths, ANonce, replay and MIC checks.
- Exact AP RSN IE comparison in M3, CCMP/PSK selection and GTK KDE extraction.
- A repeated authenticated M3 only resends M4; it never reinstalls keys or resets
  packet numbers. Changed/replayed M3 is rejected. Handshake has a deadline.
- Key install/send failure and disconnect close the port and wipe derived keys.

`drivers/wifi_station.c` composes this with the network stack after the radio's
MLME has completed authentication and association. It blocks IP before WPA2,
requires authenticated/decrypted/replay-checked RX metadata on secured networks,
and starts DHCP only after authorization. `WIFI_ONLINE` requires a DHCP lease;
loss of radio or authorization detaches the interface and invalidates IP.

The hardware backend must implement actual CCMP encryption/decryption, per-key
packet numbers/replay checks and atomic key installation. This code does not
implement software CCMP, open-system 802.11 authentication/association, enterprise,
WPA3, PMF, group rekey or pairwise rekey. Rekey requires a fresh connection;
unsupported rekey closes the controlled port rather than retaining stale keys.

## IPv4 integration

`kernel/netif.h` and `net/netif.c` define a transport-neutral Ethernet MSDU
interface. `drivers/net.c` can bind it to the existing native ARP/IPv4/ICMP,
DNS/TCP/HTTP/TLS stack. The current stack selects one transport; an active
RTL8139 or an owning network task makes an additional attach return EBUSY.
It does not support simultaneous Ethernet and Wi-Fi routes.

`net/dhcp.c` implements discover/offer/request/ACK, transaction/MAC/server checks,
subnet/router/DNS parsing, lease renewal, rebinding, expiry, NAK and bounded
retries. The frame adapter validates Ethernet destination, IPv4 checksum/length/
fragmentation and UDP checksum when supplied. No address is considered configured
before an accepted ACK. Option overload and invalid/unsupported replies fail.

The IP stack retains a single gateway and peer ARP entry, with one bounded
pending packet for same-subnet resolution. It is not a general routing table.
The backend data port exchanges Ethernet MSDUs, so a raw 802.11 backend must
also supply framing/deframing and validated security metadata.

## Validation

- `make test-iwl-firmware test-iwl-pcie`: real firmware parsing and mocked PCI/
  MMIO acquisition/cleanup failure paths. No physical firmware upload.
- `make test-wpa2`: published password/IEEE PMK vector, independently generated
  PTK/MIC/handshake fixtures, RFC 3394 vector, repeated M3, wrong MIC/source,
  truncation, RNG/key install/TX failures and timeout.
- `make test-dhcp`: DORA, identity/bounds, renewal/rebinding, expiry, NAK,
  retries and millisecond clock wrap.
- `make test-wifi-station`: simulated radio through the real WPA2, controlled
  port, DHCP, native ARP/IPv4/ICMP and encrypted TX callback; blocks plaintext
  injection and invalidates IP on radio loss. This does not test RF or CCMP.
- `make test-wifi-runtime test-wifi-scan test-wifi-port test-wifi-intel`:
  shared scan/connection contract, BSSID, 2.4/5 GHz mapping and inventory.
- `LUNA_TEST_DIR=/tmp/luna-intel-test python3 tests/intel_firmware_guest.py`
  after `make all` (Unix sockets require a native filesystem on WSL): native guest loading
  of the 1.74 MB firmware twice, license installation and inactive-radio status.

## Required to make AX211 operational

1. Implement SO/GF Gen3 power/reset and context information, RX/TX/command DMA
   queues, firmware staging/paging, interrupt or polled notification handling,
   error recovery and a sufficient E820-backed memory budget.
2. Require and parse firmware ALIVE, configure PNVM/NVM/calibration/MAC,
   regulatory/RF-kill policy and firmware command/API versions.
3. Implement actual 2.4/5 GHz scan, channel selection, authentication/association,
   station contexts, multicast filtering, TX/RX, CCMP key/PN/replay operations
   and connect the callbacks to the shared station layer.
4. Validate boot, real scans, wrong passwords, timeouts, RF-kill, disconnect/
   reconnect, DHCP and actual packet traffic on physical AX211 hardware.

## Primary references

- [Intel AX211 specifications / CNVio2](https://www.intel.com.br/content/www/br/pt/products/sku/204837/intel-wifi-6e-ax211-gig/specifications.html)
- [Linux v6.12 iwlwifi formats and CSR definitions](https://github.com/torvalds/linux/tree/v6.12/drivers/net/wireless/intel/iwlwifi)
- [Upstream wpa_supplicant WPA derivation and EAPOL MIC documentation](https://w1.fi/wpa_supplicant/devel/wpa__common_8c.html)
- [RFC 3394 AES Key Wrap](https://www.rfc-editor.org/rfc/rfc3394)
- [RFC 2131 DHCP](https://www.rfc-editor.org/rfc/rfc2131)
