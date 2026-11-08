# litehtml + libstdc++ integration status (experimental)

## Sources and licenses
- litehtml v0.10 pinned as Git submodule in `third_party/litehtml` (BSD-3-Clause)
- Existing Lexbor v3 is the Luna Browser HTML5 parser; litehtml currently has a separate Gumbo-backed document model
- GCC static libstdc++ and its runtime are supplied **by the compiler/toolchain**, not vendored as an unlicensed binary
- Keep GCC's runtime library exception, GCC/libstdc++ notices, and source/build provenance for redistribution

## Current code
- `ui/litehtml_bridge.cpp`: C++17 `document_container` adapter with C ABI in `ui/litehtml_bridge.h`
- `ui/litehtml_graphics.c`: native Luna text, rectangle, clipping callbacks; image callback is currently not implemented
- `lib/cxx_runtime.cpp`: heap-backed new/delete and selected ABI hooks
- `tests/litehtml_bridge_test.cpp`: host smoke test for flex layout and drawing callbacks
- `Makefile`: `make litehtml-native` builds i386 static renderer; `LUNA_LITEHTML=1` attempts a kernel link
- `.github/workflows/litehtml.yml`: host test and experimental kernel link

## Critical release gates
1. Complete toolchain compatibility: host Ubuntu static libstdc++ may depend on glibc/Linux symbols and is **not** a valid LunaOS runtime until a full freestanding static link passes.
2. litehtml v0.10 uses standard-library headers, exceptions and RTTI. C++ runtime must provide all referenced ABI and unwinder functions; inspect `ld` output for Linux dependencies.
3. Replace/bridge the real `web_layout` and `web_paint` call paths, not just provide a demo renderer. Current WebNode and live QuickJS tree are independent of litehtml's DOM; mutations must invalidate and resync the view.
4. Implement resource resolution and non-placeholder image drawing, font metrics, gradients, clips and link hit-testing; verify scroll offsets and relative URLs.
5. Build ISO and verify actual guest rendering, smoke sites, firmware/driver regressions and OTA package creation.

**Do not claim litehtml is enabled in the release based only on the host smoke test.**
The default ISO continues using the prior Luna CSS/layout until those gates pass.
