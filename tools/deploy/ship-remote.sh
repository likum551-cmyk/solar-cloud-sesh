#!/usr/bin/env bash
# The VPS half of tools/deploy/ship.sh. ship.sh uploads this file next to the source tarball and starts it detached
# (setsid nohup), so a dropped ssh connection never kills a release half way. Not meant to be run by hand, but it can be,
# to retry a ship whose upload already landed:
#
#   ship-remote.sh --rc FILE --tarball F --sha256 HEX --version X.Y.Z --notes TEXT
#                  [--packs TAR] [--countdown S] [--vps-tests] [--dry-run]
#
# Steps: 1 unpack-source.sh (checksum, swap the tree; "already the current tree" is fine) 2 cd into the NEW tree
# 3 publish-packs.sh --dry-run, then the real publish (only with --packs) 4 release.sh (--skip-tests unless --vps-tests).
# --dry-run: unpack-source.sh --dry-run and publish-packs.sh --dry-run only (that one runs from the CURRENT tree, the new
# one is not swapped in); release.sh is not run, because its own --dry-run checks the version against the swapped tree.
# The exit code (the first failing step's own code) is written to the --rc file, which ship.sh reads at the end.
set -uo pipefail

RC_FILE=''
if [ "${1:-}" = --rc ] && [ $# -ge 2 ]; then RC_FILE=$2; shift 2; fi
trap 'rc=$?; [ -z "$RC_FILE" ] || printf "%s\n" "$rc" > "$RC_FILE"' EXIT

log() { printf '[ship-remote %s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { log "ERROR: $1"; exit "${2:-1}"; }

SRC=${GAGE_SHIP_SRC:-/opt/gage/src}
TARBALL=''; SHA=''; VERSION=''; NOTES=''; PACKS=''; COUNTDOWN=''; VPS_TESTS=0; DRY=0
while [ $# -gt 0 ]; do
  case $1 in
    --tarball) [ $# -ge 2 ] || die "--tarball needs a value" 2; TARBALL=$2; shift 2 ;;
    --sha256) [ $# -ge 2 ] || die "--sha256 needs a value" 2; SHA=$2; shift 2 ;;
    --version) [ $# -ge 2 ] || die "--version needs a value" 2; VERSION=$2; shift 2 ;;
    --notes) [ $# -ge 2 ] || die "--notes needs a value" 2; NOTES=$2; shift 2 ;;
    --packs) [ $# -ge 2 ] || die "--packs needs a value" 2; PACKS=$2; shift 2 ;;
    --countdown) [ $# -ge 2 ] || die "--countdown needs a value" 2; COUNTDOWN=$2; shift 2 ;;
    --vps-tests) VPS_TESTS=1; shift ;;
    --dry-run) DRY=1; shift ;;
    *) die "unknown argument $1" 2 ;;
  esac
done
[ -n "$TARBALL" ] && [ -n "$SHA" ] && [ -n "$VERSION" ] && [ -n "$NOTES" ] || die "--tarball, --sha256, --version and --notes are required" 2
[ -f "$TARBALL" ] || die "no such file: $TARBALL"
[ -z "$PACKS" ] || [ -f "$PACKS" ] || die "no such packs file: $PACKS"

log "ship $VERSION | tarball $TARBALL | packs ${PACKS:-none} | $([ "$DRY" = 1 ] && echo DRY RUN || echo real run)"

# ---- 1 unpack (swaps $SRC; the previous tree becomes src.prev) ---------------------------------------------------
log "== 1/3 unpack-source.sh"
unpack_args=("$TARBALL" --sha256 "$SHA")
[ "$DRY" = 0 ] || unpack_args+=(--dry-run)
bash "$SRC/tools/deploy/unpack-source.sh" "${unpack_args[@]}" || die "unpack-source.sh failed (exit $?); nothing is live" $?

# ---- 2 into the new tree (the old working directory may now be src.prev) ----------------------------------------
cd "$SRC" || die "cannot cd into $SRC"

# ---- 3 car packs --------------------------------------------------------------------------------------------------
if [ -n "$PACKS" ]; then
  log "== 2/3 publish-packs.sh (dry run first)"
  bash tools/deploy/publish-packs.sh "$PACKS" --dry-run || die "publish-packs.sh --dry-run failed (exit $?); the packs and the client are unchanged" $?
  if [ "$DRY" = 0 ]; then
    bash tools/deploy/publish-packs.sh "$PACKS" || die "publish-packs.sh failed (exit $?); the client is NOT released" $?
  fi
else
  log "== 2/3 no --packs: car packs unchanged"
fi

# ---- 4 release ----------------------------------------------------------------------------------------------------
rel_args=("$VERSION" --notes "$NOTES")
[ "$VPS_TESTS" = 1 ] || rel_args+=(--skip-tests)
[ -z "$COUNTDOWN" ] || rel_args+=(--countdown "$COUNTDOWN")
if [ "$DRY" = 1 ]; then
  log "== 3/3 [dry-run] would run: bash tools/deploy/release.sh $(printf '%q ' "${rel_args[@]}")"
  log "dry run finished: the tarball and the packs passed their checks; nothing is live."
  exit 0
fi
log "== 3/3 release.sh"
bash tools/deploy/release.sh "${rel_args[@]}"
rc=$?
[ "$rc" = 0 ] || die "release.sh exited $rc (see its exit-code table: 4 = server rolled back, client published; 5 = rollback failed too; 75 = another run holds the lock)" "$rc"
log "SHIP_RESULT=ok version=$VERSION"
exit 0
