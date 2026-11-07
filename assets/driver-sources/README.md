Pinned upstream Linux wireless source archive for port development.

The archive preserves drivers/net/wireless, COPYING and LICENSES from the commit
in manifest.json. SHA-256 is checked by the LunaOS image builder. Original notices
remain in the archived source files. These sources are included in the raw image
as source material; they are not compiled or loaded as functional LunaOS drivers.

Regenerate with: python3 tools/vendor_wireless.py PATH_TO_PINNED_LINUX_CHECKOUT
The checkout needs drivers/net/wireless, LICENSES and COPYING. The normal LunaOS
build is offline and uses this vendored archive rather than fetching current master.
