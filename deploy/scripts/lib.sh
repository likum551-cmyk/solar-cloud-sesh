# shellcheck shell=bash
# Shared helpers for the GAGE deploy scripts. Source it from a script that has already run
# `set -euo pipefail`; it is not meant to be executed.
#
# Conventions: every script sets SCRIPT_NAME before sourcing, keeps DRY_RUN=0|1, and wraps each
# mutating command in `run` so --dry-run prints instead of executing.

if [ -n "${GAGE_LIB_LOADED:-}" ]; then
  return 0
fi
GAGE_LIB_LOADED=1

DRY_RUN=${DRY_RUN:-0}

_ts() { date -u +%Y-%m-%dT%H:%M:%SZ; }
log() { printf '%s [%s] %s\n' "$(_ts)" "${SCRIPT_NAME:-gage}" "$*"; }
warn() { printf '%s [%s] WARN: %s\n' "$(_ts)" "${SCRIPT_NAME:-gage}" "$*" >&2; }
die() {
  printf '%s [%s] ERROR: %s\n' "$(_ts)" "${SCRIPT_NAME:-gage}" "$*" >&2
  exit 1
}
usage_die() {
  printf '%s [%s] ERROR: %s\n' "$(_ts)" "${SCRIPT_NAME:-gage}" "$*" >&2
  exit 2
}

need_cmd() {
  local c
  for c in "$@"; do
    command -v "$c" >/dev/null 2>&1 || die "required command not found: $c"
  done
}

# run CMD ARGS...  Execute, or under --dry-run print the quoted command and succeed.
run() {
  if [ "$DRY_RUN" = 1 ]; then
    printf '%s [%s] DRY-RUN: ' "$(_ts)" "${SCRIPT_NAME:-gage}"
    printf '%q ' "$@"
    printf '\n'
    return 0
  fi
  "$@"
}

is_uint() {
  case ${1-} in
    '' | *[!0-9]*) return 1 ;;
    *) return 0 ;;
  esac
}

# Strict dotted IPv4: four decimal fields 0..255 without leading zeros.
valid_ipv4() {
  local ip=${1-} IFS=. o
  local -a parts
  [[ $ip =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || return 1
  read -r -a parts <<<"$ip"
  [ "${#parts[@]}" -eq 4 ] || return 1
  for o in "${parts[@]}"; do
    [[ $o =~ ^(0|[1-9][0-9]{0,2})$ ]] || return 1
    [ "$o" -le 255 ] || return 1
  done
}

# IPv4 or IPv4/prefix.
valid_ipv4_or_cidr() {
  local v=${1-}
  if [[ $v == */* ]]; then
    local ip=${v%/*} p=${v#*/}
    valid_ipv4 "$ip" || return 1
    is_uint "$p" || return 1
    [ "$p" -ge 0 ] && [ "$p" -le 32 ]
  else
    valid_ipv4 "$v"
  fi
}

json_escape() {
  local s=${1-}
  s=${s//\\/\\\\}
  s=${s//\"/\\\"}
  s=${s//$'\n'/\\n}
  s=${s//$'\r'/}
  s=${s//$'\t'/\\t}
  printf '%s' "$s"
}

# env_stdin_get KEY  Read a systemd-style environment file on stdin and print the value of
# KEY=value (blank lines and # comments skipped, one pair of surrounding quotes removed, last
# assignment wins). Exit 1 when the key is absent. Never sources anything, so a `$` or
# backtick in a secret is inert.
env_stdin_get() {
  local key=$1 line val found=1 result=''
  while IFS= read -r line || [ -n "$line" ]; do
    line=${line#"${line%%[![:space:]]*}"}
    case $line in '' | '#'* | ';'*) continue ;; esac
    [ "${line%%=*}" = "$key" ] || continue
    val=${line#*=}
    val=${val%"${val##*[![:space:]]}"}
    case $val in
      \"*\") val=${val#\"}; val=${val%\"} ;;
      \'*\') val=${val#\'}; val=${val%\'} ;;
    esac
    result=$val
    found=0
  done
  [ "$found" -eq 0 ] || return 1
  printf '%s' "$result"
}

# env_file_get FILE KEY  As env_stdin_get on a file; exit 1 when the file is unreadable.
env_file_get() {
  [ -r "$1" ] || return 1
  env_stdin_get "$2" <"$1"
}

# env_file_get_priv FILE KEY  Same, reading the file with root rights (env files are mode 600
# and owned by the service user, so the deploy user cannot read them directly).
env_file_get_priv() {
  local content
  content=$(priv cat -- "$1") || return 1
  env_stdin_get "$2" <<<"$content"
}

# as_user USER CMD ARGS...  Run CMD as another system user (runuser when root, else sudo -u).
as_user() {
  local u=$1
  shift
  if [ "$(id -u)" -eq 0 ]; then
    runuser -u "$u" -- "$@"
  else
    sudo -u "$u" -- "$@"
  fi
}

# json_int_field NAME  Read JSON on stdin, print the integer value of the first "NAME":<int>.
json_int_field() {
  sed -n 's/.*"'"$1"'":[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -n1
}

# wait_until SECONDS INTERVAL CMD ARGS...  Poll CMD until it succeeds; return 1 on timeout.
wait_until() {
  local limit=$1 step=$2
  shift 2
  local start=$SECONDS
  while :; do
    if "$@"; then
      return 0
    fi
    if [ $((SECONDS - start)) -ge "$limit" ]; then
      return 1
    fi
    sleep "$step"
  done
}

# Run a privileged command: directly when already root, else through sudo.
priv() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  else
    sudo "$@"
  fi
}

# conf_value NAME DEFAULT [FILE]  The value of setting NAME: the environment variable when set and
# non-empty, else NAME=... from FILE (systemd-style env file), else DEFAULT.
conf_value() {
  local name=$1 default=$2 file=${3-} v=''
  v=${!name-}
  if [ -z "$v" ] && [ -n "$file" ]; then
    v=$(env_file_get "$file" "$name") || v=''
  fi
  printf '%s' "${v:-$default}"
}
