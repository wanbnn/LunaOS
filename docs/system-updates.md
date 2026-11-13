# Luna Update Engine v2

Settings → System → **Check for updates / Verificar atualizações** checks the
exact SHA of the official `main` branch. An installed writable LunaOS disk,
working internet and at least 256 MiB RAM are required; live media are not
updated. The installed commit is shown in Settings. No metrics or telemetry are
stored or sent by the updater; important events go to the kernel's serial log.

## One continuous HTTPS package

A new official release has `update.pack` as **one update package**, replacing
the previous `update.manifest` plus hundreds of individual `chunk-*.z`
downloads. It contains the validated `LUNAUPD1` 9,984-byte manifest followed
in order by the compressed payload of every nonempty block. The v1 manifest
layout and per-block SHA-256 remain unchanged. The file is downloaded directly
into bounded RAM allocations through one HTTPS body stream; there is no
temporary file on the live LunaFS snapshot volume. During the transfer, the
network callback only parses the manifest and copies compressed blocks into
RAM. **After HTTPS finishes**, the updater inflates and SHA-256-verifies every
included block in a distinct 40–50% progress phase; no disk sectors are
modified until that phase succeeds. This avoids stalling Wi-Fi RX/TCP polling
while a 256 KiB chunk is decompressed and hashed. The package still requires
enough RAM for its compressed payload, and the two phases are sequential,
not parallel workers.

For update-package downloads, the TCP stack initially allocates up to
**1 MiB of receive buffer** after the TLS connection is established, then
auto-tunes the window based on measured delivery rate and TCP RTT, up to
**10 MiB** when the peer negotiates window scaling and enough RAM is
available. Ten MiB is a *conditional ceiling*, never a mandatory upfront
allocation. The limit can be lower with little RAM or no TCP window scaling.
Failed large allocations retain the previous working buffer. TCP also
coalesces small window-credit updates and avoids shrinking the ring while
fresh packets are arriving. Other HTTP requests retain their existing
adaptation policy.

At 50 ms RTT, 10 MiB is enough receive credit for up to ~1.68 Gbit/s
**window-bound** throughput; it is not a measured radio or download rate.
Driver RX processing, WPA2 decryption, TLS overhead, losses and remote
congestion control still limit end-to-end speed.

For an installed clean build, LunaOS first requests
`update-from-<installed full commit SHA>.pack` for the exact target commit.
If that asset is absent (404), it downloads `update.pack` instead. The
differential format has an 80-byte header: `LUNADLT2`, the 40-byte exact
installed base SHA, and a 32-byte included-block bitmap; it then has the same
target manifest and only the compressed changed-block payloads. The initial
MBR/boot block is always included. Excluded live filesystem snapshot sectors
are never included.

To allow existing v1 installations to migrate directly to the newest commit,\nthe release pipeline ALSO publishes legacy \`update.manifest\` and \`chunk-*.z\`\nassets. The v2 engine never requests those legacy assets; it always transfers\nexactly one \`.pack\` per update attempt. Legacy assets may only be removed\nafter all v1 clients are retired.\n\nThe publication workflow builds a delta against the preceding published
release when that release's `LunaOS.img` can be downloaded. Older bases
without a corresponding delta use the full package. A locally modified or
dirty build also uses the full package. Delta blocks omitted from the package
must already match the target manifest SHA-256 on disk; if this check fails,
the installer switches to the full package before any writes.

## Validation, installation and recovery

Each downloaded block is decoded and SHA-256 checked before any OS disk write.
For each full block, the updater compares the verified target bytes against
the existing disk and skips unchanged sectors. User settings and files are
saved first; **only changed sectors** are backed up to RAM, before any write.

Up to eight 256 KiB blocks are written per bounded batch, followed by a disk
flush and a readback comparison of every written block. The first boot block is
written last. The installed MBR's disk ID and partition table are preserved.
A write/flush/readback failure triggers verified restoration from the RAM
backup. Failed updates never reboot.

**Power-loss recovery is still not implemented.** The backup lives in RAM;
cutting power or resetting while writes are in flight can leave a mixed boot
image. Keep the device powered; use installation media if required. A/B
boot slots and persistent rollback remain future work.

HTTPS requires valid hostnames, certificates and redirects; HTTP downgrade
is prohibited. No offline release-signing key is in this version: trust
remains anchored in the official GitHub repository and its release assets.

The Settings progress bar shows bytes received and current speed during
1–40% (download), SHA-256 block progress during 40–50% (verification), recovery
blocks and verified installed blocks afterward. It does not collect telemetry.

Run `make test-update` for full/delta package format, exact revision identity,
streaming verification, invalid package handling, network errors, disk
comparisons, failure-injected writes/reads/flush, rollback and reboot behavior.
Physical hardware updating still requires validation before relying on it.
