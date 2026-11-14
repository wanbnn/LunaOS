# Upstream Git on LunaOS

The default image now contains the actual upstream Git 2.56.1 ELF32 binary
at `/bin/git` and upstream Dash 0.5.12 at `/bin/sh`.

See [native build, usage and limits](../../docs/git-native.md) and the
[current evidence](../../docs/git-port-status.md).

```
git submodule update --init --recursive
make git-native-build
make -j4 all
make test-git-native-guest
```

The isolated build script is `build-native.py`. It stages musl, Git and Dash
under `build/git-native`, leaving the kernel libc and upstream checkouts
unchanged. Unsupported ABI operations return errors rather than issuing Linux
syscall numbers to LunaOS. Source pins, licenses and build settings are
recorded in the source tree and `/etc/licenses` in the image.

`make git-upstream-host-test` remains a separate Linux comparison fixture.
Its success does not establish guest compatibility. HTTPS/SSH and full Git
acceptance are still pending; this is an experimental native port.
