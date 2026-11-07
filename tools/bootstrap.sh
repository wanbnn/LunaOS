#!/usr/bin/env bash
# Install build-only Python tools locally; never changes the guest operating system.
set -euo pipefail
cd "$(dirname "$0")/.."
if command -v uv >/dev/null; then
  uv pip install --python "$(command -v python3)" --target .tools/python -r requirements-dev.txt
else
  python3 -m pip install --target .tools/python -r requirements-dev.txt
fi

if ! find /usr/lib/gcc .tools/usr/lib/gcc -path '*/32/libgcc.a' -print -quit 2>/dev/null | rg -q .; then
  echo "A 32-bit GCC runtime is required: install gcc-multilib on the host."
fi
