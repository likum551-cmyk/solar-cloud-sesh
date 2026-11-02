#!/usr/bin/env bash
# Solar RP VPS kit: ONE self-extracting file, made by tools/deploy/pack-vps-kit.sh on GI's PC. Run it on the VPS as root:
#
#   bash solar-rp-vps-kit-<stamp>.run [--dry-run] [--verify] [--extract-only] [install.sh options]
#
#   --dry-run       check the payload, unpack it into a scratch folder, run install.sh --dry-run from there (it prints every
#                   step and changes nothing), delete the scratch folder. Read this first.
#   --verify        only check the payload size and SHA-256, then stop (exit 0, or 3 on a mismatch)
#   --extract-only  check, unpack to /root/solar-rp-kit/<stamp>/ and stop (run its install.sh yourself later)
#   other options   handed to install.sh: --no-plugins --skip-claude --skip-memory --skip-source --force-source (see install.sh --help)
#
# What it does: finds the payload appended to this file, refuses unless its size and SHA-256 equal the values the packer wrote
# into this header, unpacks it to /root/solar-rp-kit/<stamp>/ (kept; older kits are not deleted) and runs install.sh from there:
# the Claude setup into /root/.claude, the source into /opt/gage/src, the plugins, and a printed "what next". It never runs
# bootstrap-vps.sh, a deploy, a publish or the game server. Running it twice is safe.
# KIT_EXTRACT_ROOT moves the extraction folder (tests). Exit codes: 0 ok, 1 failure, 2 usage, 3 payload damaged or not complete.
set -euo pipefail

KIT_STAMP='@@STAMP@@'
PAYLOAD_SHA256='@@SHA256@@'
PAYLOAD_BYTES=@@BYTES@@

SELF=${BASH_SOURCE[0]}
log() { printf '[vps-kit] %s\n' "$*"; }
die() { printf '[vps-kit] ERROR: %s\n' "$1" >&2; exit "${2:-1}"; }

MODE=install
PASS=()
for a in "$@"; do
  case $a in
    -h|--help) sed -n '2,/^set -euo/p;/^set -euo/q' "$SELF" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    --verify) MODE=verify ;;
    --extract-only) MODE=extract ;;
    --dry-run) MODE=dry; PASS+=("$a") ;;
    *) PASS+=("$a") ;;
  esac
done
for c in tar gzip sha256sum grep tail wc mktemp cut; do command -v "$c" >/dev/null 2>&1 || die "missing command: $c"; done
[[ $KIT_STAMP =~ ^[0-9]{8}T[0-9]{6}Z$ ]] || die "this file was not finished by the packer (stamp placeholder still in the header)" 3
[[ $PAYLOAD_SHA256 =~ ^[0-9a-f]{64}$ ]] || die "this file was not finished by the packer (checksum placeholder still in the header)" 3

TMP=$(mktemp -d "${TMPDIR:-/tmp}/vps-kit-run.XXXXXX")
trap 'rm -rf -- "$TMP"' EXIT

# the payload starts on the line after the marker; the marker is built from two pieces so this line is not the marker itself
MARK_A='__SOLAR_RP_KIT_'; MARK_B='PAYLOAD_BELOW__'
line=$(grep -a -n -m1 -x -F "${MARK_A}${MARK_B}" "$SELF" | cut -d: -f1 || true)
[[ $line =~ ^[0-9]+$ ]] || die "no payload marker in $SELF: the file is damaged or was edited" 3
tail -n +$((line + 1)) "$SELF" > "$TMP/payload.tgz"
got_bytes=$(wc -c < "$TMP/payload.tgz")
[ "$got_bytes" = "$PAYLOAD_BYTES" ] || die "payload is $got_bytes bytes, the header says $PAYLOAD_BYTES: the upload is incomplete or the file was changed (scp it again)" 3
got_sha=$(sha256sum "$TMP/payload.tgz" | cut -d' ' -f1)
[ "$got_sha" = "$PAYLOAD_SHA256" ] || die "payload SHA-256 mismatch: file $got_sha, header $PAYLOAD_SHA256. The upload is damaged; nothing was unpacked or installed." 3
log "kit $KIT_STAMP: payload ok ($got_bytes bytes, sha256 ${got_sha:0:16}...)"
[ "$MODE" != verify ] || exit 0

if [ "$MODE" = dry ]; then
  DEST=$TMP/extract/$KIT_STAMP
else
  ROOT=${KIT_EXTRACT_ROOT:-/root/solar-rp-kit}
  DEST=$ROOT/$KIT_STAMP
fi
if [ "$MODE" != dry ] && [ -e "$DEST" ]; then
  if [ "$(cat "$DEST/.payload-sha256" 2>/dev/null || true)" = "$PAYLOAD_SHA256" ] && [ -s "$DEST/install.sh" ]; then
    log "$DEST already holds this payload: using it"
  else
    die "$DEST exists but does not hold this payload; move it away and run again"
  fi
else
  PART=$DEST.partial.$$
  mkdir -p -- "$PART"
  chmod 700 -- "$PART"
  tar -xzf "$TMP/payload.tgz" -C "$PART" --no-same-owner || die "extraction failed (disk full?)"
  [ -s "$PART/install.sh" ] || die "the payload has no install.sh"
  printf '%s\n' "$PAYLOAD_SHA256" > "$PART/.payload-sha256"
  mkdir -p -- "$(dirname -- "$DEST")"
  mv -T -- "$PART" "$DEST"
  log "unpacked to $DEST"
fi

[ "$MODE" != extract ] || { log "stopped after unpacking (--extract-only). Next: bash $DEST/install.sh --dry-run"; exit 0; }
rc=0
bash "$DEST/install.sh" "${PASS[@]+"${PASS[@]}"}" || rc=$?
exit "$rc"
__SOLAR_RP_KIT_PAYLOAD_BELOW__
