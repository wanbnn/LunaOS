# Calendar and system time

Click the clock in the top bar to open the monthly calendar. Use the arrows to
browse months, **Today** to return to the current month, or **Date & time
settings** to configure the clock. The same controls are under Settings >
Date & time (Data e hora). Months and weekday labels follow the interface
language. The calendar supports Gregorian dates from 1970 through 2199.

Manual configuration works without a network. Enter `YYYY-MM-DD`, `HH:MM:SS`
and a fixed UTC offset such as `-03:00`, then click **Apply manual time**.
This disables automatic synchronization so that a pending network reply cannot
overwrite the manual choice. Invalid input is rejected. The offset range is
UTC-12:00 through UTC+14:00; IANA timezone rules and seasonal daylight saving
changes are not implemented.

Automatic synchronization is enabled by default. **Sync now** also works when
automatic synchronization is disabled. The default server is
`time.cloudflare.com`, UDP port 123, using unicast SNTPv4. Cloudflare documents
this as a public, free anycast NTP service:
https://developers.cloudflare.com/time-services/ntp/usage/ . The server is
editable; no provider's continued availability is assumed. The implementation
follows the packet format described in https://www.rfc-editor.org/rfc/rfc4330 .
The generic NTP Pool hostname is deliberately not shipped as a distribution
default: https://www.ntppool.org/en/vendors.html .

The service queues DNS and UDP requests without blocking the desktop. It waits
for IPv4 connectivity, retries failures with exponential backoff, and refreshes
a successful synchronization every six hours. There is no dependency on Wi-Fi:
any active IPv4 interface supported by the network stack can carry the traffic.
The OS must have a working network driver and IP configuration first.

Time remains UTC internally; the configured offset affects display and
`localtime_r`, not `gettimeofday` or TLS timestamps. Adjusting wall time never
changes scheduler/uptime timers. An unsigned elapsed-time accumulator tolerates
the 32-bit monotonic millisecond counter wrapping while the service is polled.
The RTC is read with bounded, repeated coherent snapshots and decoding of
BCD/binary and 12/24-hour modes. It is assumed to contain UTC. RTC data registers
are not written. `/etc/luna-clock` persists the UTC offset, automatic mode,
server/port and correction relative to the physical RTC; the RTC continues to
account for elapsed time while powered off. If hardware time is invalid, the
last saved checkpoint is available, but elapsed powered-off time cannot be
reconstructed without a working RTC or network.

UDP source address/ports/checksum, DNS transaction/question/answer names,
NTP version, server mode, leap alarm, stratum, origin nonce, timestamp bounds
and root timing quality are checked. Kiss-o'-Death replies never set the clock.
This is **unauthenticated SNTP**, not NTS; checks do not protect against a
network attacker able to forge the chosen server's replies. Network era
unfolding supports 2000–2099, including the 2036 NTP rollover. Manual dates use
the full 1970–2199 calendar range. The existing 32-bit TLS timestamp API returns
failure time zero beyond its representable Unix range (2106); clock storage and
the calendar use 64-bit time.

## Terminal

```
date
date zone -03:00
date set 2032-02-29 12:34:56
date auto on
date auto off
date sync
date server time.cloudflare.com
date server 10.0.2.2 41234
```

`date set` interprets the value in the configured UTC offset. Settings are saved
automatically by clock commands; disk failures are reported while the chosen
time remains active in memory. The Settings save button also persists clock
configuration. On read-only boot media, persistence requires writable storage
supported by the filesystem.

## Verification

```
sh tests/calendar_unit.sh
make all -j4
LUNA_TEST_DIR=/tmp/luna-calendar-guest python3 tests/calendar_guest.py
make test-wifi-station
```

Unit tests cover date conversion over the supported range, leap-year rules,
UTC offsets, RTC modes, NTP validation and era handling, DNS validation,
persistence, monotonic wrap and cancellation. The guest test boots offline,
sets a leap-day date, rejects an invalid date, opens the calendar/settings,
reboots to verify persistence, and synchronizes against a local UDP fixture
through the actual guest network stack. This test does not require an external
NTP server and does not claim public-service reachability. Screenshots are
written to `dist/calendar.png` and `dist/date-time-settings.png`.
