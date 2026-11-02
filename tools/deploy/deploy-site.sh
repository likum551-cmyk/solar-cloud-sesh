#!/usr/bin/env bash
# Deploy the solar-rp.com website (apps/website): build it from the unpacked source, check the build, copy it into a
# new release folder and switch the live symlink to it in one rename. Run on the VPS as root, after
# tools/deploy/unpack-source.sh.
#
#   deploy-site.sh [--skip-build] [--rollback] [--keep N] [--no-http-check] [--dry-run]
#
#   --skip-build     use apps/website/dist as it is (it must exist); default: npm ci, npm test, npm run build there.
#   --rollback       switch back to the newest release older than the live one (no build, no copy).
#   --keep N         release folders to keep, the live one included (default 3, minimum 2 so a rollback target exists).
#   --no-http-check  skip the check through Caddy after the switch.
#   --dry-run        run every check, print every change, change nothing. Exit 1 if a real run would stop.
#
# Layout: releases in /var/www/gage/site-releases/<UTC stamp>/, the live site is the symlink /var/www/gage/site
# (Caddy's root for every path except /update/* and /healthz, see tools/deploy/Caddyfile). The switch is
# `ln -s` to a temporary name and `mv -T` over the link, so a request sees either the old tree or the new one.
#
# Order of effects (a failure at any step leaves the previous site serving):
#   1 build (npm ci, tests, build)      2 check dist: index.html, every /assets /media /shots file it names
#   3 copy into site-releases/<stamp>   4 switch the symlink              5 HTTP check through Caddy
#   6 keep the newest N releases
# Exit codes: 0 ok, 1 failure, 2 usage, 75 another deploy-site is running.
set -euo pipefail

SCRIPT_NAME=deploy-site
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }

SKIP_BUILD=0
ROLLBACK=0
KEEP=3
HTTP_CHECK=1
while [ $# -gt 0 ]; do
  case $1 in
    --skip-build) SKIP_BUILD=1; shift ;;
    --rollback) ROLLBACK=1; shift ;;
    --keep) [ $# -ge 2 ] || usage_die "--keep needs a number"; KEEP=$2; shift 2 ;;
    --no-http-check) HTTP_CHECK=0; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage_die "unknown option $1" ;;
  esac
done
is_uint "$KEEP" && [ "$KEEP" -ge 2 ] || usage_die "--keep must be a number >= 2"
[ "$ROLLBACK" = 0 ] || [ "$SKIP_BUILD" = 0 ] || usage_die "--rollback builds nothing; drop --skip-build"

deploy_config
acquire_lock deploy-site

FAILED=0
# fail_check MESSAGE: stops a real run; in a dry run it is counted and the run goes on.
fail_check() {
  if [ "$DRY_RUN" = 1 ]; then warn "would stop: $1"; FAILED=1; else die "$1"; fi
}

SITE_SRC=$GAGE_SRC/apps/website
DIST=$SITE_SRC/dist
LIVE=$GAGE_WEB_ROOT/site
RELEASES=$GAGE_WEB_ROOT/site-releases
# the phrase the HTTP check looks for: a request that answers 200 with any other page has not served this site
PHRASE='Los Santos'

live_target() { [ -L "$LIVE" ] && readlink -f -- "$LIVE" || true; }
# the live release's NAME: compare names, never paths (readlink -f canonicalises, $RELEASES may not be canonical)
live_name() { local t; t=$(live_target); [ -n "$t" ] && basename -- "$t" || true; }
# releases, oldest first (the names are UTC stamps, so a name sort is a time sort)
list_releases() { [ -d "$RELEASES" ] && find "$RELEASES" -mindepth 1 -maxdepth 1 -type d ! -name '.*' -printf '%f\n' | sort || true; }

switch_to() {
  local target=$1 tmp="$GAGE_WEB_ROOT/.site.new.$$"
  run ln -sfn -- "$target" "$tmp"
  run mv -Tf -- "$tmp" "$LIVE"
  log "live site -> $target"
}

http_check() {
  [ "$HTTP_CHECK" = 1 ] || { log "HTTP check skipped (--no-http-check)"; return 0; }
  [ "$DRY_RUN" = 0 ] || { log "[dry-run] would fetch the site through Caddy and look for '$PHRASE'"; return 0; }
  local url body asset
  if [ -n "$DOMAIN" ]; then
    url="https://$DOMAIN/"
    body=$(curl -fsS --max-time 15 --resolve "$DOMAIN:443:127.0.0.1" "$url") || die "GET $url through 127.0.0.1 failed (is Caddy up and the certificate issued? journalctl -u caddy)"
  else
    url="http://127.0.0.1/"
    body=$(curl -fsS --max-time 15 "$url") || die "GET $url failed (journalctl -u caddy)"
  fi
  grep -q -- "$PHRASE" <<<"$body" || die "GET $url answered, but without '$PHRASE': Caddy is not serving $LIVE (check the Caddyfile handle block)"
  # one built asset too: the page loads nothing if /assets/ is not served
  asset=$(grep -o -E '/assets/[A-Za-z0-9._-]+\.js' <<<"$body" | head -n 1 || true)
  [ -n "$asset" ] || die "the served index.html names no /assets/*.js"
  if [ -n "$DOMAIN" ]; then
    curl -fsS --max-time 15 -o /dev/null --resolve "$DOMAIN:443:127.0.0.1" "https://$DOMAIN$asset" || die "GET $asset failed"
  else
    curl -fsS --max-time 15 -o /dev/null "http://127.0.0.1$asset" || die "GET $asset failed"
  fi
  log "HTTP check ok: $url has '$PHRASE', $asset loads"
}

# ---- rollback ---------------------------------------------------------------------------------------------------
if [ "$ROLLBACK" = 1 ]; then
  cur=$(live_name)
  [ -n "$cur" ] || die "no live site to roll back from ($LIVE is not a symlink)"
  prev=''; found=0
  while IFS= read -r r; do
    if [ "$r" = "$cur" ]; then found=1; break; fi
    prev=$r
  done < <(list_releases)
  [ "$found" = 1 ] || die "the live site points at $(live_target), which is not a release in $RELEASES; fix the symlink by hand"
  [ -n "$prev" ] || die "no release older than $cur in $RELEASES"
  log "rolling back: $cur -> $prev"
  switch_to "$RELEASES/$prev"
  http_check
  log "done (rollback). The newer release stays in $RELEASES until a deploy prunes it."
  exit 0
fi

# ---- 1 build ----------------------------------------------------------------------------------------------------
[ -f "$SITE_SRC/package.json" ] && [ -f "$SITE_SRC/package-lock.json" ] || die "$SITE_SRC is not the website source (no package.json / package-lock.json). Unpack the source first."
if [ "$SKIP_BUILD" = 0 ]; then
  need_cmd node npm
  if command -v node >/dev/null 2>&1; then
    nv=$(node -p 'process.versions.node')
    [ "$(ver_cmp "$nv" 22.12.0)" -ge 0 ] || fail_check "Node $nv is too old: apps/website needs >= 22.12.0 (bootstrap-vps.sh installs Node 22)"
  fi
  log "== 1/6 build ($SITE_SRC)"
  run bash -c 'cd "$1" && npm ci --no-audit --no-fund && npm test && npm run build' _ "$SITE_SRC" || die "website build failed"
else
  log "== 1/6 build skipped (--skip-build): using $DIST as it is"
fi

# ---- 2 check dist -----------------------------------------------------------------------------------------------
log "== 2/6 check $DIST"
if [ ! -f "$DIST/index.html" ]; then
  if [ "$DRY_RUN" = 1 ] && [ "$SKIP_BUILD" = 0 ]; then
    log "[dry-run] $DIST/index.html does not exist yet; the build would create it"
  else
    fail_check "$DIST/index.html is missing"
  fi
else
  grep -q -- "$PHRASE" "$DIST/index.html" || fail_check "$DIST/index.html does not contain '$PHRASE': not the Solar RP site"
  missing=0; refs=0
  while IFS= read -r ref; do
    refs=$((refs + 1))
    [ -f "$DIST$ref" ] || { warn "index.html names $ref, which is not in dist"; missing=$((missing + 1)); }
  done < <(grep -o -E '"/(assets|media|shots)/[^"?#]+"' "$DIST/index.html" | tr -d '"' | sort -u)
  [ "$refs" -gt 0 ] || fail_check "index.html names no /assets, /media or /shots file: the check would pass on nothing"
  [ "$missing" = 0 ] || fail_check "$missing file(s) named by index.html are missing from dist"
  # the release manifest is served by /update; a copy inside the site would shadow nothing but would mislead
  [ ! -e "$DIST/update" ] || fail_check "$DIST/update exists: the site must not carry an /update folder"
  [ -z "$(find "$DIST" -name '*.map' -print -quit)" ] || warn "dist contains source maps; they will be public"
  log "dist ok: $refs referenced files present, $(du -sh -- "$DIST" | cut -f1)"
fi

# ---- 3 copy into a new release ----------------------------------------------------------------------------------
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
NEW=$RELEASES/$STAMP
log "== 3/6 copy -> $NEW"
[ ! -e "$NEW" ] || fail_check "$NEW already exists (two deploys in one second?)"
run install -d -m 755 -- "$RELEASES"
run cp -a -- "$DIST" "$RELEASES/.incoming-$STAMP"
run chmod -R a+rX,go-w -- "$RELEASES/.incoming-$STAMP"
run mv -T -- "$RELEASES/.incoming-$STAMP" "$NEW"

if [ "$FAILED" = 1 ]; then
  die "dry run found problems (above); a real run would have stopped"
fi

# ---- 4 switch ---------------------------------------------------------------------------------------------------
log "== 4/6 switch"
if [ -e "$LIVE" ] && [ ! -L "$LIVE" ]; then
  die "$LIVE exists and is not a symlink; move it aside by hand first"
fi
PREV=$(live_target)
switch_to "$NEW"

# ---- 5 HTTP check -----------------------------------------------------------------------------------------------
log "== 5/6 HTTP check"
if ! ( http_check ); then
  if [ -n "$PREV" ] && [ "$DRY_RUN" = 0 ]; then
    warn "HTTP check failed: switching back to $(basename -- "$PREV")"
    switch_to "$PREV"
  fi
  die "the new site did not pass the HTTP check; $( [ -n "$PREV" ] && echo "the previous release is live again" || echo "there was no previous release")"
fi

# ---- 6 prune ----------------------------------------------------------------------------------------------------
log "== 6/6 keep the newest $KEEP releases"
mapfile -t all < <(list_releases)
drop=$(( ${#all[@]} - KEEP ))
live_now=$(live_name)
[ -n "$live_now" ] || [ "$DRY_RUN" = 1 ] || die "cannot tell the live release after the switch; pruning nothing"
for (( i=0; i<drop; i++ )); do
  [ "${all[$i]}" = "$live_now" ] && continue
  r=$RELEASES/${all[$i]}
  run rm -rf -- "$r"
  log "removed old release ${all[$i]}"
done
log "done: https://${DOMAIN:-$PUBLIC_IP}/ serves $STAMP"
