Automatic LunaOS build from the commit identified by this release tag.

- `LunaOS.img`: BIOS/UEFI disk image.
- `LunaOS.iso`: installation and recovery image.
- `update.pack`: **single full update package**, containing the verified manifest and all compressed blocks.
- `update-from-<base commit>.pack` (optional): single smaller differential update package for an exact installed commit.

Settings → System → Check for updates downloads one compatible package via
continuous certificate-verified HTTPS and verifies every block using SHA-256.
The updater preserves LunaFS user-data slots and installed disk identity.
It requires a compatible installed writable disk, working internet and at
least 256 MiB RAM. Keep the device powered: persistent power-loss rollback
is not yet implemented.
