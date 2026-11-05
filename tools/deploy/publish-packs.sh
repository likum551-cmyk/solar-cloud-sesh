#!/usr/bin/env bash
# Publish the car packs every launcher downloads before the game starts (apps/launcher/pack_sync.h,
# docs/UPDATES.md "Car packs"). Run on the VPS.
#
#   publish-packs.sh <packs folder | packs .tar> [--manifest packs.json|packs-gen9.json] [--no-http-check] [--dry-run]
#
#   --manifest       packs.json (default: the Legacy set) or packs-gen9.json (the GTA V Enhanced set, made by
#                    `stage_packs.mjs --gen9`; Enhanced launchers sync it into %LOCALAPPDATA%\GAGE-MP\packs-gen9).
#                    Both share the content-addressed blob folder update/packs/.
#
#   <packs folder>   one folder per pack, each with a dlc.rpf at its top (tools/packs/stage_packs.mjs output; on the PC
#                    that is %LOCALAPPDATA%\GAGE-MP\packs). A .tar of it (top level "packs/<name>/..." or "<name>/...")
#                    is unpacked into a temporary folder first.
#   The set published REPLACES the previous one: a pack missing from it is removed from every launcher that installed it.
#
# Order of effects (a failure at any step leaves the previous packs.json serving):
#   1 every file copied to /var/www/gage/update/packs/<sha256> (new names only; an existing blob is never rewritten)
#   2 packs.json signed into /var/www/gage/.staging/ and checked against update-signing-pub.txt
#   3 mv staging -> update/packs.json (atomic; the previous one is copied to $GAGE_RELEASES/manifests/)
#   4 HTTP check through Caddy: packs.json and one blob
# Old blobs stay (a launcher mid-download from the previous packs.json still finds its files). Never prints the private
# key. Exit codes: 0 ok, 1 failure, 2 usage, 75 another publish is running.
set -euo pipefail

SCRIPT_NAME=publish-packs
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }

SRC=''
HTTP_CHECK=1
MANIFEST=packs.json
while [ $# -gt 0 ]; do
  case $1 in
    --manifest) [ $# -ge 2 ] || usage_die "--manifest needs a name"; MANIFEST=$2; shift 2 ;;
    --no-http-check) HTTP_CHECK=0; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) usage_die "unknown option $1" ;;
    *) [ -z "$SRC" ] || usage_die "more than one source given"; SRC=$1; shift ;;
  esac
done
[ -n "$SRC" ] || usage_die "a packs folder or .tar is required"
case $MANIFEST in packs.json|packs-gen9.json) ;; *) usage_die "--manifest must be packs.json or packs-gen9.json (got '$MANIFEST')" ;; esac
MBASE=${MANIFEST%.json}
[ -e "$SRC" ] || die "$SRC does not exist"

deploy_config
acquire_lock publish
need_cmd node tar cmp
[ "$HTTP_CHECK" = 0 ] || need_cmd curl

UPD="$GAGE_WEB_ROOT/update"
STAGE="$GAGE_WEB_ROOT/.staging"
KEY="$GAGE_KEY_DIR/update-signing-key.pem"
PUB="$GAGE_KEY_DIR/update-signing-pub.txt"
SIGN="$GAGE_SRC/tools/packs/sign_packs.mjs"
LOCAL_HTTP="${GAGE_LOCAL_HTTP:-http://127.0.0.1}"
CUR="$UPD/$MANIFEST"

[ -f "$SIGN" ] || die "missing $SIGN (unpack the 0.4.14+ source first)"
[ -f "$KEY" ] || die "signing key not found at $KEY"
[ "$(stat -c %a -- "$KEY")" = 600 ] || die "$KEY must have mode 600"
[ -f "$PUB" ] || die "public key not found at $PUB"

# ---- the source: a folder, or a .tar unpacked into a temp folder --------------------------------------------
ROOT=$SRC
if [ -f "$SRC" ]; then
  case $SRC in *.tar) ;; *) die "$SRC is a file but not a .tar" ;; esac
  tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/gage-packs.XXXXXX")
  defer "rm -rf -- $(printf '%q' "$tmpdir")"
  # GNU tar refuses members with '..' and strips a leading '/'; links are refused by sign_packs.mjs.
  tar -xf "$SRC" -C "$tmpdir" --no-same-owner
  ROOT=$tmpdir
  [ -d "$tmpdir/packs" ] && ROOT="$tmpdir/packs"
fi
[ -d "$ROOT" ] || die "$ROOT is not a folder"
log "packs from $ROOT: $(find "$ROOT" -mindepth 1 -maxdepth 1 -type d ! -name '.*' | wc -l) folder(s)"

run mkdir -p "$UPD/packs" "$STAGE" "$GAGE_RELEASES/manifests"
NEW="$STAGE/$MANIFEST.$$"
defer "rm -f -- $(printf '%q' "$NEW") $(printf '%q' "$NEW.tmp")"

if [ "$DRY_RUN" = 1 ]; then
  node "$SIGN" --key "$KEY" --pub "$PUB" --root "$ROOT" --blobs "$UPD/packs" --out "$NEW" --dry-run
  log "[dry-run] mv -f $NEW $CUR   (atomic; the previous $MANIFEST is first copied to $GAGE_RELEASES/manifests/)"
  log "dry-run OK"
  exit 0
fi

# ---- 1 + 2: blobs, then the signed list ------------------------------------------------------------------------
node "$SIGN" --key "$KEY" --pub "$PUB" --root "$ROOT" --blobs "$UPD/packs" --out "$NEW"
chmod 755 "$UPD/packs"

# ---- 3: atomic switch ------------------------------------------------------------------------------------------
if [ -f "$CUR" ]; then
  cp -- "$CUR" "$GAGE_RELEASES/manifests/$MBASE-$(date -u +%Y%m%dT%H%M%SZ).json"
fi
mv -f -- "$NEW" "$CUR"
log "published $CUR"

# ---- 4: HTTP check ---------------------------------------------------------------------------------------------
if [ "$HTTP_CHECK" = 1 ]; then
  served=$(mktemp "${TMPDIR:-/tmp}/gage-served.XXXXXX")
  defer "rm -f -- $(printf '%q' "$served")"
  blob=$(find "$UPD/packs" -maxdepth 1 -type f -regextype posix-extended -regex '.*/[0-9a-f]{64}' -print -quit)
  bases=("$LOCAL_HTTP")
  [ -z "$DOMAIN" ] || bases+=("https://$DOMAIN")
  for base in "${bases[@]}"; do
    curl -fsS --max-time 15 -o "$served" -- "$base/update/$MANIFEST" || die "published, but $base/update/$MANIFEST does not answer"
    cmp -s "$served" "$CUR" || die "published, but $base serves different bytes than $CUR"
    if [ -n "$blob" ]; then
      curl -fsSI --max-time 15 -o /dev/null -- "$base/update/packs/$(basename "$blob")" || die "published, but $base does not serve $(basename "$blob")"
    fi
    log "HTTP check ok: $base serves $MANIFEST and the files"
  done
fi
log "done. Launchers pick $MANIFEST up on their next PLAY ($([ "$MANIFEST" = packs.json ] && echo "GTA V Legacy" || echo "GTA V Enhanced at pack_enh>=2"))."
