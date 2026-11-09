# wasm3

Core interpreter from wasm3 v0.5.0 (MIT), excluding upstream host-specific
WASI adapters. The LunaOS adapter is in `apps/cpython/`.

Source: https://github.com/wasm3/wasm3/releases/tag/v0.5.0
Archive SHA-256: b778dd72ee2251f4fe9e2666ee3fe1c26f06f517c3ffce572416db067546536c


LunaOS adds a deadline hook at loop back edges and uses Clang's mandatory
tail calls for i386 interpreter dispatch to bound host stack growth. It
also passes the floating-point dispatch register as bits to avoid x87 work
on integer operations.
