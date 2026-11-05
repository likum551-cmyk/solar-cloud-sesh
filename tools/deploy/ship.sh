#!/usr/bin/env bash
# One command from GI's PC (Git Bash) to the live release: pack the source, upload it, unpack it on the VPS, publish the
# car packs (optional) and run release.sh, with the VPS output streamed back here.
#
#   ship.sh <version> --notes TEXT [--packs FILE.tar] [--countdown S] [--vps-tests] [--allow-dirty] [--host USER@HOST]
#           [--dry-run]
#
#   <version>       x.y.z; must equal kClientVersion in apps/launcher/gage_gui.cpp (checked here, before anything runs)
#   --notes TEXT    the release notes the launcher shows (required, max 2000 characters)
#   --packs FILE    a packs .tar (tools/packs output, e.g. SolarRP-packs-0.4.18b.tar) to publish before the release;
#                   omitted = the published car packs stay as they are
#   --countdown S   passed to release.sh (default there: 60)
#   --vps-tests     run the selftests on the VPS too (default: release.sh --skip-tests, the tests ran on this PC)
#   --allow-dirty   ship even with uncommitted changes to tracked files (default: refuse; the release must be a commit)
#   --host          default root@5.175.253.187 (or $GAGE_SHIP_HOST)
#   --dry-run       pack and upload for real, then only unpack-source.sh --dry-run and publish-packs.sh --dry-run on the
#                   VPS; release.sh is not run and nothing goes live
#
# Connections: one scp (the tarball, ship-remote.sh and the packs) and one ssh. With a password login that is two
# password prompts; with an ssh key, none. On the VPS ship-remote.sh runs detached (setsid nohup) and logs to
# /opt/gage/logs/ship/<stamp>.log, so Ctrl+C here or a dropped connection only stops the WATCHING, never the release;
# ship.sh prints the command that re-attaches. Exit code: the VPS run's own (release.sh's table), 1 for a local failure.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
log() { printf '[ship] %s\n' "$*"; }
die() { printf '[ship] ERROR: %s\n' "$1" >&2; exit "${2:-1}"; }

VERSION=''; NOTES=''; NOTES_SET=0; PACKS=''; COUNTDOWN=''; VPS_TESTS=0; ALLOW_DIRTY=0; DRY=0
HOST=${GAGE_SHIP_HOST:-root@5.175.253.187}
SSH=${GAGE_SHIP_SSH:-ssh}; SCP=${GAGE_SHIP_SCP:-scp}
REMOTE_DIR=${GAGE_SHIP_REMOTE_DIR:-/root}
REMOTE_LOGS=${GAGE_SHIP_REMOTE_LOGS:-/opt/gage/logs/ship}
while [ $# -gt 0 ]; do
  case $1 in
    --notes) [ $# -ge 2 ] || die "--notes needs a value" 2; NOTES=$2; NOTES_SET=1; shift 2 ;;
    --packs) [ $# -ge 2 ] || die "--packs needs a file" 2; PACKS=$2; shift 2 ;;
    --countdown) [ $# -ge 2 ] && [[ $2 =~ ^[0-9]+$ ]] && [ "$2" -le 600 ] || die "--countdown needs seconds (0..600)" 2; COUNTDOWN=$2; shift 2 ;;
    --vps-tests) VPS_TESTS=1; shift ;;
    --allow-dirty) ALLOW_DIRTY=1; shift ;;
    --host) [ $# -ge 2 ] || die "--host needs USER@HOST" 2; HOST=$2; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) die "unknown option $1 (try --help)" 2 ;;
    *) [ -z "$VERSION" ] || die "more than one version given" 2; VERSION=$1; shift ;;
  esac
done

# ---- checks on this PC, before anything is packed or sent ---------------------------------------------------------
[[ $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "version '$VERSION' must be x.y.z (ship.sh 0.4.20 --notes \"...\")" 2
[ "$NOTES_SET" = 1 ] && [ -n "$NOTES" ] || die "--notes TEXT is required (the launcher shows it)" 2
[ ${#NOTES} -le 2000 ] || die "notes are ${#NOTES} characters, the limit is 2000" 2
[[ $HOST =~ ^[A-Za-z0-9._-]+@[A-Za-z0-9.-]+$ ]] || die "--host must look like user@host (got '$HOST')" 2
CODE_VERSION=$(sed -n 's/.*kClientVersion = L"\([^"]*\)".*/\1/p' "$ROOT/apps/launcher/gage_gui.cpp")
[ "$CODE_VERSION" = "$VERSION" ] || die "kClientVersion in apps/launcher/gage_gui.cpp is '$CODE_VERSION', not $VERSION (release.sh would refuse it on the VPS)"

if git -C "$ROOT" rev-parse --git-dir >/dev/null 2>&1; then
  # pack-source.sh packs the working tree, so uncommitted edits AND new untracked files would ship; the local content
  # folders it excludes anyway (DLC PACKS/, fresh_images/, fresh_videos/) do not count
  untracked=$(git -C "$ROOT" ls-files --others --exclude-standard | grep -v -E '^(DLC PACKS|fresh_images|fresh_videos)/' || true)
  if ! git -C "$ROOT" diff --quiet HEAD -- || [ -n "$untracked" ]; then
    git -C "$ROOT" status --short --untracked-files=no >&2
    [ -z "$untracked" ] || printf '%s\n' "$untracked" | sed 's/^/?? /' >&2
    if [ "$ALLOW_DIRTY" = 1 ]; then log "WARNING: shipping the uncommitted changes above (--allow-dirty)"
    else die "uncommitted changes or new files (above) would ship: commit first, or --allow-dirty"; fi
  fi
  ahead=$(git -C "$ROOT" rev-list --count '@{u}..HEAD' 2>/dev/null || echo '?')
  [ "$ahead" = 0 ] || log "WARNING: HEAD is $ahead commit(s) ahead of its upstream (not pushed); the release ships the working tree anyway"
  log "commit $(git -C "$ROOT" log -1 --format='%h %s')"
fi

PACKS_U=''; PACKS_NAME=''
if [ -n "$PACKS" ]; then
  [ -f "$PACKS" ] || die "--packs: no such file: $PACKS"
  case $PACKS in *.tar) ;; *) die "--packs must be a .tar (publish-packs.sh takes a folder or a .tar)" ;; esac
  PACKS_NAME=$(basename -- "$PACKS")
  [[ $PACKS_NAME =~ ^[A-Za-z0-9._-]+$ ]] || die "--packs file name may only contain letters, digits, . _ - (got '$PACKS_NAME')"
  # scp reads "H:/..." as host:path; the POSIX form /h/... has no colon before the first slash
  PACKS_U=$(cygpath -u "$PACKS" 2>/dev/null || printf '%s' "$PACKS")
  log "packs: $PACKS ($(($(wc -c < "$PACKS") / 1048576)) MiB, sha256 $(sha256sum "$PACKS" | cut -d' ' -f1))"
fi

# ---- 1 pack ----------------------------------------------------------------------------------------------------------
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
NAME=solar-rp-src-$STAMP
OUT_DIR=$(dirname "$ROOT")
TARBALL=$OUT_DIR/$NAME.tar.gz
log "== 1/3 pack the source ($NAME)"
bash "$ROOT/tools/deploy/pack-source.sh" --name "$NAME" --out "$OUT_DIR" > "$OUT_DIR/$NAME.pack.log" 2>&1 \
  || { cat "$OUT_DIR/$NAME.pack.log" >&2; die "pack-source.sh failed (its whole output is above)"; }
grep '^\[pack-source\]' "$OUT_DIR/$NAME.pack.log"
[ -f "$TARBALL" ] || die "pack-source.sh finished but $TARBALL does not exist"
SHA=$(sha256sum "$TARBALL" | cut -d' ' -f1)
PRINTED=$(sed -n 's/^\[pack-source\] sha256 \([0-9a-f]\{64\}\)$/\1/p' "$OUT_DIR/$NAME.pack.log")
[ "$SHA" = "$PRINTED" ] || die "the tarball's sha256 ($SHA) differs from what pack-source.sh printed ($PRINTED)"
rm -f -- "$OUT_DIR/$NAME.pack.log"

# ---- 2 upload (one scp, POSIX paths so "H:" is never read as a host) -------------------------------------------------
log "== 2/3 upload to $HOST:$REMOTE_DIR/"
HELPER_NAME=ship-remote-$STAMP.sh
TARBALL_U=$(cygpath -u "$TARBALL" 2>/dev/null || printf '%s' "$TARBALL")
HELPER_U=$(cygpath -u "$ROOT/tools/deploy/ship-remote.sh" 2>/dev/null || printf '%s' "$ROOT/tools/deploy/ship-remote.sh")
# the helper goes up under its stamped name: copy it beside the tarball first (scp keeps the source's base name)
HELPER_LOCAL=$OUT_DIR/$HELPER_NAME
cp -- "$ROOT/tools/deploy/ship-remote.sh" "$HELPER_LOCAL"
HELPER_U=$(cygpath -u "$HELPER_LOCAL" 2>/dev/null || printf '%s' "$HELPER_LOCAL")
files=("$TARBALL_U" "$HELPER_U")
[ -z "$PACKS_U" ] || files+=("$PACKS_U")
"$SCP" "${files[@]}" "$HOST:$REMOTE_DIR/" || { rm -f -- "$HELPER_LOCAL"; die "scp failed; nothing changed on the VPS"; }
rm -f -- "$HELPER_LOCAL"

# ---- 3 run on the VPS, detached, and follow its log ----------------------------------------------------------------
q() { printf '%q' "$1"; }
RLOG=$REMOTE_LOGS/$STAMP.log
RRC=$RLOG.rc
args=(--rc "$RRC" --tarball "$REMOTE_DIR/$NAME.tar.gz" --sha256 "$SHA" --version "$VERSION" --notes "$NOTES")
[ -z "$PACKS_NAME" ] || args+=(--packs "$REMOTE_DIR/$PACKS_NAME")
[ -z "$COUNTDOWN" ] || args+=(--countdown "$COUNTDOWN")
[ "$VPS_TESTS" = 0 ] || args+=(--vps-tests)
[ "$DRY" = 0 ] || args+=(--dry-run)
qargs=''
for a in "${args[@]}"; do qargs="$qargs $(q "$a")"; done
REMOTE="mkdir -p $(q "$REMOTE_LOGS") && : > $(q "$RLOG") && rm -f $(q "$RRC") || exit 1
setsid -w nohup bash $(q "$REMOTE_DIR/$HELPER_NAME")$qargs > $(q "$RLOG") 2>&1 < /dev/null &
pid=\$!
tail -n +1 -f --pid=\$pid $(q "$RLOG")
if [ -s $(q "$RRC") ]; then exit \"\$(cat $(q "$RRC"))\"; fi
echo '[ship] the VPS run left no exit code; read the log above'; exit 1"

log "== 3/3 run on the VPS ($([ "$DRY" = 1 ] && echo DRY RUN || echo 'real run: this goes LIVE'))"
log "log there: $RLOG   (Ctrl+C here stops watching only; re-attach: ssh $HOST tail -n +1 -f $RLOG)"
set +e
"$SSH" -o ServerAliveInterval=30 -o ServerAliveCountMax=6 "$HOST" "$REMOTE"
rc=$?
set -e
if [ "$rc" = 0 ]; then
  log "done: $VERSION $([ "$DRY" = 1 ] && echo 'dry run passed' || echo 'is live') ($NAME.tar.gz, sha256 $SHA)"
else
  log "the VPS run ended with exit $rc; its log: ssh $HOST cat $RLOG"
fi
exit "$rc"
