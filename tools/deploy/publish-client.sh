#!/usr/bin/env bash
# Publish a launcher update: copy the installer next to version.json, sign a manifest, verify it, then
# atomically switch version.json to it. Run on the VPS after tools/deploy/build-client.sh.
#
#   publish-client.sh <version> [--notes TEXT] [--mandatory] [--update-url URL | --no-update-url]
#                     [--installer FILE] [--rollback-from VERSION|FILE] [--no-http-check]
#                     [--skip-pubkey-check] [--force-key-mismatch] [--dry-run]
#
#   <version>        x.y.z. Must equal kClientVersion in apps/launcher/gage_gui.cpp (a normal release), or, with
#                    --rollback-from, any version strictly newer than the one published now.
#   --notes TEXT     release notes shown by the launcher (max 2000 characters).
#   --mandatory      the launcher forces the update. Refused together with --rollback-from.
#   --update-url U   signed `updateUrl`: moves clients to another manifest URL. OPTIONAL and normally unused: launchers from
#                    0.4.0 on already bake https://solar-rp.com/update/version.json. Only launchers that baked the bare-IP
#                    URL would need it. Default: SIGNED_UPDATE_URL from /etc/gage/deploy.env. --no-update-url: none.
#   --installer F    installer file; default <build>/win/installer/SolarRP-Setup-<version>.exe.
#   --rollback-from  re-publish an OLDER installer (a version number, looked up in the web root and in
#                    /opt/gage/releases/installers, or a file path) under the NEW <version>. See docs/OPS.md
#                    "Rollback": clients never accept a manifest that is not newer than what they run, so the
#                    old bits must go out under a higher number. Emergency measure; read the caveat first.
#   --dry-run        run every check, print every change, change nothing. Exit 1 if a real run would stop.
#
# Order of effects (a failure at any step leaves the previous version.json serving):
#   1 installer copied to /var/www/gage/update/ (atomic rename)   2 sign into /var/www/gage/.staging/
#   3 verify.mjs against the public key AND the installer           4 mv staging -> update/version.json (atomic)
#   5 local HTTP check                                              6 keep the newest 3 installers, archive the rest
# Never prints the private key. Exit codes: 0 ok, 1 failure, 2 usage, 75 another publish is running.
set -euo pipefail

SCRIPT_NAME=publish-client
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }

VERSION=''
NOTES=''
MANDATORY=0
UPDATE_URL_OPT=''        # '' = take SIGNED_UPDATE_URL from the config; value = explicit
NO_UPDATE_URL=0
INSTALLER_OPT=''
ROLLBACK_FROM=''
HTTP_CHECK=1
SKIP_PUBKEY_CHECK=0
FORCE_KEY_MISMATCH=0

while [ $# -gt 0 ]; do
  case $1 in
    --notes) [ $# -ge 2 ] || usage_die "--notes needs a value"; NOTES=$2; shift 2 ;;
    --mandatory) MANDATORY=1; shift ;;
    --update-url) [ $# -ge 2 ] || usage_die "--update-url needs a value"; UPDATE_URL_OPT=$2; shift 2 ;;
    --no-update-url) NO_UPDATE_URL=1; shift ;;
    --installer) [ $# -ge 2 ] || usage_die "--installer needs a value"; INSTALLER_OPT=$2; shift 2 ;;
    --rollback-from) [ $# -ge 2 ] || usage_die "--rollback-from needs a value"; ROLLBACK_FROM=$2; shift 2 ;;
    --no-http-check) HTTP_CHECK=0; shift ;;
    --skip-pubkey-check) SKIP_PUBKEY_CHECK=1; shift ;;
    --force-key-mismatch) FORCE_KEY_MISMATCH=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) usage_die "unknown option $1" ;;
    *) [ -z "$VERSION" ] || usage_die "more than one version given"; VERSION=$1; shift ;;
  esac
done
[ -n "$VERSION" ] || usage_die "version required (publish-client.sh 0.3.5)"
[[ $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || usage_die "version '$VERSION' must be x.y.z"
[ "$NO_UPDATE_URL" = 0 ] || [ -z "$UPDATE_URL_OPT" ] || usage_die "--update-url and --no-update-url together"
[ ${#NOTES} -le 2000 ] || usage_die "notes are ${#NOTES} characters, the limit is 2000"
if [ -n "$ROLLBACK_FROM" ] && [ "$MANDATORY" = 1 ]; then
  usage_die "--mandatory with --rollback-from would force every client onto a rolled-back build; refused"
fi

deploy_config
acquire_lock publish
PROBLEMS=0
# fail_check MESSAGE: stops a real run; in a dry run it is counted and the run goes on.
fail_check() {
  if [ "$DRY_RUN" = 1 ]; then warn "dry-run: a real run would stop here: $1"; PROBLEMS=$((PROBLEMS + 1))
  else die "$1"; fi
}

UPD="$GAGE_WEB_ROOT/update"
STAGE="$GAGE_WEB_ROOT/.staging"
KEY="$GAGE_KEY_DIR/update-signing-key.pem"
PUB="$GAGE_KEY_DIR/update-signing-pub.txt"
SIGN="$GAGE_SRC/tools/update-sign/sign.mjs"
VERIFY="$GAGE_SRC/tools/update-sign/verify.mjs"
LOCAL_HTTP="${GAGE_LOCAL_HTTP:-http://127.0.0.1}"
DEST_NAME="$INSTALLER_PREFIX-$VERSION.exe"
INSTALLER_URL="$UPDATE_BASE_URL/$DEST_NAME"

need_cmd node sha256sum stat cmp sort
[ "$HTTP_CHECK" = 0 ] || need_cmd curl
[ -f "$SIGN" ] || fail_check "missing $SIGN"
[ -f "$VERIFY" ] || fail_check "missing $VERIFY"

# ---- the signing key: present, private, never printed ------------------------------------
if [ ! -f "$KEY" ]; then
  fail_check "signing key not found at $KEY (GI copies update-signing-key.pem there, chmod 600)"
elif [ "$(uname -s)" = Linux ]; then
  # file modes mean nothing on NTFS (Git Bash), so the check is Linux-only
  key_mode=$(stat -c %a -- "$KEY")
  [ "$key_mode" = 600 ] || fail_check "$KEY has mode $key_mode, must be 600"
fi
[ -f "$PUB" ] || fail_check "public key not found at $PUB (update-signing-pub.txt next to the private key)"

# ---- which installer, and is it the right one --------------------------------------------
SRC_INSTALLER=''
if [ -n "$ROLLBACK_FROM" ]; then
  if [ -f "$ROLLBACK_FROM" ]; then
    SRC_INSTALLER=$ROLLBACK_FROM
  elif [[ $ROLLBACK_FROM =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    # current name first, then the pre-rename one (releases archived before the Solar RP rename)
    for d in "$UPD" "$GAGE_RELEASES/installers"; do
      for p in "$INSTALLER_PREFIX" "$LEGACY_INSTALLER_PREFIX"; do
        if [ -f "$d/$p-$ROLLBACK_FROM.exe" ]; then SRC_INSTALLER="$d/$p-$ROLLBACK_FROM.exe"; break 2; fi
      done
    done
    [ -n "$SRC_INSTALLER" ] || fail_check "no installer for $ROLLBACK_FROM in $UPD or $GAGE_RELEASES/installers"
  else
    fail_check "--rollback-from '$ROLLBACK_FROM' is neither a version nor a file"
  fi
  [ -z "$INSTALLER_OPT" ] || usage_die "--installer and --rollback-from together"
else
  built=$(client_version)
  [ "$built" = "$VERSION" ] || fail_check "kClientVersion in apps/launcher/gage_gui.cpp is $built, not $VERSION. Bump it, rebuild with build-client.sh, then publish."
  SRC_INSTALLER=${INSTALLER_OPT:-$GAGE_BUILD_ROOT/win/installer/$DEST_NAME}
  [ -f "$SRC_INSTALLER" ] || fail_check "installer not found: $SRC_INSTALLER (run tools/deploy/build-client.sh first)"
fi
SRC_SHA=''; SRC_SIZE=0
if [ -n "$SRC_INSTALLER" ] && [ -f "$SRC_INSTALLER" ]; then
  SRC_SIZE=$(file_size "$SRC_INSTALLER")
  [ "$SRC_SIZE" -gt 1048576 ] || fail_check "$SRC_INSTALLER is only $SRC_SIZE bytes, not an installer"
  [ "$(head -c 2 -- "$SRC_INSTALLER")" = "MZ" ] || fail_check "$SRC_INSTALLER does not start with MZ, not a Windows executable"
  SRC_SHA=$(sha256_of "$SRC_INSTALLER")
fi

# ---- the signed updateUrl ----------------------------------------------------------------
SIGNED_URL=''
if [ "$NO_UPDATE_URL" = 0 ]; then SIGNED_URL=${UPDATE_URL_OPT:-$SIGNED_UPDATE_URL}; fi
if [ -n "$SIGNED_URL" ]; then
  [[ $SIGNED_URL =~ ^https?://[^[:space:]/?#]+/.*/version\.json$ ]] || fail_check "update URL '$SIGNED_URL' must look like https://<host>/update/version.json"
  [ ${#SIGNED_URL} -le 256 ] || fail_check "update URL is ${#SIGNED_URL} characters, the launcher accepts at most 256"
fi

# ---- what is published now, and the anti-downgrade rule ----------------------------------
CUR_MANIFEST="$UPD/version.json"
CUR_VERSION=''
if [ -f "$CUR_MANIFEST" ]; then
  if [ -f "$PUB" ] && ! node "$VERIFY" --pub "$PUB" --manifest "$CUR_MANIFEST" >/dev/null 2>&1; then
    if [ "$FORCE_KEY_MISMATCH" = 1 ]; then warn "the published manifest does NOT verify against $PUB; continuing because of --force-key-mismatch"
    else fail_check "the published $CUR_MANIFEST does not verify against $PUB: a different key signed it. Every shipped launcher trusts one key; refusing (--force-key-mismatch to override)."; fi
  fi
  CUR_VERSION=$(ops_js manifest-info "$CUR_MANIFEST" version) || fail_check "cannot read the version of the published manifest"
  if [ -n "$CUR_VERSION" ]; then
    cmp_result=$(ver_cmp "$VERSION" "$CUR_VERSION")
    if [ "$cmp_result" = -1 ]; then
      fail_check "$VERSION is older than the published $CUR_VERSION; clients only accept a strictly newer version. To go back, publish the old installer under a NEWER number: --rollback-from."
    elif [ "$cmp_result" = 0 ]; then
      cur_sha=$(ops_js manifest-info "$CUR_MANIFEST" sha256) || cur_sha=''
      [ "$cur_sha" = "$SRC_SHA" ] || fail_check "$VERSION is already published with different bytes (sha256 $cur_sha); bump the version instead"
      log "$VERSION is already published with the same installer: re-signing (notes / updateUrl change only)"
    fi
  fi
else
  log "no manifest published yet at $CUR_MANIFEST (first release)"
fi
if [ -n "$ROLLBACK_FROM" ]; then
  [ -n "$CUR_VERSION" ] || warn "rollback requested but nothing is published yet"
  old_v=$(installer_version_of "$(basename -- "$SRC_INSTALLER")" || true)
  if [ -n "$old_v" ] && [ "$(ver_cmp "$VERSION" "$old_v")" != 1 ]; then
    fail_check "rollback version $VERSION must be newer than the installer's own version $old_v"
  fi
fi

# ---- the baked public key must be the key we sign with -----------------------------------
if [ -z "$ROLLBACK_FROM" ] && [ "$SKIP_PUBKEY_CHECK" = 0 ]; then
  cache="$GAGE_BUILD_ROOT/win/CMakeCache.txt"
  if [ ! -f "$cache" ]; then
    fail_check "cannot confirm which public key the installer was built with: $cache is missing (--skip-pubkey-check if the installer came from elsewhere)"
  elif [ -f "$PUB" ]; then
    baked=$(sed -n 's/^GAGE_UPDATE_PUBKEY:[A-Z]*=//p' "$cache" | head -n 1)
    want=$(tr -d '[:space:]' < "$PUB")
    [ -n "$baked" ] || fail_check "$cache has no GAGE_UPDATE_PUBKEY: the launcher in this installer cannot verify updates"
    [ -z "$baked" ] || [ "$baked" = "$want" ] || fail_check "the launcher was built with a different public key than $PUB: clients would reject this update"
  fi
fi

if [ "$DRY_RUN" = 1 ]; then
  log "dry-run summary: version $VERSION, installer ${SRC_INSTALLER:-?} (${SRC_SIZE} bytes), url $INSTALLER_URL, published now '${CUR_VERSION:-none}'"
  log "dry-run: mandatory=$MANDATORY updateUrl='${SIGNED_URL:-}' rollback-from='${ROLLBACK_FROM:-}' notes=${#NOTES} chars"
fi

run mkdir -p "$UPD" "$STAGE" "$GAGE_RELEASES/manifests" "$GAGE_RELEASES/installers"

# ---- 1. installer first, so no manifest ever names a file that is not there ---------------
DEST="$UPD/$DEST_NAME"
if [ -f "$DEST" ] && [ -n "$SRC_SHA" ]; then
  [ "$(sha256_of "$DEST")" = "$SRC_SHA" ] || fail_check "$DEST exists with different bytes; clients may have cached it. Bump the version (or remove it by hand if nobody has seen it)."
  log "installer already in place: $DEST"
elif [ -n "$SRC_SHA" ]; then
  log "copying the installer to $DEST"
  run cp -- "$SRC_INSTALLER" "$STAGE/$DEST_NAME.part"
  run chmod 644 "$STAGE/$DEST_NAME.part"
  run mv -f -- "$STAGE/$DEST_NAME.part" "$DEST"
fi

# ---- 1b. a signed updateUrl must already answer, or clients would be moved to a dead URL ---
if [ -n "$SIGNED_URL" ] && [ "$HTTP_CHECK" = 1 ]; then
  probe_url="${SIGNED_URL%/version.json}/$DEST_NAME"
  log "pre-flight: the new origin must already serve the installer: $probe_url"
  if [ "$DRY_RUN" = 1 ]; then
    log "[dry-run] curl -fsSI --max-time 20 $probe_url (expects content-length $SRC_SIZE)"
  else
    hdrs=$(curl -fsSI --max-time 20 -- "$probe_url") || die "cannot reach $probe_url. NOT publishing a manifest that moves clients to it. Check DNS, Caddy and the certificate."
    got_len=$(printf '%s' "$hdrs" | tr -d '\r' | sed -n 's/^[Cc]ontent-[Ll]ength: *//p' | tail -n 1)
    [ "$got_len" = "$SRC_SIZE" ] || die "$probe_url answered with content-length '$got_len', expected $SRC_SIZE. NOT publishing."
  fi
fi

# ---- 2. sign into staging, 3. verify -----------------------------------------------------
NEW="$STAGE/version.json.$$"
sign_args=(--key "$KEY" --installer "$DEST" --version "$VERSION" --url "$INSTALLER_URL" --out "$NEW")
[ -z "$NOTES" ] || sign_args+=(--notes "$NOTES")
[ "$MANDATORY" = 0 ] || sign_args+=(--mandatory)
[ -z "$SIGNED_URL" ] || sign_args+=(--update-url "$SIGNED_URL")
defer "rm -f -- $(printf '%q' "$NEW") $(printf '%q' "$NEW.tmp")"
if [ "$DRY_RUN" = 1 ]; then
  # show the shape only: the key path is not printed
  log "[dry-run] node $SIGN --key <private key> --installer $DEST --version $VERSION --url $INSTALLER_URL --out $NEW (notes ${#NOTES} chars, mandatory $MANDATORY, update-url '${SIGNED_URL:-}')"
  log "[dry-run] node $VERIFY --pub $PUB --manifest $NEW --installer $DEST"
  log "[dry-run] mv -f $NEW $CUR_MANIFEST   (atomic; the previous manifest is first copied to $GAGE_RELEASES/manifests/)"
  log "[dry-run] keep the newest 3 installers in $UPD, move the rest to $GAGE_RELEASES/installers/"
  [ "$PROBLEMS" -eq 0 ] || die "dry-run found $PROBLEMS problem(s) a real run would stop on"
  log "dry-run OK: a real run would publish $VERSION"
  exit 0
fi
node "$SIGN" "${sign_args[@]}"
node "$VERIFY" --pub "$PUB" --manifest "$NEW" --installer "$DEST"
# belt and braces: the signed payload says what we meant to publish
[ "$(ops_js manifest-info "$NEW" version)" = "$VERSION" ] || die "signed version differs from $VERSION"
[ "$(ops_js manifest-info "$NEW" url)" = "$INSTALLER_URL" ] || die "signed url differs from $INSTALLER_URL"
[ "$(ops_js manifest-info "$NEW" sha256)" = "$(sha256_of "$DEST")" ] || die "signed sha256 differs from the installer on disk"
[ "$(ops_js manifest-info "$NEW" updateUrl)" = "$SIGNED_URL" ] || die "signed updateUrl differs from '$SIGNED_URL'"
chmod 644 "$NEW"

# ---- 4. atomic switch --------------------------------------------------------------------
if [ -f "$CUR_MANIFEST" ]; then
  cp -- "$CUR_MANIFEST" "$GAGE_RELEASES/manifests/version-${CUR_VERSION:-unknown}-$(date -u +%Y%m%dT%H%M%SZ).json"
fi
mv -f -- "$NEW" "$CUR_MANIFEST"
log "published $VERSION: $CUR_MANIFEST"

# ---- 5. HTTP checks: through Caddy on loopback, and (domain mode) through the public HTTPS name -------------
# The public check goes out and back in over this machine's own address, so it cannot prove the Hetzner Cloud
# Firewall is open. It does prove DNS, a valid certificate (which Let's Encrypt only issues if 80/443 are reachable
# from the internet) and that Caddy serves the bytes just published.
if [ "$HTTP_CHECK" = 1 ]; then
  served=$(mktemp "${TMPDIR:-/tmp}/gage-served.XXXXXX")
  defer "rm -f -- $(printf '%q' "$served")"
  bases=("$LOCAL_HTTP")
  [ -z "$DOMAIN" ] || bases+=("https://$DOMAIN")
  for base in "${bases[@]}"; do
    curl -fsS --max-time 15 -o "$served" -- "$base/update/version.json" \
      || die "published, but $base/update/version.json does not answer (Caddy: systemctl status caddy, journalctl -u caddy -n 50; for https also DNS, the certificate and ports 80/443 in ufw AND the Hetzner Cloud Firewall)"
    cmp -s "$served" "$CUR_MANIFEST" || die "published, but $base serves different bytes than $CUR_MANIFEST"
    hdrs=$(curl -fsSI --max-time 15 -- "$base/update/$DEST_NAME") || die "published, but $base/update/$DEST_NAME does not answer"
    got_len=$(printf '%s' "$hdrs" | tr -d '\r' | sed -n 's/^[Cc]ontent-[Ll]ength: *//p' | tail -n 1)
    [ "$got_len" = "$(file_size "$DEST")" ] || die "installer served by $base with content-length '$got_len', file is $(file_size "$DEST")"
    log "HTTP check ok: manifest and installer are served by $base"
  done
fi

# ---- 6. retention: the newest 3 versions stay, older ones are archived ---------------------
# Both naming families count (SolarRP-Setup-<v>.exe and the pre-rename GAGE-MP-Setup-<v>.exe); archived files keep their own name.
mapfile -t all < <(find "$UPD" -maxdepth 1 -type f \( -name "$INSTALLER_PREFIX-*.exe" -o -name "$LEGACY_INSTALLER_PREFIX-*.exe" \) -printf '%f\n')
versions=()
for f in "${all[@]}"; do
  v=$(installer_version_of "$f" || true)
  if [ -n "$v" ]; then versions+=("$v"); fi
done
[ ${#versions[@]} -gt 0 ] || die "retention: no installer in $UPD after publishing (this must not happen)"
mapfile -t keep < <(printf '%s\n' "${versions[@]}" | sort -V -u -r | head -n 3)
for f in "${all[@]}"; do
  v=$(installer_version_of "$f" || true)
  [ -n "$v" ] || continue
  [ "$f" != "$DEST_NAME" ] || continue
  in_keep=0
  for k in "${keep[@]}"; do if [ "$k" = "$v" ]; then in_keep=1; fi; done
  [ "$in_keep" = 0 ] || continue
  if [ -e "$GAGE_RELEASES/installers/$f" ] && [ "$(sha256_of "$GAGE_RELEASES/installers/$f")" != "$(sha256_of "$UPD/$f")" ]; then
    mv -- "$UPD/$f" "$GAGE_RELEASES/installers/$f.$(sha256_of "$UPD/$f" | cut -c1-12)"
  else
    mv -f -- "$UPD/$f" "$GAGE_RELEASES/installers/$f"
  fi
  log "archived $f to $GAGE_RELEASES/installers/"
done
action=release
[ -z "$ROLLBACK_FROM" ] || action="rollback-from-$ROLLBACK_FROM"
printf '%s\t%s\t%s\t%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$VERSION" "$(sha256_of "$DEST")" "$(file_size "$DEST")" "$action" \
  >> "$GAGE_RELEASES/index.tsv"

log "done: $VERSION ($(file_size "$DEST") bytes, sha256 $(sha256_of "$DEST"))"
log "installer $INSTALLER_URL"
log "manifest  $UPDATE_BASE_URL/version.json${SIGNED_URL:+  (signed updateUrl: $SIGNED_URL)}"
if [ -n "$ROLLBACK_FROM" ]; then
  warn "rollback caveat: this installer installs the OLD launcher, whose kClientVersion is lower than $VERSION, so it will offer $VERSION again. Not mandatory, so harmless, but ship a properly rebuilt higher version soon (docs/OPS.md, Rollback)."
fi
