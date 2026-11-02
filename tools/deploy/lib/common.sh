#!/usr/bin/env bash
# Shared helpers for the tools/deploy scripts. Source it, do not run it:
#
#   SCRIPT_NAME=publish-client
#   . "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"
#   deploy_config
#
# Defines logging, a dry-run aware `run`, an EXIT-time `defer`, a mkdir lock, a tolerant reader for
# /etc/gage/deploy.env and the version helpers. The scripts set `set -euo pipefail` themselves.

DEPLOY_LIB_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DEPLOY_DIR=$(cd "$DEPLOY_LIB_DIR/.." && pwd)
DEPLOY_REPO_ROOT=$(cd "$DEPLOY_DIR/../.." && pwd)
: "${SCRIPT_NAME:=deploy}"
DRY_RUN=${DRY_RUN:-0}

log()  { printf '[%s] %s\n' "$SCRIPT_NAME" "$*"; }
warn() { printf '[%s] WARN: %s\n' "$SCRIPT_NAME" "$*" >&2; }
die()  { printf '[%s] ERROR: %s\n' "$SCRIPT_NAME" "$1" >&2; exit "${2:-1}"; }
usage_die() { printf '[%s] ERROR: %s (try --help)\n' "$SCRIPT_NAME" "$1" >&2; exit 2; }
is_uint() { [[ ${1:-} =~ ^[0-9]+$ ]]; }

# run CMD ARGS...: executes, or in dry-run mode prints the command quoted and does nothing.
run() {
  if [ "$DRY_RUN" = 1 ]; then
    printf '[%s] [dry-run]' "$SCRIPT_NAME"; printf ' %q' "$@"; printf '\n'
  else
    "$@"
  fi
}

# need_cmd NAME...: every command must exist, except in dry-run where a missing one is only reported.
need_cmd() {
  local c missing=()
  for c in "$@"; do command -v "$c" >/dev/null 2>&1 || missing+=("$c"); done
  [ ${#missing[@]} -eq 0 ] && return 0
  if [ "$DRY_RUN" = 1 ]; then
    warn "dry-run: not installed on this machine, a real run needs: ${missing[*]}"
    return 0
  fi
  die "missing commands: ${missing[*]}"
}

# ---- EXIT-time cleanup -----------------------------------------------------------------
_DEFERRED=()
# defer 'command string': run in reverse order when the script exits (success or failure).
defer() { _DEFERRED+=("$1"); }
_run_deferred() {
  local i rc=$?
  for (( i=${#_DEFERRED[@]}-1; i>=0; i-- )); do eval "${_DEFERRED[$i]}" || true; done
  return $rc
}
trap _run_deferred EXIT

# acquire_lock NAME: one run at a time per NAME (mkdir is atomic; a lock whose pid is gone is stale).
# No lock in dry-run: a dry run changes nothing.
acquire_lock() {
  [ "$DRY_RUN" = 1 ] && return 0
  local dir="${GAGE_LOCK_DIR:-${TMPDIR:-/tmp}}/gage-$1.lock" pid
  if ! mkdir "$dir" 2>/dev/null; then
    pid=$(cat "$dir/pid" 2>/dev/null || true)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      die "another '$1' run is active (pid $pid, lock $dir)" 75
    fi
    warn "removing stale lock $dir (pid '${pid:-?}' is gone)"
    rm -rf -- "$dir"
    mkdir "$dir" 2>/dev/null || die "cannot take lock $dir"
  fi
  printf '%s\n' "$$" > "$dir/pid"
  defer "rm -rf -- $(printf '%q' "$dir")"
}

# ---- configuration ---------------------------------------------------------------------
# load_env_file FILE: KEY=value lines (no export, no shell syntax; the same file feeds systemd's
# EnvironmentFile=). A variable that is already set in the environment wins over the file.
load_env_file() {
  local f=$1 line k v
  [ -f "$f" ] || return 0
  while IFS= read -r line || [ -n "$line" ]; do
    line=${line%$'\r'}
    case $line in ''|'#'*|';'*) continue ;; esac
    k=${line%%=*}; v=${line#*=}
    [[ $k =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
    case $v in \"*\") v=${v#\"}; v=${v%\"} ;; \'*\') v=${v#\'}; v=${v%\'} ;; esac
    if [ -z "${!k+x}" ]; then export "$k=$v"; fi
  done < "$f"
}

# deploy_config: defaults, then /etc/gage/deploy.env, then the caller's environment wins.
deploy_config() {
  : "${GAGE_OPT:=/opt/gage}"
  : "${GAGE_ENV_FILE:=/etc/gage/deploy.env}"
  load_env_file "$GAGE_ENV_FILE"
  : "${PUBLIC_IP:=5.175.253.187}"
  # DOMAIN: the public host name of Solar RP's update server. DNS A records for the apex AND www point at PUBLIC_IP.
  # Set DOMAIN= (empty) in /etc/gage/deploy.env to run IP-only over plain HTTP instead.
  [ -n "${DOMAIN+x}" ] || DOMAIN=solar-rp.com
  : "${GAGE_SRC:=$DEPLOY_REPO_ROOT}"
  : "${GAGE_BUILD_ROOT:=$GAGE_SRC/build}"
  : "${GAGE_WEB_ROOT:=/var/www/gage}"
  : "${GAGE_RELEASES:=$GAGE_OPT/releases}"
  : "${GAGE_SERVER_DIR:=$GAGE_OPT/server}"
  : "${GAGE_KEY_DIR:=/root/.gage-keys}"
  : "${GAGE_WINEPREFIX:=$GAGE_OPT/wine}"
  # URL prefix of the installer inside a signed manifest, and the manifest URL baked into new launchers (0.4.0 on:
  # https://solar-rp.com/update/version.json). The bare-IP plain-HTTP copy of /update/ stays served as a fallback.
  if [ -n "$DOMAIN" ]; then
    : "${UPDATE_BASE_URL:=https://$DOMAIN/update}"
    : "${BAKED_MANIFEST_URL:=https://$DOMAIN/update/version.json}"
  else
    : "${UPDATE_BASE_URL:=http://$PUBLIC_IP/update}"
    : "${BAKED_MANIFEST_URL:=http://$PUBLIC_IP/update/version.json}"
  fi
  # Signed `updateUrl` that moves clients to a different manifest URL. Optional and normally empty: only launchers
  # that baked another URL (the bare IP) would ever need it.
  : "${SIGNED_UPDATE_URL:=}"
  # Player-facing file names (the rename to Solar RP). The old installer name is accepted only to find, archive and
  # roll back releases that were published before the rename.
  : "${CLIENT_EXE:=SolarLauncher.exe}"
  : "${INSTALLER_PREFIX:=SolarRP-Setup}"
  LEGACY_INSTALLER_PREFIX=GAGE-MP-Setup
  [[ $PUBLIC_IP =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] || die "PUBLIC_IP '$PUBLIC_IP' is not an IPv4 address"
  [ -z "$DOMAIN" ] || [[ $DOMAIN =~ ^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$ ]] || die "DOMAIN '$DOMAIN' is not a host name"
  [[ $INSTALLER_PREFIX =~ ^[A-Za-z0-9._-]+$ ]] || die "INSTALLER_PREFIX '$INSTALLER_PREFIX' has characters that do not belong in a file name"
  [[ $CLIENT_EXE =~ ^[A-Za-z0-9._-]+\.exe$ ]] || die "CLIENT_EXE '$CLIENT_EXE' is not an .exe file name"
  UPDATE_BASE_URL=${UPDATE_BASE_URL%/}
}

# installer_version_of FILENAME: prints x.y.z when FILENAME is an installer of this project, under the current or the
# pre-rename name (SolarRP-Setup-<v>.exe, GAGE-MP-Setup-<v>.exe); fails otherwise.
installer_version_of() {
  local f=$1
  if [[ $f =~ ^($INSTALLER_PREFIX|$LEGACY_INSTALLER_PREFIX)-([0-9]+\.[0-9]+\.[0-9]+)\.exe$ ]]; then
    printf '%s' "${BASH_REMATCH[2]}"
    return 0
  fi
  return 1
}

# ---- helpers ---------------------------------------------------------------------------
sha256_of() {
  local out
  out=$(sha256sum -- "$1") || die "sha256sum failed for $1"
  printf '%s' "${out%% *}"
}
file_size() {
  local n
  n=$(stat -c %s -- "$1") || die "cannot stat $1"
  printf '%s' "$n"
}

# client_version: kClientVersion from apps/launcher/gage_gui.cpp, read the way installer/build-installer.sh does.
client_version() {
  local f="$GAGE_SRC/apps/launcher/gage_gui.cpp" v
  [ -f "$f" ] || die "missing $f"
  v=$(sed -n 's/.*kClientVersion = L"\([^"]*\)".*/\1/p' "$f")
  [[ $v =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "no usable kClientVersion in $f (got '$v')"
  printf '%s' "$v"
}

# ver_cmp A B: prints -1, 0 or 1 comparing two x.y.z versions numerically.
ver_cmp() {
  local IFS=. i
  local -a a=($1) b=($2)
  for i in 0 1 2; do
    if (( 10#${a[i]:-0} > 10#${b[i]:-0} )); then echo 1; return; fi
    if (( 10#${a[i]:-0} < 10#${b[i]:-0} )); then echo -1; return; fi
  done
  echo 0
}

# find_tool NAME...: first of the names that is on PATH (prints it), else fails.
find_tool() {
  local n
  for n in "$@"; do
    if command -v "$n" >/dev/null 2>&1; then printf '%s' "$n"; return 0; fi
  done
  return 1
}

ops_js() { node "$DEPLOY_LIB_DIR/ops.mjs" "$@"; }

# ---- the game server's port, player count and admin-channel token -----------------------------------------
# Shared by deploy-server.sh and release.sh. The caller sets CONF (the live server.json) and ENVF
# (/etc/gage/server.env) before calling any of these.

# game_port: the UDP game port from $CONF (default 24800).
game_port() {
  local p=24800
  [ -f "$CONF" ] && p=$(ops_js json-get "$CONF" port 24800)
  is_uint "$p" || die "$CONF: port '$p' is not a number"
  printf '%s' "$p"
}

# players_online: prints N when the UDP query answers, "?" when the server does not answer.
players_online() {
  local out
  if out=$(ops_js udp-query 127.0.0.1 "$(game_port)" --timeout-ms 1500 --attempts 3 2>/dev/null); then
    local n=${out#*\"players\":}; n=${n%%,*}
    printf '%s' "$n"
  else
    printf '?'
  fi
}

# admin_token: prints the admin-channel token (or nothing); never logs it. Environment first, then server.env,
# then server.json.
admin_token() {
  local t=${GAGE_ADMIN_TOKEN:-}
  if [ -z "$t" ] && [ -r "$ENVF" ]; then t=$(sed -n 's/^GAGE_ADMIN_TOKEN=//p' "$ENVF" | head -n 1 | tr -d '\r"'"'"); fi
  if [ -z "$t" ] && [ -f "$CONF" ]; then t=$(ops_js json-get "$CONF" adminToken "" || true); fi
  printf '%s' "$t"
}
