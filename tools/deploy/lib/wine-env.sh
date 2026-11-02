#!/usr/bin/env bash
# Wine helpers for tools/deploy (the Windows-only tools run on the Ubuntu VPS: Inno Setup's ISCC.exe, the selftests).
# Source it after lib/common.sh and deploy_config:
#
#   . "$DEPLOY_LIB_DIR/wine-env.sh"
#   wine_env_init            # exports WINEPREFIX etc., finds wine / winepath / wineserver
#   wine_run some.exe args   # runs under xvfb-run when there is no DISPLAY
#
# The shims in tools/deploy/shim/ source this file too (they are run by installer/build-installer.sh, which knows
# nothing about wine), so it must work with only GAGE_WINEPREFIX / GAGE_OPT in the environment.

# find_tool is defined in common.sh; shims that source only this file get a copy.
if ! declare -F find_tool >/dev/null 2>&1; then
  find_tool() { local n; for n in "$@"; do if command -v "$n" >/dev/null 2>&1; then printf '%s' "$n"; return 0; fi; done; return 1; }
fi

wine_env_init() {
  : "${GAGE_OPT:=/opt/gage}"
  : "${GAGE_WINEPREFIX:=$GAGE_OPT/wine}"
  # No WINEARCH: wine(1) says a win64 prefix runs 64-bit applications and 32-bit ones in WoW64 mode, and refuses to
  # start when WINEARCH disagrees with the prefix. Leaving it unset never mismatches. ISCC.exe is 32-bit x86, so
  # the prefix must be WoW64 (needs wine32:i386; tools/deploy/bootstrap-vps.sh installs it).
  unset WINEARCH
  export WINEPREFIX="$GAGE_WINEPREFIX"
  export WINEDEBUG="${WINEDEBUG:--all}"
  # mscoree / mshtml empty: do not offer to download Mono / Gecko when the prefix is created (no GUI here).
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
  WINE_BIN=$(find_tool wine wine-stable wine64) || return 1
  WINEPATH_BIN=$(find_tool winepath winepath-stable) || return 1
  WINESERVER_BIN=$(find_tool wineserver wineserver-stable) || return 1
  XVFB_RUN=''
  if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then XVFB_RUN=$(command -v xvfb-run); fi
  export WINE_BIN WINEPATH_BIN WINESERVER_BIN XVFB_RUN
}

# wine_run ARGS...: wine, behind a virtual X server when no display exists (a few Windows programs create a window
# even when run from a console; xvfb-run needs the xauth package).
wine_run() {
  if [ -n "${XVFB_RUN:-}" ]; then "$XVFB_RUN" -a "$WINE_BIN" "$@"; else "$WINE_BIN" "$@"; fi
}

# wine_win_path UNIXPATH: the Windows spelling (Z:\opt\gage\...) of a Unix path, for arguments to Windows programs.
wine_win_path() {
  local out
  out=$("$WINEPATH_BIN" -w "$1") || return 1
  printf '%s' "${out%$'\r'}"
}
