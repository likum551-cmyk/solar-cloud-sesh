#!/usr/bin/env bash
# One health-check pass, run every minute by gage-healthcheck.timer. Checks, then alerts a webhook
# only on a state change (and every HEALTH_REPEAT_SECONDS while it stays broken).
#
#   healthcheck.sh [--no-alert] [--test-alert] [--verbose]
#
#   --no-alert     print results and update nothing, send nothing
#   --test-alert   send one test message to the webhook and exit (use after configuring it)
#   --verbose      also print every passing check
#
# Checks: master /healthz, platform /health (needs the database), PostgreSQL accepting connections,
# systemd units active (gage-master, gage-platform, caddy, postgresql), every ENABLED gage-server@
# instance active AND answering the UDP query, at least HEALTH_MIN_SERVERS such instances, root disk
# usage, newest database dump age, optional TLS certificate expiry.
# A check that cannot run counts as failed ("unknown" is not "ok").
#
# Settings (environment, e.g. from /etc/gage/healthcheck.env via the systemd unit):
#   GAGE_ALERT_WEBHOOK_URL      https URL; Slack and Discord both accept the JSON sent (placeholder)
#   HEALTH_MASTER_URL           default http://127.0.0.1:8080/healthz
#   HEALTH_PLATFORM_URL         default http://127.0.0.1:8787/health
#   HEALTH_MIN_SERVERS          default 1
#   HEALTH_DISK_MAX_PCT         default 90
#   HEALTH_BACKUP_DIR           default /var/backups/gage
#   HEALTH_BACKUP_MAX_AGE_HOURS default 36
#   HEALTH_TLS_HOSTS            space-separated host names to check on 127.0.0.1:443 (default none)
#   HEALTH_TLS_MIN_DAYS         default 14
#   HEALTH_FAIL_THRESHOLD       consecutive failing runs before the first alert, default 2
#   HEALTH_REPEAT_SECONDS       re-alert interval while broken, default 3600
#   HEALTH_STATE_DIR            default $STATE_DIRECTORY or /var/lib/gage-healthcheck
# Exit codes: 0 all checks passed, 1 something failed (or the alert could not be delivered).
set -euo pipefail

SCRIPT_NAME=healthcheck
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"

ETC_DIR=${GAGE_ETC:-/etc/gage}
NODE_BIN=${NODE_BIN:-/usr/bin/node}
PROBE_CLI=$SCRIPT_DIR/udp-probe.mjs
PROBE_MODULE=${GAGE_PROBE_MODULE:-/opt/gage/master/current/dist/probe.js}
SYSTEMD_DIR=${GAGE_SYSTEMD_DIR:-/etc/systemd/system}

NO_ALERT=0
TEST_ALERT=0
VERBOSE=0
declare -a FAILED=()

usage() {
  sed -n '2,/^set -euo pipefail$/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'
}

pass() { [ "$VERBOSE" != 1 ] || log "ok: $1"; }
fail() { # name detail
  FAILED+=("$1")
  warn "FAIL: $1: $2"
}

json_ok() { # url -> 0 when the endpoint answers {"status":"ok",...}
  local body
  body=$(curl -fsS --max-time 5 "$1" 2>&1) || { printf '%s' "$body"; return 1; }
  case $body in
    *'"status":"ok"'*) return 0 ;;
    *) printf 'unexpected body: %s' "${body:0:120}"; return 1 ;;
  esac
}

check_http() { # name url
  local out
  if out=$(json_ok "$2"); then pass "$1"; else fail "$1" "$2 -> $out"; fi
}

check_unit() { # unit
  if systemctl is-active --quiet "$1"; then pass "unit $1"; else fail "unit $1" "not active"; fi
}

instance_port() {
  local p
  p=$(json_int_field port <"$ETC_DIR/server-$1.json" 2>/dev/null) || p=''
  printf '%s' "${p:-24800}"
}

check_servers() {
  local f i n=0 port out rc
  for f in "$SYSTEMD_DIR"/multi-user.target.wants/gage-server@*.service; do
    [ -e "$f" ] || continue
    i=${f##*/gage-server@}
    i=${i%.service}
    n=$((n + 1))
    if ! systemctl is-active --quiet "gage-server@$i.service"; then
      fail "server@$i" "unit not active"
      continue
    fi
    port=$(instance_port "$i")
    set +e
    out=$("$NODE_BIN" "$PROBE_CLI" 127.0.0.1 "$port" --timeout-ms 1500 --attempts 3 --module "$PROBE_MODULE" 2>&1)
    rc=$?
    set -e
    case $rc in
      0) pass "server@$i UDP $port ($out)" ;;
      3) fail "server@$i" "probe unavailable: $out" ;;
      *) fail "server@$i" "no valid UDP query reply on port $port: $out" ;;
    esac
  done
  local min=${HEALTH_MIN_SERVERS:-1}
  if [ "$n" -lt "$min" ]; then fail "servers" "$n enabled gage-server@ instance(s), expected at least $min"; fi
}

check_disk() {
  local max=${HEALTH_DISK_MAX_PCT:-90} used
  used=$(df --output=pcent / | tail -n1 | tr -dc '0-9')
  if ! is_uint "$used"; then fail "disk" "cannot read usage of /"; return; fi
  if [ "$used" -gt "$max" ]; then fail "disk" "/ is ${used}% full (limit ${max}%)"; else pass "disk ${used}%"; fi
}

check_backup() {
  local dir=${HEALTH_BACKUP_DIR:-/var/backups/gage} hours=${HEALTH_BACKUP_MAX_AGE_HOURS:-36} newest age
  if [ ! -d "$dir" ]; then fail "backup" "$dir does not exist"; return; fi
  newest=$(find "$dir" -maxdepth 1 -type f -name '*.dump' -printf '%T@\n' 2>/dev/null | sort -n | tail -n1)
  newest=${newest%%.*}
  if [ -z "$newest" ]; then fail "backup" "no .dump files in $dir"; return; fi
  age=$((($(date +%s) - newest) / 3600))
  if [ "$age" -gt "$hours" ]; then fail "backup" "newest dump is ${age} h old (limit ${hours} h)"; else pass "backup ${age} h old"; fi
}

check_tls() {
  local h days=${HEALTH_TLS_MIN_DAYS:-14}
  for h in ${HEALTH_TLS_HOSTS:-}; do
    if printf '' | openssl s_client -connect 127.0.0.1:443 -servername "$h" 2>/dev/null |
      openssl x509 -noout -checkend $((days * 86400)) >/dev/null 2>&1; then
      pass "tls $h"
    else
      fail "tls $h" "no certificate served, or it expires within $days days"
    fi
  done
}

send_alert() { # message -> 0 delivered
  local url=${GAGE_ALERT_WEBHOOK_URL:-} msg payload
  if [ -z "$url" ]; then
    warn "GAGE_ALERT_WEBHOOK_URL is not set: alert NOT delivered: $1"
    return 1
  fi
  [[ $url =~ ^https://[^\"[:space:]]+$ ]] || { warn "GAGE_ALERT_WEBHOOK_URL is not a plain https URL: alert NOT delivered"; return 1; }
  msg=$(json_escape "$1")
  payload=$(printf '{"content":"%s","text":"%s"}' "$msg" "$msg")
  # The URL is a secret: give it to curl on stdin, not in argv (visible in /proc/<pid>/cmdline).
  if printf 'url = "%s"\n' "$url" | curl -fsS --max-time 10 -K - -H 'Content-Type: application/json' -d "$payload" -o /dev/null; then
    return 0
  fi
  warn "webhook delivery failed"
  return 1
}

main() {
  while [ $# -gt 0 ]; do
    case $1 in
      --no-alert) NO_ALERT=1; shift ;;
      --test-alert) TEST_ALERT=1; shift ;;
      --verbose) VERBOSE=1; shift ;;
      -h | --help) usage; exit 0 ;;
      *) usage_die "unknown argument: $1 (try --help)" ;;
    esac
  done
  need_cmd curl systemctl df find sort

  local host
  host=$(hostname)
  if [ "$TEST_ALERT" = 1 ]; then
    send_alert "[gage] test alert from $host: the webhook works" || die "test alert not delivered"
    log "test alert delivered"
    return 0
  fi

  check_http master "${HEALTH_MASTER_URL:-http://127.0.0.1:8080/healthz}"
  check_http platform "${HEALTH_PLATFORM_URL:-http://127.0.0.1:8787/health}"
  if pg_isready -q -h 127.0.0.1 -p 5432; then pass "postgres"; else fail "postgres" "not accepting connections on 127.0.0.1:5432"; fi
  local u
  for u in gage-master.service gage-platform.service caddy.service postgresql.service; do check_unit "$u"; done
  check_servers
  check_disk
  check_backup
  check_tls

  local summary='' f
  for f in "${FAILED[@]}"; do summary=${summary:+$summary, }$f; done

  if [ "$NO_ALERT" = 1 ]; then
    if [ -z "$summary" ]; then log "all checks passed"; return 0; fi
    log "failing: $summary (--no-alert: nothing sent, state untouched)"
    return 1
  fi

  local state_dir state now prev_fails prev_alerted prev_last prev_summary
  state_dir=${HEALTH_STATE_DIR:-${STATE_DIRECTORY:-/var/lib/gage-healthcheck}}
  state=$state_dir/state
  mkdir -p -- "$state_dir"
  now=$(date +%s)
  prev_fails=$(env_file_get "$state" fails) || prev_fails=0
  prev_alerted=$(env_file_get "$state" alerted) || prev_alerted=0
  prev_last=$(env_file_get "$state" last_alert) || prev_last=0
  prev_summary=$(env_file_get "$state" summary) || prev_summary=''
  is_uint "$prev_fails" || prev_fails=0
  is_uint "$prev_last" || prev_last=0

  local fails=0 alerted=$prev_alerted last=$prev_last stored=$prev_summary
  local threshold=${HEALTH_FAIL_THRESHOLD:-2} repeat=${HEALTH_REPEAT_SECONDS:-3600}
  local rc=0
  if [ -z "$summary" ]; then
    log "all checks passed"
    if [ "$prev_alerted" = 1 ]; then
      if send_alert "[gage] RECOVERED on $host: all checks pass again"; then alerted=0; stored=''; else rc=1; fi
    else
      stored=''
    fi
  else
    rc=1
    fails=$((prev_fails + 1))
    log "failing ($fails consecutive run(s)): $summary"
    if [ "$fails" -ge "$threshold" ]; then
      if [ "$prev_alerted" != 1 ] || [ "$summary" != "$prev_summary" ] || [ $((now - prev_last)) -ge "$repeat" ]; then
        if send_alert "[gage] ALERT on $host: $summary"; then
          alerted=1; last=$now; stored=$summary
        fi
      fi
    fi
  fi

  {
    printf 'fails=%s\nalerted=%s\nlast_alert=%s\nsummary=%s\n' "$fails" "$alerted" "$last" "$stored"
  } >"$state.tmp"
  mv -T -- "$state.tmp" "$state"
  return "$rc"
}

main "$@"
