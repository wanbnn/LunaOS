Automatic LunaOS build from the commit identified by this release tag.

- `LunaOS.img`: BIOS/UEFI disk installation image.
- `LunaOS.iso`: installation and recovery media.
- `update.manifest` and `chunk-*.z`: authenticated update transport consumed by Settings → System → Check for updates. Keep these assets together.

The updater preserves the live LunaFS snapshot slots and installed disk identity.
It requires an installed compatible LunaOS disk, working internet access and at
least 256 MiB RAM. Keep the device powered throughout installation. This release
format does not yet provide recovery from power loss during boot-sector writes.
