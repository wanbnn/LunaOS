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


## HTML5 Browser — Lexbor backend

This build enables the pinned Lexbor v3.0.0 HTML tokenizer and tree builder by default in the Luna Browser, including HTML5 recovery, templates and SVG/MathML parsing. The Lexbor tree is currently adapted into LunaOS's legacy DOM used for layout and QuickJS. DOM Events, Shadow DOM slotting and browser security isolation remain incomplete. This is a test build; preserve backups and use a writable installed disk for Luna Update Engine v2.
