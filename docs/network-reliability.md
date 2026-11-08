# Wi-Fi and browser reliability investigation

## Plan and scope

1. Restore connected RTL8822BE receive and response configuration; verify the
   programmed MAC address, preserve scan behavior and test transport bounds.
2. Reproduce and repair native TCP recovery under overlap, reordering and ACK
   loss; retain bounded memory and reject incomplete HTTP/TLS responses.
3. Exercise browser resource URLs and full-size stylesheets in QEMU, including
   redirects, nondefault ports and CDN references.
4. Build/check the release artifacts, commit and push; validate the RF outcome
   on the physical notebook and both access points.

The software regressions below have automated coverage. The last physical RF
validation step remains pending: the development environment cannot reproduce
the notebook's radio link or the MitraStar GPT-2741GNAC-N2.

## Evidence and changes

The notebook received IPv4, ARP, DNS and web data, but the MitraStar eventually
sent a disconnect with reason 3. A captured diagnostic showed 1566 unicast
replays, all with Retry set, and received PN equal to the already accepted PN.
There were no MIC/key/format failures. This is consistent with the AP not
receiving a usable link-layer ACK; it does not prove which radio component is
responsible. With another router the user observed continuing ICMP connectivity
after an incomplete HTTP transfer. These are separate observations.

The RTL8822BE driver now clears the scanner's accept-all flag for connections,
enables BSSID filtering, and restores scanning filters and unassociated mode
when a new scan starts. It programs band-specific response rates (`0x15f` in
2.4 GHz, `0x150` in 5 GHz) and checks MAC-address readback before authentication.
These follow the reference driver's [filter configuration](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/realtek/rtw88/mac80211.c)
and [response-rate definitions](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/realtek/rtw88/main.h).
Their effect on physical ACK delivery is still unverified. The transmit bound
also accepts a full 1500-byte Ethernet payload after SNAP/CCMP encapsulation
(1548-byte MPDU), which was previously rejected by a 1540-byte limit.

The native TCP receiver previously discarded the new suffix of an overlapping
retransmission and every out-of-order segment. It now trims duplicate prefixes
and holds four bounded segments until the gap closes, including FIN ordering
and sequence wrap. SYN advertises MSS 1200, outgoing writes respect the peer's
MSS, and new connections get distinct source ports even within one timer tick.
Reset packets must match the active receive sequence. Recovery follows the
[TCP specification](https://www.rfc-editor.org/rfc/rfc9293.html); this remains a
small synchronous stack, not a claim of full TCP conformance.

A QEMU test reproduced a stylesheet failure caused by resource URL joining:
`/large.css` lost the base URL's port. Protocol-relative `//cdn/path` references
also gained extra slashes. Browser links/resources and HTTP redirects now use
the same bounded resolver. Resource paths use the final document URL after
redirects. Transport diagnostics distinguish resets and read/ACK timeouts,
and `net` retains the last failed HTTP URL/error, including subresources.

The HTTP response buffer was already 512 KiB. Its old 128 KiB error text was
incorrect; the buffer size was not the stylesheet failure's cause.

## Automated validation

- `make test-tcp`: overlapping retransmissions; gaps and four-slot queue
  overflow; duplicate suppression; FIN after a gap; sequence wrap; stale and
  valid resets; SYN MSS; peer MSS; lost ACK followed by a successful retry;
  full receive-buffer bounds.
- `make test-url`: root and relative URLs, ports, protocol-relative CDN URLs,
  query/fragment references, supported schemes and insufficient output space.
- RTL8822BE register/DMA tests: connected/scanning filters, MAC/BSSID and mode,
  response rates in both bands, descriptor sequence and full encrypted MTU.
- Wi-Fi station/client/CCMP regression tests; web-engine tests.
- `make check`: host regressions, firmware/archive checks, freestanding link,
  IMG/ISO integrity and image size limits.
- `tests/https.py`: real QEMU native TLS, CA/hostname rejection, chunk decoding,
  truncation rejection, protocol-relative redirect to a new document directory,
  a complete 350 KB linked stylesheet plus a CDN-style second stylesheet on
  a nondefault HTTPS port, then another HTTPS request and an ICMP reply.
- TCP and URL host tests also run with address/undefined-behavior sanitizers.
- `tests/smoke.py`: boot/desktop, persistent filesystem, native
  Ethernet/ARP/ICMP/TCP/HTTP and a 64 MiB offline boot.

The TLS fixture installs its temporary CA only in a disposable guest image.
Certificates and generated fixture data are not added to release trust roots.

## Physical acceptance test (pending)

Use the image built from the final commit. On each AP, connect with DNS
`1.1.1.1`, open Python and Google in sequence without warming the DNS cache
with ping, and repeat navigation after a large page/resource transfer. Confirm
that the Wi-Fi link and IP ping remain usable and that the downloads complete.
Capture `net` and `wifidiag` immediately if any step fails. Relevant fields are
the last failed HTTP URL, last transport error, TCP gaps/resets/timeouts,
RCR/net_type/response rates, Retry/PN evidence and AP disconnect reason.

Duplicate retransmissions can occur normally. A zero replay count is not an
acceptance criterion, and replay/MIC checks must not be disabled to hide loss.
