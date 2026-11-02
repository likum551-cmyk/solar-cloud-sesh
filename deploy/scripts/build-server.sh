#!/usr/bin/env bash
# Build gage-server from source on the VPS and run its two test programs.
#
#   build-server.sh [--src DIR] [--build-dir DIR] [--jobs N] [--clean] [--skip-tests] [--dry-run]
#
# Run it as the deploy user (never root): it only writes inside the build directory.
# Output: <build-dir>/server/gage-server (the binary), <build-dir>/selftest/{netcode,server}_test.
# Exit 0 only when the build succeeded AND both test programs passed (or --skip-tests).
#
# CMake entry point: the repository root with -DGAGE_SERVER_ONLY=ON (the documented command).
# If the root CMakeLists.txt does not (yet) define that option, the script falls back to
# apps/server, whose CMakeLists.txt defines the same targets (its header says so). Both put
# the outputs at the same relative paths, so everything downstream is identical.
set -euo pipefail

SCRIPT_NAME=build-server
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"

SRC=${GAGE_SRC:-}
BUILD_DIR=''
JOBS=''
CLEAN=0
SKIP_TESTS=0
TEST_TIMEOUT=${TEST_TIMEOUT:-600}

usage() {
  sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case $1 in
    --src) [ $# -ge 2 ] || usage_die "--src needs a value"; SRC=$2; shift 2 ;;
    --build-dir) [ $# -ge 2 ] || usage_die "--build-dir needs a value"; BUILD_DIR=$2; shift 2 ;;
    --jobs) [ $# -ge 2 ] || usage_die "--jobs needs a value"; JOBS=$2; shift 2 ;;
    --clean) CLEAN=1; shift ;;
    --skip-tests) SKIP_TESTS=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h | --help) usage; exit 0 ;;
    *) usage_die "unknown argument: $1 (try --help)" ;;
  esac
done

if [ "$(id -u)" -eq 0 ]; then
  die "do not build as root: run as the deploy user"
fi

# Default source root: the repository this kit lives in (deploy/scripts/ -> two levels up).
if [ -z "$SRC" ]; then
  SRC=$(cd "$SCRIPT_DIR/../.." && pwd)
fi
[ -d "$SRC" ] || die "source directory not found: $SRC"
SRC=$(cd "$SRC" && pwd)
[ -f "$SRC/CMakeLists.txt" ] || die "$SRC has no CMakeLists.txt (not the repository root?)"
[ -f "$SRC/cmake/gage_server.cmake" ] || die "$SRC/cmake/gage_server.cmake missing"
[ -f "$SRC/apps/server/CMakeLists.txt" ] || die "$SRC/apps/server/CMakeLists.txt missing (expected the apps/ layout)"

[ -n "$BUILD_DIR" ] || BUILD_DIR=$SRC/build/server
if [ -n "$JOBS" ]; then
  is_uint "$JOBS" && [ "$JOBS" -ge 1 ] || usage_die "--jobs must be a positive integer"
else
  # A 2 GB VPS can run out of memory with one compiler per core on large C++; cap at 4.
  JOBS=$(nproc 2>/dev/null || echo 2)
  [ "$JOBS" -le 4 ] || JOBS=4
fi

need_cmd cmake ninja g++ gcc timeout sha256sum
cmake_ver=$(cmake --version | head -n1 | sed 's/[^0-9.]*//; s/[^0-9.].*//')
cmake_major=${cmake_ver%%.*}
cmake_minor=${cmake_ver#*.}
cmake_minor=${cmake_minor%%.*}
if [ "$cmake_major" -lt 3 ] || { [ "$cmake_major" -eq 3 ] && [ "$cmake_minor" -lt 16 ]; }; then
  die "cmake $cmake_ver is older than the 3.16 the project requires"
fi

if grep -Eq 'option\(GAGE_SERVER_ONLY|if\(GAGE_SERVER_ONLY' "$SRC/CMakeLists.txt"; then
  CMAKE_SOURCE=$SRC
  CMAKE_ARGS=(-DGAGE_SERVER_ONLY=ON)
  log "CMake entry: repository root with -DGAGE_SERVER_ONLY=ON"
else
  CMAKE_SOURCE=$SRC/apps/server
  CMAKE_ARGS=()
  warn "root CMakeLists.txt does not define GAGE_SERVER_ONLY yet; using apps/server (same targets, same output paths)"
fi

if [ "$CLEAN" = 1 ]; then
  # Only ever delete a directory literally named .../build/server (or a --build-dir that ends in /build/*).
  case $BUILD_DIR in
    */build/*) ;;
    *) die "refusing --clean: build dir must be under a directory named build (got $BUILD_DIR)" ;;
  esac
  [ "$BUILD_DIR" != "/" ] || die "refusing --clean on /"
  log "cleaning $BUILD_DIR"
  run rm -rf -- "$BUILD_DIR"
fi

log "configure: cmake -S $CMAKE_SOURCE -B $BUILD_DIR -G Ninja -DCMAKE_BUILD_TYPE=Release ${CMAKE_ARGS[*]:-}"
run cmake -S "$CMAKE_SOURCE" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release "${CMAKE_ARGS[@]}"

log "build with $JOBS job(s)"
run cmake --build "$BUILD_DIR" --parallel "$JOBS"

BIN=$BUILD_DIR/server/gage-server
NETCODE=$BUILD_DIR/selftest/netcode_test
SERVERT=$BUILD_DIR/selftest/server_test

if [ "$DRY_RUN" = 1 ]; then
  log "dry run: would verify $BIN and run $NETCODE and $SERVERT (timeout ${TEST_TIMEOUT}s each)"
  exit 0
fi

[ -x "$BIN" ] || die "build finished but $BIN is missing or not executable"

if [ "$SKIP_TESTS" = 1 ]; then
  warn "--skip-tests: the binary has NOT been tested on this machine; do not enable it for players"
else
  # The tests start in-process servers on 127.0.0.1 with OS-assigned ports and write ban files
  # relative to the working directory: run them in a throwaway directory.
  scratch=$(mktemp -d "${TMPDIR:-/tmp}/gage-server-tests.XXXXXX")
  trap 'rm -rf -- "$scratch"' EXIT
  for t in "$NETCODE" "$SERVERT"; do
    [ -x "$t" ] || die "test program missing: $t"
    log "running $(basename "$t") (timeout ${TEST_TIMEOUT}s)"
    set +e
    (cd "$scratch" && timeout "$TEST_TIMEOUT" "$t") >"$scratch/$(basename "$t").log" 2>&1
    rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
      warn "$(basename "$t") exited $rc; full log follows"
      cat "$scratch/$(basename "$t").log" >&2
      die "tests failed: the binary must not be deployed"
    fi
    # Surface the SKIPPED lines (server_test skips one admin check on a host with no non-loopback address).
    grep -i 'skipped' "$scratch/$(basename "$t").log" || true
    log "$(basename "$t"): passed"
  done
fi

sum=$(sha256sum "$BIN" | cut -d' ' -f1)
log "OK: $BIN"
log "sha256 $sum"
