#!/usr/bin/env bash
# Package the built launcher into build/win/installer/SolarRP-Setup-<version>.exe.
# Build first (cmake --build build/win, see CLAUDE.md), and render the wizard
# art once (node installer/art/render.mjs) when it changes.
#   ISCC=<path to ISCC.exe>  overrides the Inno Setup compiler
#   BIN=<dir>                overrides the folder holding SolarLauncher.exe and the DLLs
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISCC="${ISCC:-$ROOT/.toolchain/innosetup-6.7.3/ISCC.exe}"
BIN="${BIN:-$ROOT/build/win/bin}"

[ -x "$ISCC" ] || { echo "error: Inno Setup compiler not found at $ISCC" >&2; exit 1; }

VERSION="$(sed -n 's/.*kClientVersion = L"\([^"]*\)".*/\1/p' "$ROOT/apps/launcher/gage_gui.cpp")"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "error: no kClientVersion in gage_gui.cpp (got '$VERSION')" >&2; exit 1; }

for f in SolarLauncher.exe gageinjector.dll gage_loader.dll gage_mp.dll gage_launch_shim.dll ui/index.html; do
  [ -f "$BIN/$f" ] || { echo "error: $BIN/$f missing; build first" >&2; exit 1; }
done
ls "$ROOT"/installer/art/out/wizard-*.png >/dev/null 2>&1 || { echo "error: no wizard art; run node installer/art/render.mjs" >&2; exit 1; }

# ISCC takes /D switches: keep MSYS from rewriting them into paths
MSYS_NO_PATHCONV=1 "$ISCC" /Q "/DAppVersion=$VERSION" "/DBinDir=$(cygpath -w "$BIN")" "$(cygpath -w "$ROOT/installer/gage-mp.iss")"

OUT="$ROOT/build/win/installer/SolarRP-Setup-$VERSION.exe"
[ -f "$OUT" ] || { echo "error: $OUT was not produced" >&2; exit 1; }
echo "$OUT"
