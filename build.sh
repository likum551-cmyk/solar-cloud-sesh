#!/usr/bin/env bash
# GAGE-MP build script — cross-compiles the client to Windows PE x86_64.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build/win"

CC="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
CXX="${MINGW_CXX:-x86_64-w64-mingw32-g++}"

command -v cmake  >/dev/null || { echo "error: cmake not found"        >&2; exit 1; }
command -v ninja  >/dev/null || { echo "error: ninja not found"        >&2; exit 1; }
command -v "$CXX" >/dev/null || { echo "error: $CXX not found"        >&2; exit 1; }

cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$BUILD/bin"

cmake --build "$BUILD" --parallel

echo
echo "Built artifacts:"
ls -la "$BUILD/bin"
echo
test -f "$BUILD/bin/SolarLauncher.exe" || { echo "error: SolarLauncher.exe missing from bin/" >&2; exit 1; }
echo "Validate the launcher PE (expect: subsystem 0x2 GUI, static CRT, no WebView2Loader.dll import):"
x86_64-w64-mingw32-objdump -p "$BUILD/bin/SolarLauncher.exe" 2>/dev/null | grep -E "Subsystem|Import Address Table|DLL Name" | head -n 25 || true
echo
echo "Done. Run the client with:"
echo "  bin/SolarLauncher.exe     # THE launcher (React+Tailwind SPA in embedded WebView2)"
echo "  selftest/gage_v.exe --selftest  # CLI: inject the whole chain into one process and verify"
echo "  selftest/gage_v.exe --dump      # CLI: resolve server + print the plan, no injection"
