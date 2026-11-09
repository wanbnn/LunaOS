# CPython for LunaOS

`python313.bundle` contains CPython 3.13.9 for wasm32-wasi and the Python
source modules from that distribution's standard library. Its upstream
distribution is the
[CPython 3.13.9 WASI SDK 24 build](https://github.com/brettcannon/cpython-wasi-build/releases/tag/v3.13.9).
The CPython license is in [CPython.txt](../licenses/CPython.txt).

Rebuild the bundle with the pinned source archive:

```sh
python3 tools/cpython-bundle.py assets/cpython/python313.bundle build/cpython/upstream.zip
```

Bundle SHA-256:
`472a8ca81a8822f61a019d6a84f58dc1b06371f234e54ca0e37688889576e417`.
