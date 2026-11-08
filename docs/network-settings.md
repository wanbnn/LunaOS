# Network settings and power status

Settings → Network includes a DNS row. Click it to cycle through Cloudflare
(`1.1.1.1`, the default), Google (`8.8.8.8`), and automatic DNS from DHCP.
Changes apply immediately; use **Save settings** to persist them.

LunaSH also accepts `dns 1.1.1.1`, `dns 8.8.8.8`, or another unicast IPv4
address. `dns auto` restores DHCP DNS. The command applies and saves the
preference to disk, reporting save failures; `dns` and `net` show the effective
server when IPv4 is ready. The choice applies to browser, updater, ping name
resolution, and network time, survives DHCP refreshes, and is stored as
`dns_server` in `/etc/luna-settings` (empty for automatic mode).

Foreground name resolution keeps the last successful IPv4 answer until its
TTL expires (at most five minutes), so a browser connection can reuse a name
just resolved by ping. Zero-TTL answers are not cached. Changing DNS or
reconnecting invalidates the cache. `net` reports cache hits; `wifidiag`
separates CCMP format, key, replay and MIC drops for physical-radio diagnosis.

Settings → Network uses the native `wifi_*` API for scanning and connection.
Three results are displayed per page; the arrow buttons show the remaining
results. Open and WPA2-PSK networks can be selected; WPA2 passwords are masked,
submitted with Enter or Connect, and cleared after submission or leaving the
page. Passwords are not saved. Receive-only backends show results but cannot
connect, and unsupported security is not offered as a connection action.

The Control Center Wi-Fi row displays the connection name and current state.
Clicking it opens Network and starts a scan. The name is captured by the common
connection API, including connections initiated in setup. A Wi-Fi icon replaces
the Ethernet icon when an operational radio is registered. Power toggles are
not offered because the current Wi-Fi driver contract has no radio-power API.

Battery status is read every ten seconds from ACPI PNP0C0A devices using _BIX
(or _BIF) and _BST. Valid capacities produce a percentage in the top bar and
Control Center; `+` indicates charging. Multiple packs are combined only when
capacity units match. Unknown capacity or unavailable AML methods produce no
percentage. Firmware requiring an unsupported Embedded Controller operation
region still needs an EC driver before these methods can supply battery data.
Physical notebook validation is required; the standard QEMU desktop has no
battery.
