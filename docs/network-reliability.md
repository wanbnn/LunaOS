# Wi-Fi and browser reliability investigation

## Plan and scope

1. Preserve the RTL8822BE join configuration that connected on the notebook
   and test transport bounds and retries.
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

The RTL8822BE join path preserves its original receive filter (`0xe400220f`)
and PHY-table response rates. The stricter filters, scan mode changes,
response-rate writes and mandatory MAC readback added in `aa09afe` were
withdrawn after the user reported persistent connection failures. The precise
hardware failure stage remains unknown because a diagnostic from the failing
version was unavailable. Physical validation of the restored join path remains
pending. The transmit bound accepts a full 1500-byte Ethernet payload after SNAP/CCMP encapsulation
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
- RTL8822BE register/DMA tests verify MAC/BSSID and mode, preserve receive
  filters and PHY-table rates across repeated attempts, and check descriptor
  sequence and full encrypted MTU. Client tests cover successful association
  after a failed attempt on the same instance.
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
- `make test-wifi-qemu` runs the native RTL8822BE driver through PCIe/MMIO/DMA
  with Ethernet disabled. An independent raw 802.11 peer verifies WPA2/CCMP,
  DHCP, DNS, ICMP, HTTP and a complete 350 KB stylesheet. It exercises a
  rejected authentication, wrong-password timeout, successful retry and
  reconnection after an AP disconnect. See [Wi-Fi emulation](wifi-emulation.md).

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

## Adaptive receive memory and timing

Receive storage starts at 16 KiB and grows with occupancy, up to the 65,535-byte
unscaled TCP window. Reordered segments use individually allocated nodes in
sequence order, with a byte budget tied to the receive buffer. Allocation
failure preserves existing data and credit, leaving missing bytes unacknowledged
for retransmission. Connection close and owner cancellation release the storage.

After sustained low occupancy, the target size decreases. Physical shrinking
waits until outstanding advertised credit fits the target. Window updates retain
the previous right edge while that credit drains, including sequence wraparound.
This implementation uses the existing unscaled window and stop-and-wait sender.

The sender measures RTT on unretransmitted SYN/data exchanges. Smoothed RTT and
RTT variation determine the RTO, bounded between one and 60 seconds. Timeout
backoff doubles the RTO. Karn's rule excludes ambiguous retransmission samples.
Receive recovery ACKs use progressively spaced intervals. Raw stream wait budgets
adapt separately within their caller-supplied bounds. [RFC 6298](https://www.rfc-editor.org/rfc/rfc6298.html)

HTTP uses its own 60-second idle policy and a five-minute total request deadline
across redirects, TLS and response reads. This total deadline also expires during
continuous trickle delivery. Streaming file downloads use the idle policy and
keep their total transfer duration unrestricted. The HTTP policy is independent
of the sender's retransmission timer.

`net` reports current receive/reorder memory, peak buffer size, latest RTT, RTO,
allocation failures and HTTP timeout counts. `make test-tcp` exercises growth,
credit-preserving shrink, allocation failure, reordering, timing wrap, RTT
estimation, retransmission exclusion, backoff and total HTTP deadline expiry.
