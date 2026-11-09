# CPython for LunaOS

`python313.bundle` contains CPython 3.13.9 for wasm32-wasi and the Python
modules from that distribution's standard library, with common imports
precompiled. Its upstream
distribution is the
[CPython 3.13.9 WASI SDK 24 build](https://github.com/brettcannon/cpython-wasi-build/releases/tag/v3.13.9).
The CPython license is in [CPython.txt](../licenses/CPython.txt).

Rebuild the bundle using CPython 3.13.9 and the pinned source archive:

```sh
python3.13 tools/cpython-bundle.py assets/cpython/python313.bundle build/cpython/upstream.zip
```

Bundle SHA-256:
`6e47d94345e95d10f7e526846cd60745fe28c1ea23ef1d494329613cdb488680`.
