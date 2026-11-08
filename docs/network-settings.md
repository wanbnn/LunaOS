# Network settings and power status

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
