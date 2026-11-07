#!/usr/bin/env bash
# Install build-only Python tools locally; never changes the guest operating system.
set -euo pipefail
cd "$(dirname "$0")/.."
if command -v uv >/dev/null; then
  uv pip install --python "$(command -v python3)" --target .tools/python -r requirements-dev.txt
else
  python3 -m pip install --target .tools/python -r requirements-dev.txt
fi

# Validate the host toolchain without requiring ripgrep or assuming a
# specific GCC installation directory. This is a warning only: this script
# installs Python build tools, not system packages.
if ! printf '#include <string.h>\nint main(void) { return 0; }\n' |
     gcc -m32 -x c -o /dev/null - >/dev/null 2>&1; then
  echo "Warning: 32-bit GCC headers/runtime are missing or incomplete." >&2
  echo "Ubuntu/Debian: sudo apt install gcc-multilib libc6-dev-i386" >&2
fi
