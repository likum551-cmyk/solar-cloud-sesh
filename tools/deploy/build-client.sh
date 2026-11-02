#!/usr/bin/env bash
# Build the Windows client on the Ubuntu VPS: launcher UI, cross-compile with mingw-w64 (-posix), prove the result,
# run the selftests under wine, and package the installer with Inno Setup under wine. Run it as root on the VPS from
# anywhere; it uses the source tree it lives in (/opt/gage/src after tools/deploy/unpack-source.sh).
#
#   build-client.sh [--dry-run] [--clean] [--skip-ui] [--tests gate|soft|strict|off] [--no-installer] [--jobs N]
#
#   --dry-run       run every read-only check (markers, CRLF, test list), print what the real run would do, change nothing.
#   --clean         delete <build>/win first (needed when the toolchain or compiler changed since the last configure).
#   --skip-ui       do not run npm ci / npm run build; use apps/launcher-ui/dist as it is (it must exist).
#   --tests MODE    gate (default)  gate tests stop the build, soft tests are reported only
#                   soft            nothing stops the build; use for the first runs on a new VPS
#                   strict          every non-skipped test must pass
#                   off             run no tests (the PE gate and the installer still run)
#                   The classification of every test is in tools/deploy/wine-selftests.list.
#   --no-installer  stop after the tests (no ISCC run)
#   --jobs N        parallel compile jobs (default: number of CPUs)
#
# Steps, in order (a failure stops the run at that step, nothing is published):
#   1 preflight   markers in CMakeLists.txt, no CRLF in the shell scripts, Node >= 22.12, kClientVersion, build dir layout
#   2 wine        prefix created once, Inno Setup copied to /opt/gage/inno/6.7.3, ISCC.exe smoke test (fails early, before the compile)
#   3 launcher UI npm ci + npm run build in apps/launcher-ui
#   4 configure   cmake with cmake/toolchain-mingw-linux.cmake and the baked update URL / public key; the cache is read back
#   5 compile     cmake --build (every target, including the selftests)
#   6 prove       UI byte count, baked URL and key found inside the launcher exe, PE import allow-list, bin/ layout
#   7 selftests   by name under wine, from wine-selftests.list; unclassified / stale / uncounted tests fail the run
#   8 installer   installer/build-installer.sh, unchanged, through the cygpath and iscc shims in tools/deploy/shim
# Output: <build>/win/installer/SolarRP-Setup-<kClientVersion>.exe (names: CLIENT_EXE / INSTALLER_PREFIX, lib/common.sh). Publish it with tools/deploy/publish-client.sh.
# Logs: /opt/gage/logs/<UTC stamp>-<step>.log (whole, never truncated). Exit codes: 0 ok, 1 failure, 2 usage, 75 busy.
set -euo pipefail

SCRIPT_NAME=build-client
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }

CLEAN=0; SKIP_UI=0; TESTS_MODE=gate; DO_INSTALLER=1; JOBS=''
while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) DRY_RUN=1; shift ;;
    --clean) CLEAN=1; shift ;;
    --skip-ui) SKIP_UI=1; shift ;;
    --tests) [ $# -ge 2 ] || usage_die "--tests needs a value"; TESTS_MODE=$2; shift 2 ;;
    --no-installer) DO_INSTALLER=0; shift ;;
    --jobs) [ $# -ge 2 ] || usage_die "--jobs needs a number"; JOBS=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage_die "unknown option $1" ;;
  esac
done
case $TESTS_MODE in gate|soft|strict|off) ;; *) usage_die "--tests must be gate, soft, strict or off (got '$TESTS_MODE')" ;; esac
[ -z "$JOBS" ] || { is_uint "$JOBS" && [ "$JOBS" -ge 1 ]; } || usage_die "--jobs must be a positive number"

deploy_config
[ -n "$JOBS" ] || JOBS=$(nproc 2>/dev/null || echo 2)

SRC=$GAGE_SRC
BUILD_WIN=$GAGE_BUILD_ROOT/win
BIN=$BUILD_WIN/bin
SELFTEST=$BUILD_WIN/selftest
UI_DIR=$SRC/apps/launcher-ui
TOOLCHAIN=$SRC/cmake/toolchain-mingw-linux.cmake
INNO_SRC=$SRC/.toolchain/innosetup-6.7.3
export GAGE_INNO_DIR=${GAGE_INNO_DIR:-$GAGE_OPT/inno/6.7.3}
export GAGE_OPT GAGE_WINEPREFIX
SHIM=$DEPLOY_DIR/shim
LOGDIR=$GAGE_OPT/logs
TMPROOT=$GAGE_OPT/tmp
LIST=$DEPLOY_DIR/wine-selftests.list
STAMP=$(date -u +%Y%m%dT%H%M%SZ)

log "source $SRC | build $BUILD_WIN | jobs $JOBS | tests $TESTS_MODE | $([ "$DRY_RUN" = 1 ] && echo DRY RUN || echo real run)"
if [ "$DRY_RUN" = 0 ]; then
  [ "$(id -u)" = 0 ] || warn "not root: the wine prefix, the logs and the build tree under $GAGE_OPT are meant to belong to root"
  mkdir -p "$LOGDIR" "$TMPROOT"
fi
acquire_lock build

PROBLEMS=0
# fail_check MESSAGE: stops a real run; in a dry run it is counted and the run goes on.
fail_check() {
  if [ "$DRY_RUN" = 1 ]; then warn "dry-run: a real run would stop here: $1"; PROBLEMS=$((PROBLEMS + 1))
  else die "$1"; fi
}

# run_logged NAME CMD...: whole output into $LOGDIR/<stamp>-NAME.log; on failure show the first error lines and return the code.
run_logged() {
  local name=$1 lf rc=0; shift
  lf="$LOGDIR/$STAMP-$name.log"
  log "$name: $* (log: $lf)"
  if [ "$DRY_RUN" = 1 ]; then log "[dry-run] not run"; return 0; fi
  "$@" > "$lf" 2>&1 || rc=$?
  if [ $rc -ne 0 ]; then
    warn "$name failed with exit $rc. First error lines (the whole log is $lf):"
    grep -n -m 25 -i -E 'error|fatal|FAILED|No such file|cannot' "$lf" >&2 || head -n 25 "$lf" >&2 || true
    return $rc
  fi
  return 0
}

# ---- 1 preflight -------------------------------------------------------------------------------------------
log "== 1/8 preflight"
for f in CMakeLists.txt cmake/toolchain-mingw-linux.cmake installer/build-installer.sh installer/gage-mp.iss \
         apps/launcher/gage_gui.cpp apps/launcher-ui/package.json tools/embed_ui.cmake; do
  [ -f "$SRC/$f" ] || fail_check "source tree incomplete: $SRC/$f is missing (re-pack with tools/deploy/pack-source.sh)"
done

# Project lesson 2026-10-29 (three times): an edit of CMakeLists.txt made from an older copy silently rolled back the
# new-UI embed, crypt32, the launcher-assets copy and the GAGE_* cache options. Grep for each before building.
for m in 'launcher-ui/dist' 'crypt32' 'launcher-assets' 'GAGE_TEST_SERVER' 'GAGE_API_BASE' 'GAGE_UPDATE_URL' 'GAGE_UPDATE_PUBKEY'; do
  grep -q -F -- "$m" "$SRC/CMakeLists.txt" || fail_check "CMakeLists.txt no longer mentions '$m' (an older copy overwrote it?). Fix the source on the PC and re-pack"
done
log "CMakeLists.txt markers present (launcher-ui/dist, crypt32, launcher-assets, GAGE_TEST_SERVER, GAGE_API_BASE, GAGE_UPDATE_URL, GAGE_UPDATE_PUBKEY)"

# A tarball made from a Windows checkout can carry CRLF into scripts; bash then fails on `\r` in odd places.
# (Counted with tr/wc, not grep: `grep $'\r'` inside $(...) matched every line under Git Bash while the same call outside it did not.)
has_cr() { [ "$(tr -d '\r' < "$1" | wc -c)" != "$(wc -c < "$1")" ]; }
crlf_files=''
for f in "$SRC/installer/build-installer.sh" "$DEPLOY_DIR"/*.sh "$DEPLOY_DIR"/lib/*.sh "$DEPLOY_DIR"/shim/*; do
  [ -f "$f" ] || continue
  if has_cr "$f"; then crlf_files="$crlf_files $f"; fi
done
[ -z "$crlf_files" ] || fail_check "CRLF line endings in:$crlf_files. Fix: sed -i 's/\\r\$//' <file> (or re-pack from a checkout with LF endings)"

# A tar made on NTFS may lose the execute bit; the shims must be executable (build-installer.sh tests `-x "$ISCC"`).
[ "$DRY_RUN" = 1 ] || chmod +x "$SHIM/cygpath" "$SHIM/iscc" "$SHIM/wine-exe" "$SRC/installer/build-installer.sh"

VERSION=$(client_version)
log "kClientVersion = $VERSION"

if command -v node >/dev/null 2>&1; then
  nv=$(node -p 'process.versions.node')
  [ "$(ver_cmp "$nv" 22.12.0)" -ge 0 ] || fail_check "Node $nv is too old: apps/launcher-ui needs >= 22.12.0 (bootstrap-vps.sh installs Node 22 from NodeSource)"
  log "node $nv"
else
  fail_check "node is not installed"
fi
need_cmd cmake ninja npm timeout
CXX_POSIX=$(find_tool x86_64-w64-mingw32-g++-posix || true)
[ -n "$CXX_POSIX" ] || fail_check "x86_64-w64-mingw32-g++-posix not found (apt package g++-mingw-w64-x86-64-posix; bootstrap-vps.sh)"
[ -f "$INNO_SRC/ISCC.exe" ] || fail_check "$INNO_SRC/ISCC.exe missing: pack-source.sh must include .toolchain/innosetup-6.7.3"
ls "$SRC"/installer/art/out/wizard-*.png >/dev/null 2>&1 || fail_check "no installer wizard art (installer/art/out/wizard-*.png): run 'node installer/art/render.mjs' on the PC and re-pack"

# installer/build-installer.sh writes to <repo>/build/win/installer whatever we pass; the build tree must be that path.
if [ "$(readlink -m "$BUILD_WIN")" != "$(readlink -m "$SRC/build/win")" ]; then
  fail_check "build dir $BUILD_WIN is not $SRC/build/win (also through symlinks): build-installer.sh writes the installer to <repo>/build/win/installer. Make $SRC/build a symlink to $GAGE_BUILD_ROOT (tools/deploy/unpack-source.sh does)"
fi

# The public key baked into the launcher: the signing key's public half when it is on this machine, else the CMake default.
PUBKEY=''
if [ -f "$GAGE_KEY_DIR/update-signing-pub.txt" ]; then
  PUBKEY=$(tr -d '[:space:]' < "$GAGE_KEY_DIR/update-signing-pub.txt")
  [[ $PUBKEY =~ ^[A-Za-z0-9+/]{86}==$ ]] || fail_check "$GAGE_KEY_DIR/update-signing-pub.txt is not an 88-character base64 public key"
  log "baked public key: from $GAGE_KEY_DIR/update-signing-pub.txt (the key publish-client.sh signs with)"
else
  warn "no $GAGE_KEY_DIR/update-signing-pub.txt: the launcher is built with the key in CMakeLists.txt; publish-client.sh refuses a release when that is not your signing key"
fi
log "baked update URL: $BAKED_MANIFEST_URL"

# ---- 2 wine, Inno Setup ------------------------------------------------------------------------------------
log "== 2/8 wine and Inno Setup"
. "$DEPLOY_LIB_DIR/wine-env.sh"
if ! wine_env_init; then
  WINE_BIN=''
  fail_check "wine / winepath / wineserver not found (tools/deploy/bootstrap-vps.sh installs wine64 and wine32:i386)"
fi
if [ -n "${WINE_BIN:-}" ]; then
  disp=${DISPLAY:-none}; [ -z "${XVFB_RUN:-}" ] || disp="virtual (xvfb-run)"
  log "wine: $("$WINE_BIN" --version 2>/dev/null || echo '?') | prefix $WINEPREFIX | display: $disp"
  if [ "$DRY_RUN" = 1 ]; then
    [ -f "$WINEPREFIX/system.reg" ] && log "wine prefix exists" || log "[dry-run] would create the wine prefix (wineboot --init)"
  else
    if [ ! -f "$WINEPREFIX/system.reg" ]; then
      log "creating the wine prefix $WINEPREFIX (first run, takes a minute)"
      mkdir -p "$WINEPREFIX"
      run_logged wineboot wine_run wineboot --init || die "wineboot failed; see the log above"
      timeout 120 "$WINESERVER_BIN" -w || warn "wineserver still busy after wineboot"
    fi
  fi
fi

# Inno Setup copy: stable path outside the source tree (which is replaced by unpack-source.sh); re-copied when the source differs.
inno_fingerprint() { (cd "$1" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1); }
want_fp=''; [ -d "$INNO_SRC" ] && want_fp=$(inno_fingerprint "$INNO_SRC")
have_fp=$(cat "$GAGE_INNO_DIR/.gage-src-fingerprint" 2>/dev/null || true)
if [ -n "$want_fp" ] && [ "$want_fp" = "$have_fp" ] && [ -f "$GAGE_INNO_DIR/ISCC.exe" ]; then
  log "Inno Setup copy is current: $GAGE_INNO_DIR"
elif [ "$DRY_RUN" = 1 ]; then
  log "[dry-run] would copy $INNO_SRC -> $GAGE_INNO_DIR"
else
  log "copying Inno Setup: $INNO_SRC -> $GAGE_INNO_DIR"
  mkdir -p "$(dirname "$GAGE_INNO_DIR")"
  rm -rf -- "$GAGE_INNO_DIR.new"
  cp -a -- "$INNO_SRC" "$GAGE_INNO_DIR.new"
  printf '%s\n' "$want_fp" > "$GAGE_INNO_DIR.new/.gage-src-fingerprint"
  rm -rf -- "$GAGE_INNO_DIR"
  mv -T -- "$GAGE_INNO_DIR.new" "$GAGE_INNO_DIR"
fi

# ISCC.exe, ISCmplr.dll and ISPP.dll are 32-bit: without wine32 this is where it shows, minutes before the compile would.
if [ "$DRY_RUN" = 1 ]; then
  log "[dry-run] would smoke-test ISCC.exe (no arguments: it prints its banner)"
else
  iscc_out=$("$SHIM/iscc" 2>&1 || true)
  if ! grep -q 'Inno Setup' <<< "$iscc_out"; then
    printf '%s\n' "$iscc_out" | head -n 20 >&2
    die "ISCC.exe did not run under wine. ISCC.exe is a 32-bit program: is wine32:i386 installed (dpkg --add-architecture i386; apt install wine32:i386)? See docs/OPS.md, 'Inno Setup does not run'."
  fi
  log "ISCC.exe runs: $(grep -m1 'Inno Setup' <<< "$iscc_out")"
fi

# ---- 3 launcher UI -----------------------------------------------------------------------------------------
log "== 3/8 launcher UI"
if [ "$SKIP_UI" = 1 ]; then
  log "--skip-ui: using $UI_DIR/dist as it is"
else
  run_logged ui-build bash -c 'cd "$1" && npm ci --no-audit --no-fund && npm run build' _ "$UI_DIR" || die "launcher UI build failed"
fi
if [ "$DRY_RUN" = 0 ] || [ -f "$UI_DIR/dist/index.html" ]; then
  [ -s "$UI_DIR/dist/index.html" ] || fail_check "$UI_DIR/dist/index.html is missing or empty"
  [ -s "$UI_DIR/dist/assets/manifest.json" ] || fail_check "$UI_DIR/dist/assets/manifest.json is missing (the exe serves only assets listed there)"
fi
UI_BYTES=0; [ -f "$UI_DIR/dist/index.html" ] && UI_BYTES=$(file_size "$UI_DIR/dist/index.html")
log "dist/index.html = $UI_BYTES bytes"

# ---- 4 configure -------------------------------------------------------------------------------------------
log "== 4/8 configure"
CACHE=$BUILD_WIN/CMakeCache.txt
cache_get() { sed -n "s/^$1:[A-Z]*=//p" "$CACHE" | head -n 1; }
if [ "$CLEAN" = 1 ]; then
  case $BUILD_WIN in /?*/win) ;; *) die "refusing to delete '$BUILD_WIN'" ;; esac
  [ "$BUILD_WIN" != /win ] || die "refusing to delete '$BUILD_WIN'"
  log "--clean: removing $BUILD_WIN"
  run rm -rf -- "$BUILD_WIN"
fi
if [ -f "$CACHE" ] && [ "$CLEAN" = 0 ]; then
  cached_tc=$(cache_get CMAKE_TOOLCHAIN_FILE)
  [ "$cached_tc" = "$TOOLCHAIN" ] || fail_check "$BUILD_WIN was configured with toolchain '${cached_tc:-none}', not $TOOLCHAIN. Re-run with --clean"
fi
# bin/ui is filled by file(COPY ...) at configure time and never pruned: a renamed file would ship in the installer.
run rm -rf -- "$BIN/ui"
# A launcher with neither a platform API nor a test server runs the demo UI and refuses EVERY PLAY with code 13
# (gage_gui.cpp, "No Solar RP server is set up yet"): no player could join anything. Until apps/platform runs, the
# live server is reached as the test server.
if [ -z "${GAGE_API_BASE:-}" ] && [ -z "${GAGE_TEST_SERVER:-}" ]; then
  fail_check "neither GAGE_API_BASE nor GAGE_TEST_SERVER is set (/etc/gage/deploy.env): this launcher would refuse every PLAY with code 13. Until the platform API runs, set GAGE_TEST_SERVER=$PUBLIC_IP:24800 there"
fi
CMAKE_ARGS=(-S "$SRC" -B "$BUILD_WIN" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN"
            "-DGAGE_UPDATE_URL=$BAKED_MANIFEST_URL")
[ -z "$PUBKEY" ] || CMAKE_ARGS+=("-DGAGE_UPDATE_PUBKEY=$PUBKEY")
[ -z "${GAGE_API_BASE:-}" ] || CMAKE_ARGS+=("-DGAGE_API_BASE=$GAGE_API_BASE")
[ -z "${GAGE_TEST_SERVER:-}" ] || CMAKE_ARGS+=("-DGAGE_TEST_SERVER=$GAGE_TEST_SERVER")
run_logged configure cmake "${CMAKE_ARGS[@]}" || die "cmake configure failed"
if [ "$DRY_RUN" = 0 ]; then
  [ "$(cache_get GAGE_UPDATE_URL)" = "$BAKED_MANIFEST_URL" ] || die "CMakeCache GAGE_UPDATE_URL is '$(cache_get GAGE_UPDATE_URL)', expected $BAKED_MANIFEST_URL"
  if [ -n "$PUBKEY" ]; then
    [ "$(cache_get GAGE_UPDATE_PUBKEY)" = "$PUBKEY" ] || die "CMakeCache GAGE_UPDATE_PUBKEY does not equal the signing public key"
  else
    PUBKEY=$(cache_get GAGE_UPDATE_PUBKEY)
  fi
  # The toolchain file sets CMAKE_CXX_COMPILER as a plain variable, so CMakeCache.txt has no entry for it (the first
  # VPS run died here on an empty cache value). The compiler CMake actually detected is in
  # CMakeFiles/<cmake version>/CMakeCXXCompiler.cmake: set(CMAKE_CXX_COMPILER "<path>").
  CXX_USED=$(sed -n 's/^set(CMAKE_CXX_COMPILER "\(.*\)")$/\1/p' "$BUILD_WIN"/CMakeFiles/*/CMakeCXXCompiler.cmake 2>/dev/null | head -n 1)
  grep -q 'mingw32' <<< "$CXX_USED" || die "the detected C++ compiler is '$CXX_USED' (CMakeFiles/*/CMakeCXXCompiler.cmake): the toolchain file was not applied"
  grep -q -- '-posix' <<< "$CXX_USED" || die "the detected C++ compiler '$CXX_USED' is not the -posix compiler"
  log "configured: compiler $CXX_USED"
fi

# ---- 5 compile ---------------------------------------------------------------------------------------------
log "== 5/8 compile"
run_logged build cmake --build "$BUILD_WIN" --parallel "$JOBS" || die "compile failed"

# ---- 6 prove -----------------------------------------------------------------------------------------------
log "== 6/8 prove the build"
if [ "$DRY_RUN" = 1 ]; then
  log "[dry-run] would check: kUiHtmlLen = $UI_BYTES, baked URL and key inside $CLIENT_EXE, PE import allow-list on 5 files, bin/ layout"
else
  got=$(sed -n 's/.*kUiHtmlLen = \([0-9][0-9]*\);.*/\1/p' "$BUILD_WIN/gen/ui_html.h")
  [ "$got" = "$UI_BYTES" ] || die "the exe embeds $got UI bytes but dist/index.html has $UI_BYTES: a stale ui_html.h or a CMakeLists.txt that no longer embeds apps/launcher-ui/dist"
  log "embedded UI = dist/index.html ($UI_BYTES bytes)"

  if [ ! -s "$BIN/$CLIENT_EXE" ]; then
    # The player-facing name is part of the Solar RP rename in the source tree (CMake target / output name).
    others=$(cd "$BIN" && ls -- *.exe 2>/dev/null | tr '\n' ' ' || true)
    die "$BIN/$CLIENT_EXE was not built (found: ${others:-no .exe}). Has the SolarLauncher rename landed in CMakeLists.txt? Or set CLIENT_EXE in $GAGE_ENV_FILE."
  fi
  where=$(ops_js file-contains "$BIN/$CLIENT_EXE" "$BAKED_MANIFEST_URL") || die "the baked update URL is not inside $BIN/$CLIENT_EXE"
  log "baked update URL found in $CLIENT_EXE ($where)"
  where=$(ops_js file-contains "$BIN/$CLIENT_EXE" "$PUBKEY") || die "the update public key is not inside $BIN/$CLIENT_EXE"
  log "baked public key found in $CLIENT_EXE ($where)"

  PE_FILES=("$BIN/$CLIENT_EXE" "$BIN/gageinjector.dll" "$BIN/gage_loader.dll" "$BIN/gage_mp.dll" "$BIN/gage_launch_shim.dll")
  for f in "${PE_FILES[@]}"; do [ -s "$f" ] || die "expected build output missing: $f"; done
  # No runtime DLL may appear next to GTA: the import allow-list is the proof that -static/-static-libgcc held.
  node "$DEPLOY_LIB_DIR/pe-imports.mjs" --allow "$DEPLOY_DIR/pe-allow.txt" --machine x64 "${PE_FILES[@]}" > "$LOGDIR/$STAMP-pe-imports.txt" 2>&1 \
    || { cat "$LOGDIR/$STAMP-pe-imports.txt" >&2; die "PE import gate failed: a built binary imports a DLL that is not in tools/deploy/pe-allow.txt (a mingw runtime DLL would be missing on the player's PC)"; }
  log "PE import gate: ${#PE_FILES[@]} files, only allow-listed system DLLs (details: $LOGDIR/$STAMP-pe-imports.txt)"
  for f in ui/index.html launcher-assets/manifest.json; do [ -s "$BIN/$f" ] || die "bin/$f missing: build-installer.sh and the installer need it"; done
fi

# ---- 7 selftests under wine --------------------------------------------------------------------------------
log "== 7/8 selftests under wine (--tests $TESTS_MODE)"
trim() { local s=$1; s=${s#"${s%%[![:space:]]*}"}; s=${s%"${s##*[![:space:]]}"}; printf '%s' "$s"; }
T_NAME=(); T_MODE=(); T_TMO=(); T_ARGS=(); T_WHY=()
declare -A LISTED=()
parse_list() {
  local line n a b c d e
  [ -f "$LIST" ] || die "missing $LIST"
  while IFS= read -r line || [ -n "$line" ]; do
    line=${line%$'\r'}
    line=$(trim "$line")
    case $line in ''|'#'*) continue ;; esac
    IFS='|' read -r a b c d e <<< "$line"
    n=$(trim "$a"); b=$(trim "$b"); c=$(trim "$c"); d=$(trim "$d"); e=$(trim "$e")
    [ "$d" = - ] && d=''
    [[ $n =~ ^[a-z0-9_]+$ ]] || die "$LIST: bad test name '$n'"
    [ -z "${LISTED[$n]:-}" ] || die "$LIST: '$n' listed twice"
    case $b in gate|soft|driver|skip) ;; *) die "$LIST: '$n' has mode '$b' (gate, soft, driver or skip)" ;; esac
    is_uint "$c" || die "$LIST: '$n' has timeout '$c'"
    if [ "$b" != skip ]; then [ "$c" -ge 1 ] || die "$LIST: '$n' needs a timeout"; fi
    [ -n "$e" ] || die "$LIST: '$n' has no reason"
    LISTED[$n]=1; T_NAME+=("$n"); T_MODE+=("$b"); T_TMO+=("$c"); T_ARGS+=("$d"); T_WHY+=("$e")
  done < "$LIST"
  [ ${#T_NAME[@]} -gt 0 ] || die "$LIST holds no tests (an empty list would pass vacuously)"
}
parse_list
N_RUN=0; N_SKIP=0
for m in "${T_MODE[@]}"; do if [ "$m" = skip ]; then N_SKIP=$((N_SKIP + 1)); else N_RUN=$((N_RUN + 1)); fi; done
log "$LIST: ${#T_NAME[@]} tests classified ($N_RUN to run, $N_SKIP skipped on purpose)"
for i in "${!T_NAME[@]}"; do [ "${T_MODE[$i]}" != driver ] || [ -f "$SRC/tests/${T_NAME[$i]}.mjs" ] || fail_check "${T_NAME[$i]}: driver tests/${T_NAME[$i]}.mjs is missing"; done

if [ "$DRY_RUN" = 1 ]; then
  for i in "${!T_NAME[@]}"; do log "[dry-run] ${T_NAME[$i]}: ${T_MODE[$i]}$([ "${T_MODE[$i]}" = skip ] && echo " (skipped: ${T_WHY[$i]})")"; done
elif [ "$TESTS_MODE" = off ]; then
  log "--tests off: no selftest is run"
else
  # Rule 1 and 2 of the list: nothing unclassified in selftest/, nothing listed that is not there.
  shopt -s nullglob
  built=("$SELFTEST"/*.exe)
  shopt -u nullglob
  [ ${#built[@]} -gt 0 ] || die "no *.exe in $SELFTEST: the build produced no selftests (an empty run would pass vacuously)"
  declare -A BUILT=()
  unclassified=()
  for f in "${built[@]}"; do
    b=$(basename "$f" .exe); BUILT[$b]=1
    [ -n "${LISTED[$b]:-}" ] || unclassified+=("$b")
  done
  [ ${#unclassified[@]} -eq 0 ] || die "selftests not classified in tools/deploy/wine-selftests.list: ${unclassified[*]}. Add each with a mode and a reason (a new test must not be silently ignored)"
  for n in "${T_NAME[@]}"; do [ -n "${BUILT[$n]:-}" ] || die "wine-selftests.list names '$n' but $SELFTEST/$n.exe does not exist (stale entry or a test that failed to build)"; done

  BIN_WIN=$(wine_win_path "$BIN") || die "winepath failed for $BIN"
  RESULTS=(); FAIL_GATE=(); FAIL_SOFT=(); RAN=0; SKIPPED=0
  for i in "${!T_NAME[@]}"; do
    name=${T_NAME[$i]}; mode=${T_MODE[$i]}
    if [ "$mode" = skip ]; then
      SKIPPED=$((SKIPPED + 1)); RESULTS+=("$(printf '%-22s %-6s skipped  %s' "$name" skip "${T_WHY[$i]}")"); continue
    fi
    # Effective strictness for this run.
    eff=$mode
    case $TESTS_MODE in
      soft) eff=soft ;;
      strict) eff=gate ;;
      gate) [ "$mode" = driver ] && eff=soft ;;
    esac
    work="$TMPROOT/selftest/$name"; lf="$LOGDIR/$STAMP-test-$name.log"
    # Not under the wine prefix: some tests find their source files through a Unix __FILE__ path, which wine only
    # resolves when the current directory is on the Z: drive.
    rm -rf -- "$work"; mkdir -p "$work"
    exe="$SELFTEST/$name.exe"
    args=${T_ARGS[$i]//\{BIN\}/$BIN_WIN}
    rc=0; t0=$SECONDS
    if [ "$mode" = driver ]; then
      wrapper="$work/run-$name"
      printf '#!/usr/bin/env bash\nexec %q %q "$@"\n' "$SHIM/wine-exe" "$exe" > "$wrapper"
      chmod +x "$wrapper"
      log "test $name: node tests/$name.mjs --exe $wrapper (timeout ${T_TMO[$i]}s, $eff)"
      (cd "$SRC" && timeout -k 10 "${T_TMO[$i]}" node "tests/$name.mjs" --exe "$wrapper") < /dev/null > "$lf" 2>&1 || rc=$?
    else
      log "test $name: wine $name.exe ${args} (timeout ${T_TMO[$i]}s, $eff)"
      read -r -a a <<< "$args" || true
      (cd "$work" && timeout -k 10 "${T_TMO[$i]}" "$SHIM/wine-exe" "$exe" "${a[@]}") < /dev/null > "$lf" 2>&1 || rc=$?
    fi
    RAN=$((RAN + 1)); dt=$((SECONDS - t0))
    # Whatever the test started in the prefix must be gone before the next one; report, never kill.
    timeout 60 "$WINESERVER_BIN" -w || warn "wineserver for $WINEPREFIX still has processes after $name (left alone)"
    if [ $rc -eq 0 ]; then
      res=PASS
    else
      res=FAIL; [ $rc -ne 124 ] || res=TIMEOUT
      warn "$name: $res (exit $rc, ${dt}s, $eff). First lines of $lf:"
      head -n 15 "$lf" >&2 || true
      if [ "$eff" = gate ]; then FAIL_GATE+=("$name"); else FAIL_SOFT+=("$name"); fi
    fi
    RESULTS+=("$(printf '%-22s %-6s %-8s %4ss  exit %s  %s' "$name" "$eff" "$res" "$dt" "$rc" "$lf")")
  done
  # Rule 3: the count.
  [ "$RAN" = "$N_RUN" ] && [ "$SKIPPED" = "$N_SKIP" ] || die "ran $RAN tests and skipped $SKIPPED, the list classifies $N_RUN to run and $N_SKIP to skip"
  [ "$RAN" -gt 0 ] || die "no selftest ran"
  log "---- selftest summary ($RAN run, $SKIPPED skipped, ${#FAIL_GATE[@]} gate failures, ${#FAIL_SOFT[@]} soft failures)"
  printf '%s\n' "${RESULTS[@]}"
  [ ${#FAIL_SOFT[@]} -eq 0 ] || warn "soft failures (reported, not blocking): ${FAIL_SOFT[*]}"
  [ ${#FAIL_GATE[@]} -eq 0 ] || die "gate selftests failed: ${FAIL_GATE[*]} (logs in $LOGDIR/$STAMP-test-<name>.log; read the first failure)"
fi

# ---- 8 installer -------------------------------------------------------------------------------------------
log "== 8/8 installer"
INSTALLER=$BUILD_WIN/installer/$INSTALLER_PREFIX-$VERSION.exe
if [ "$DO_INSTALLER" = 0 ]; then
  log "--no-installer: stopping before ISCC"
else
  run_logged installer env "ISCC=$SHIM/iscc" "PATH=$SHIM:$PATH" "BIN=$BIN" bash "$SRC/installer/build-installer.sh" || die "installer build failed"
  if [ "$DRY_RUN" = 0 ]; then
    if [ ! -f "$INSTALLER" ]; then
      found=$(cd "$BUILD_WIN/installer" 2>/dev/null && ls -- *.exe 2>/dev/null | tr '\n' ' ' || true)
      die "ISCC finished but $INSTALLER does not exist (found: ${found:-nothing}). Does installer/gage-mp.iss name its output ${INSTALLER_PREFIX}-<version> (the Solar RP rename)? Set INSTALLER_PREFIX in $GAGE_ENV_FILE otherwise. Output of build-installer.sh: $LOGDIR/$STAMP-installer.log"
    fi
    [ "$(head -c 2 "$INSTALLER")" = MZ ] || die "$INSTALLER is not a PE file"
    size=$(file_size "$INSTALLER")
    [ "$size" -ge 1048576 ] || die "$INSTALLER is only $size bytes: the payload (bin/, launcher-assets/) did not get in"
    log "installer: $INSTALLER ($size bytes) sha256 $(sha256_of "$INSTALLER")"
  fi
fi

if [ "$DRY_RUN" = 1 ]; then
  [ "$PROBLEMS" -eq 0 ] || die "dry-run found $PROBLEMS problem(s) a real run would stop on" 1
  log "dry-run: every check that can run here passed. Nothing was changed."
  exit 0
fi
log "done. Next: tools/deploy/publish-client.sh $VERSION --notes \"...\" [--mandatory]"
