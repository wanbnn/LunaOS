# Automatic system updates

Settings → System → **Check for updates / Verificar atualizações** performs an
update immediately when a new official build is available. The machine must be
installed on a compatible writable LunaOS disk, have internet access (Ethernet
or Wi-Fi) and at least 256 MiB RAM. Live installation media are not updated.
The running commit is shown above the button. The machine restarts automatically
only after a complete, verified installation. User files, account, preferences,
disk identity and partition entries are preserved.

## Releases and pipeline

`.github/workflows/updates.yml` builds every push to the official repository's
`main` branch, or a manual workflow run on `main`. It uses the exact checked-out
commit, runs updater and Wi-Fi contract tests, and creates a release tagged
`update-<full commit SHA>`. A release contains:

- `LunaOS.iso` for downloading, booting and installing LunaOS.
- `LunaOS.img` for writing a BIOS/UEFI disk image.
- `update.manifest` and `chunk-*.z` for the automatic updater.

Assets are uploaded to a draft first; the release becomes public only after all
uploads finish. The newest main build is marked Latest, while an older build
that finished after another push remains available without claiming to be latest.
A published commit release is not overwritten on rerun. No personal access token
is needed: the workflow uses its repository-scoped `GITHUB_TOKEN`, with
`contents: write` only in the publishing job. No release is published from a fork
or pull request. A first release requires committing and pushing this workflow
to `main`; preparing the local files does not publish anything on GitHub.

The guest reads `https://api.github.com/repos/wanbnn/LunaOS/git/ref/heads/main`,
then fetches assets only from the commit's fixed release URL. It never guesses a
branch's revision from a release title or installs an older available build when
the newest commit is still compiling. An unavailable release reports that the
build may still be running. A machine running a clean build of the same commit
reports that it is up to date. Local modified builds may be replaced by the
official build of their base commit.

## Format, verification and recovery

`tools/updatepack.py` produces a 9,984-byte v1 manifest: a 64-byte little-endian
header (`LUNAUPD1`, version, 66 MiB image size, 256 KiB chunk size, 248 records,
40-byte commit), followed by 40-byte records (destination sector, compressed
length, SHA-256 of the decoded chunk). Each asset is a bounded zlib stream;
zero-filled chunks require no asset. The decoder is the same bounded stb zlib
decoder already used by LunaOS's PNG support. The manifest layout is fixed and
excludes sectors 8192–16383, the two live LunaFS snapshot slots. It covers both
the native core and EFI partition, including firmware and archived driver sources.

All requests use certificate-validated HTTPS, including release-asset redirects;
HTTP downgrade is rejected. Integrity is SHA-256 against the manifest obtained
through HTTPS. This trusts the official GitHub repository and its release assets;
there is no separate offline release-signing key in this version.

Every chunk is downloaded, decoded and hashed before any OS sector is written.
Settings and user data are saved, then the old OS sectors are backed up in RAM.
Insufficient memory or read failures cancel before disk changes. Installation
writes in bounded batches, flushes and reads back each chunk, preserving the
installed MBR identity and partition entries and writing the first boot chunk
last. Disk write, flush or verification failures trigger readback-verified
restoration from RAM. Failed updates do not restart the machine. The Settings
task cannot be killed while the updater is running.

**Power-loss recovery is not implemented.** The recovery copy lives in RAM, so
a loss of power or reset during installation can leave a mixed boot image. Keep
the device powered on; recover using the ISO if necessary. Persistent A/B boot
slots and bootloader rollback are future work. Physical hardware and real GitHub
release installation still require validation; host tests use the real installer,
SHA and decoder with a simulated HTTPS server and failure-injected disk.

Run `make test-update` to verify package round-trip, commit identity, excluded
user-data regions, checksum errors, offline/current/live states, download and
allocation failures, successful install/restart, and write/flush/readback rollback.
