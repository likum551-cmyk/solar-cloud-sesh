#!/usr/bin/env bash
# Build, verify and switch one GAGE app (master | platform | server) to a new versioned release,
# and roll back to the previous one when the health check after the switch fails.
#
# Run as the deploy user (it uses sudo for systemctl, systemd-run, journalctl and DB access);
# never as root, because `npm ci` runs dependency install scripts.
#
# Layout (created by README step "Directories"):
#   /opt/gage/<app>/releases/<UTC-timestamp>-<git-sha>/   one directory per release
#   /opt/gage/<app>/current   -> releases/<id>            what the systemd unit runs
#   /opt/gage/<app>/previous  -> releases/<id>            the one before (rollback target)
#
# Steps (any failure before "switch" leaves the live release untouched):
#   1 build   master/platform: rsync source -> npm ci -> npm run build -> npm prune --omit=dev
#             server:          scripts/build-server.sh (cmake + both test programs must pass)
#   2 stage   releases/.staging-<id> is renamed to releases/<id> only once complete
#   3 prepare platform only: pg_dump (label pre-<id>), then migrations
#   4 smoke   start the NEW release as a transient systemd unit on spare ports and require a
#             fresh-boot health answer (master/platform) or a valid UDP query reply (server)
#   5 switch  atomic symlink swap (previous <- current, current <- new)
#   6 restart the unit(s), require a fresh health answer within --wait seconds
#   7 on failure in 6: swap the links back, restart the old release, exit 1
#   8 prune   keep the newest --keep releases plus current and previous
#
# Usage:
#   release.sh APP [--src DIR] [--keep N] [--wait SECONDS] [--test] [--skip-smoke]
#                  [--skip-backup] [--instances "1 test"] [--no-restart] [--yes]
#                  [--jobs N] [--release-id ID] [--dry-run]
#   release.sh APP --rollback [--to RELEASE_ID] [--instances "..."] [--yes] [--dry-run]
#   release.sh APP --list
#
# APP: master | platform | server
# --src DIR          repository root (default $GAGE_SRC or /opt/gage/src)
# --keep N           releases to keep (default 5)
# --wait SECONDS     health wait after a restart (default 60)
# --test             master/platform: also run npm test before pruning dev dependencies
# --skip-smoke       skip step 4 (not recommended)
# --skip-backup      platform: skip the pre-release pg_dump (not recommended)
# --instances LIST   server: instances to restart, in order (default: every enabled instance)
# --no-restart       server: switch the symlink but restart nothing (players keep the old binary)
# --yes              server: allow restarting an instance that has players online
# --rollback         swap current <-> previous (or to --to ID) and restart
# --dry-run          print the plan; change nothing
# Exit codes: 0 done, 1 failed (live release restored when a restart failed), 2 usage.
set -euo pipefail

SCRIPT_NAME=release
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"

GAGE_ROOT=${GAGE_ROOT:-/opt/gage}
ETC_DIR=${GAGE_ETC:-/etc/gage}
SYSTEMD_DIR=${GAGE_SYSTEMD_DIR:-/etc/systemd/system}
SRC=${GAGE_SRC:-$GAGE_ROOT/src}
BACKUP_SCRIPT=$SCRIPT_DIR/backup-db.sh
PROBE_CLI=$SCRIPT_DIR/udp-probe.mjs
NODE_BIN=${NODE_BIN:-/usr/bin/node}
SMOKE_RUN_DIR=${GAGE_SMOKE_RUN_DIR:-/run/gage-release}
SMOKE_WAIT=${GAGE_SMOKE_WAIT:-45}
ID_RE='^[0-9]{8}T[0-9]{6}Z-[A-Za-z0-9._-]+$'

KEEP=5
WAIT=60
RUN_TESTS=0
SKIP_SMOKE=0
SKIP_BACKUP=0
NO_RESTART=0
YES=0
JOBS=''
INSTANCES=''
RELEASE_ID=''
ROLLBACK_TO=''
MODE=release
APP=''

usage() {
  sed -n '2,/^set -euo pipefail$/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'
}

[ $# -ge 1 ] || { usage >&2; exit 2; }
case $1 in -h | --help) usage; exit 0 ;; esac
APP=$1
shift
while [ $# -gt 0 ]; do
  case $1 in
    --src) [ $# -ge 2 ] || usage_die "--src needs a value"; SRC=$2; shift 2 ;;
    --keep) [ $# -ge 2 ] || usage_die "--keep needs a value"; KEEP=$2; shift 2 ;;
    --wait) [ $# -ge 2 ] || usage_die "--wait needs a value"; WAIT=$2; shift 2 ;;
    --jobs) [ $# -ge 2 ] || usage_die "--jobs needs a value"; JOBS=$2; shift 2 ;;
    --instances) [ $# -ge 2 ] || usage_die "--instances needs a value"; INSTANCES=$2; shift 2 ;;
    --release-id) [ $# -ge 2 ] || usage_die "--release-id needs a value"; RELEASE_ID=$2; shift 2 ;;
    --to) [ $# -ge 2 ] || usage_die "--to needs a value"; ROLLBACK_TO=$2; shift 2 ;;
    --test) RUN_TESTS=1; shift ;;
    --skip-smoke) SKIP_SMOKE=1; shift ;;
    --skip-backup) SKIP_BACKUP=1; shift ;;
    --no-restart) NO_RESTART=1; shift ;;
    --yes) YES=1; shift ;;
    --rollback) MODE=rollback; shift ;;
    --list) MODE=list; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h | --help) usage; exit 0 ;;
    *) usage_die "unknown argument: $1 (try --help)" ;;
  esac
done

is_uint "$KEEP" && [ "$KEEP" -ge 2 ] || usage_die "--keep must be an integer >= 2 (current + one rollback target)"
is_uint "$WAIT" && [ "$WAIT" -ge 5 ] || usage_die "--wait must be an integer >= 5"
if [ -n "$RELEASE_ID" ] && ! [[ $RELEASE_ID =~ $ID_RE ]]; then
  usage_die "--release-id must look like 20261029T101500Z-abc12345"
fi
if [ -n "$ROLLBACK_TO" ] && ! [[ $ROLLBACK_TO =~ $ID_RE ]]; then
  usage_die "--to must be a release id (see --list)"
fi

case $APP in
  master)
    SVC_USER=gage-master; UNIT=gage-master.service; ENTRY=dist/index.js
    HEALTH_PATH=/healthz; DEFAULT_PORT=8080; SMOKE_PORT=18080
    ;;
  platform)
    SVC_USER=gage-platform; UNIT=gage-platform.service; ENTRY=dist/src/index.js
    MIGRATE=dist/scripts/migrate.js
    HEALTH_PATH=/health; DEFAULT_PORT=8787; SMOKE_PORT=18787; SMOKE_INTERNAL_PORT=18788
    ;;
  server)
    SVC_USER=gage-server; UNIT='gage-server@.service'
    ;;
  *) usage_die "APP must be master, platform or server (got '$APP')" ;;
esac

APP_ROOT=$GAGE_ROOT/$APP
REL_DIR=$APP_ROOT/releases
CURRENT=$APP_ROOT/current
PREVIOUS=$APP_ROOT/previous

if [ "$(id -u)" -eq 0 ]; then
  die "run as the deploy user, not root (npm ci executes dependency install scripts)"
fi

# ---------------------------------------------------------------------------------------------
# release bookkeeping
# ---------------------------------------------------------------------------------------------
list_releases() {
  local d
  [ -d "$REL_DIR" ] || return 0
  for d in "$REL_DIR"/*; do
    [ -d "$d" ] || continue
    d=${d##*/}
    [[ $d =~ $ID_RE ]] && printf '%s\n' "$d"
  done | sort
}

link_id() { # link path -> release id it points to (empty when absent)
  local t
  t=$(readlink "$1" 2>/dev/null) || return 0
  printf '%s' "${t##*/}"
}

set_link() { # link release-id  (atomic: new symlink beside it, then rename over)
  local link=$1 id=$2 tmp
  tmp=$link.new.$$
  run ln -sfn "releases/$id" "$tmp"
  run mv -T "$tmp" "$link"
}

make_release_id() {
  local sha=nogit
  if git -C "$SRC" rev-parse --git-dir >/dev/null 2>&1; then
    sha=$(git -C "$SRC" rev-parse --short=8 HEAD 2>/dev/null) || sha=nogit
    if [ -n "$(git -C "$SRC" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
      sha=$sha-dirty
    fi
  fi
  printf '%s-%s' "$(date -u +%Y%m%dT%H%M%SZ)" "$sha"
}

prune_releases() {
  local cur prev id n=0 d
  cur=$(link_id "$CURRENT")
  prev=$(link_id "$PREVIOUS")
  # Half-built leftovers of an earlier crashed run (the lock guarantees none is in progress).
  for d in "$REL_DIR"/.staging-*; do
    [ -d "$d" ] || continue
    log "removing stale staging directory ${d##*/}"
    run rm -rf -- "$d"
  done
  while IFS= read -r id; do
    n=$((n + 1))
    if [ "$n" -le "$KEEP" ]; then continue; fi
    if [ "$id" = "$cur" ] || [ "$id" = "$prev" ]; then continue; fi
    [[ $id =~ $ID_RE ]] || die "refusing to delete odd directory name '$id'"
    log "pruning old release $id"
    run rm -rf -- "${REL_DIR:?}/$id"
  done < <(list_releases | sort -r)
}

# ---------------------------------------------------------------------------------------------
# health checks
# ---------------------------------------------------------------------------------------------
# A node app answers {"status":"ok","uptimeSeconds":N,...}. N must be small enough to prove the
# answering process booted after STARTED_EPOCH: a stale process would report a large uptime.
health_fresh() { # url started_epoch
  local body up max
  body=$(curl -fsS --max-time 3 "$1" 2>/dev/null) || return 1
  case $body in *'"status":"ok"'*) ;; *) return 1 ;; esac
  up=$(printf '%s' "$body" | json_int_field uptimeSeconds)
  [ -n "$up" ] || return 1
  max=$(($(date +%s) - $2 + 5))
  [ "$up" -le "$max" ]
}

probe_json() { # ip port -> prints probe JSON, returns probe exit code
  "$NODE_BIN" "$PROBE_CLI" "$1" "$2" --timeout-ms 1500 --attempts 3 --module "$PROBE_MODULE"
}

udp_probe_ok() { probe_json "$1" "$2" >/dev/null 2>&1; }

port_free_tcp() { [ -z "$(ss -H -ltn "sport = :$1")" ]; }
port_free_udp() { [ -z "$(ss -H -lun "sport = :$1")" ]; }

show_unit_log() { # unit
  warn "last log lines of $1:"
  priv journalctl -u "$1" --no-pager -n 40 >&2 || true
}

# ---------------------------------------------------------------------------------------------
# smoke test: the NEW release as a transient unit on spare ports
# ---------------------------------------------------------------------------------------------
STAGE=''
SMOKE_UNIT=''
cleanup_exit() {
  # Global (not local) variables on purpose: the EXIT trap runs after every function has returned.
  case $STAGE in
    "$REL_DIR"/.staging-*) rm -rf -- "$STAGE" ;;
  esac
  smoke_cleanup
}
smoke_cleanup() {
  if [ -n "$SMOKE_UNIT" ]; then
    # Only the transient unit this run created (named below) is stopped, never anything else.
    priv systemctl stop "$SMOKE_UNIT" >/dev/null 2>&1 || true
    priv systemctl reset-failed "$SMOKE_UNIT" >/dev/null 2>&1 || true
    priv rm -f -- "$SMOKE_RUN_DIR/smoke-$APP.env" "$SMOKE_RUN_DIR/smoke-server.json" || true
    SMOKE_UNIT=''
  fi
}

smoke_prepare() {
  local unit=gage-smoke-$APP
  if priv systemctl is-active --quiet "$unit"; then
    log "stopping leftover smoke unit $unit from an earlier run"
    priv systemctl stop "$unit"
  fi
  priv install -d -m 0755 "$SMOKE_RUN_DIR"
  SMOKE_UNIT=$unit
}

smoke_node() { # release-dir
  local dir=$1 started ovr="$SMOKE_RUN_DIR/smoke-$APP.env" url
  port_free_tcp "$SMOKE_PORT" || die "smoke port $SMOKE_PORT is in use"
  smoke_prepare
  {
    printf 'HOST=127.0.0.1\nPORT=%s\n' "$SMOKE_PORT"
    if [ "$APP" = platform ]; then
      port_free_tcp "$SMOKE_INTERNAL_PORT" || die "smoke port $SMOKE_INTERNAL_PORT is in use"
      printf 'INTERNAL_HOST=127.0.0.1\nINTERNAL_PORT=%s\nINTERNAL_ON_PUBLIC_LISTENER=0\n' "$SMOKE_INTERNAL_PORT"
    fi
  } | priv tee "$ovr" >/dev/null
  priv chmod 0600 "$ovr"
  url=http://127.0.0.1:$SMOKE_PORT$HEALTH_PATH
  started=$(date +%s)
  log "smoke: starting $APP ${dir##*/} on 127.0.0.1:$SMOKE_PORT (transient unit $SMOKE_UNIT)"
  # The second EnvironmentFile overrides the first, so the real env file's PORT cannot win.
  priv systemd-run --quiet --unit="$SMOKE_UNIT" --collect \
    --description="GAGE release smoke test ($APP ${dir##*/})" \
    -p User="$SVC_USER" -p Group="$SVC_USER" -p WorkingDirectory="$dir" \
    -p EnvironmentFile="$ETC_DIR/$APP.env" -p EnvironmentFile="$ovr" \
    -p Environment=NODE_ENV=production \
    -p NoNewPrivileges=yes -p ProtectSystem=strict -p ProtectHome=yes -p PrivateTmp=yes \
    -p MemoryMax=512M \
    "$NODE_BIN" "$dir/$ENTRY"
  if ! wait_until "$SMOKE_WAIT" 1 health_fresh "$url" "$started"; then
    show_unit_log "$SMOKE_UNIT"
    smoke_cleanup
    die "smoke test failed: no fresh healthy answer from $url within ${SMOKE_WAIT} s (nothing was switched)"
  fi
  log "smoke: healthy at $url"
  smoke_cleanup
}

smoke_server() { # release-dir
  local dir=$1 port='' p cfg="$SMOKE_RUN_DIR/smoke-server.json"
  [ -f "$PROBE_MODULE" ] || die "master's probe module missing: $PROBE_MODULE (deploy master first, or --skip-smoke)"
  for p in 24890 24891 24892 24893 24894 24895 24896 24897 24898 24899; do
    if port_free_udp "$p"; then port=$p; break; fi
  done
  [ -n "$port" ] || die "no free UDP port in 24890-24899 for the smoke test"
  smoke_prepare
  printf '{"name":"release smoke","port":%s,"maxPlayers":2,"masterUrl":"","bansFile":"","adminPort":0}\n' "$port" |
    priv tee "$cfg" >/dev/null
  priv chmod 0644 "$cfg"
  log "smoke: starting gage-server ${dir##*/} on UDP $port (transient unit $SMOKE_UNIT)"
  priv systemd-run --quiet --unit="$SMOKE_UNIT" --collect \
    --description="GAGE release smoke test (server ${dir##*/})" \
    -p User="$SVC_USER" -p Group="$SVC_USER" -p WorkingDirectory="$SMOKE_RUN_DIR" \
    -p NoNewPrivileges=yes -p ProtectSystem=strict -p ProtectHome=yes -p PrivateTmp=yes \
    -p MemoryMax=512M \
    "$dir/gage-server" --config "$cfg"
  if ! wait_until "$SMOKE_WAIT" 1 udp_probe_ok 127.0.0.1 "$port"; then
    show_unit_log "$SMOKE_UNIT"
    smoke_cleanup
    die "smoke test failed: no valid UDP query reply on port $port (nothing was switched)"
  fi
  log "smoke: UDP query answered on 127.0.0.1:$port"
  smoke_cleanup
}

PROBE_MODULE=${GAGE_PROBE_MODULE:-$GAGE_ROOT/master/current/dist/probe.js}

# ---------------------------------------------------------------------------------------------
# restart + verify
# ---------------------------------------------------------------------------------------------
node_url() {
  local port
  port=$(env_file_get_priv "$ETC_DIR/$APP.env" PORT) || port=$DEFAULT_PORT
  printf 'http://127.0.0.1:%s%s' "$port" "$HEALTH_PATH"
}

restart_node_and_check() {
  local started url
  url=$(node_url)
  started=$(date +%s)
  priv systemctl restart "${UNIT}" || { show_unit_log "$UNIT"; return 1; }
  if ! wait_until "$WAIT" 1 health_fresh "$url" "$started"; then
    show_unit_log "$UNIT"
    return 1
  fi
  log "healthy: $url"
}

server_port() { # instance -> UDP game port from /etc/gage/server-<i>.json
  local p
  p=$(json_int_field port <"$ETC_DIR/server-$1.json") || p=''
  if [ -z "$p" ]; then
    warn "server-$1.json has no \"port\": using the server default 24800"
    p=24800
  fi
  printf '%s' "$p"
}

main_pid() { priv systemctl show -p MainPID --value "$1"; }

restart_instance_and_check() { # instance
  local inst=$1 unit port before after
  unit=gage-server@$inst.service
  port=$(server_port "$inst")
  before=$(main_pid "$unit")
  priv systemctl restart "$unit" || { show_unit_log "$unit"; return 1; }
  after=$(main_pid "$unit")
  if [ "$after" = 0 ] || [ "$after" = "$before" ]; then
    warn "$unit: MainPID is $after (was $before): the process was not replaced"
    show_unit_log "$unit"
    return 1
  fi
  if ! wait_until "$WAIT" 1 udp_probe_ok 127.0.0.1 "$port"; then
    show_unit_log "$unit"
    return 1
  fi
  log "healthy: $unit answers the UDP query on port $port (pid $after)"
}

enabled_instances() {
  local f i
  for f in "$SYSTEMD_DIR"/multi-user.target.wants/gage-server@*.service; do
    [ -e "$f" ] || continue
    i=${f##*/gage-server@}
    i=${i%.service}
    printf '%s\n' "$i"
  done | sort
}

# Must run in the main shell: `die` inside the `< <(resolve_instances)` process substitution below only
# ends that subshell, so a typo in --instances would be ignored and the release would switch with nothing
# restarted. Callers invoke this first, then read the names from resolve_instances.
validate_instances() {
  local i
  [ -z "$INSTANCES" ] && return 0
  for i in $INSTANCES; do
    [[ $i =~ ^[A-Za-z0-9_-]+$ ]] || die "bad instance name '$i'"
    [ -f "$ETC_DIR/server-$i.json" ] || die "no $ETC_DIR/server-$i.json for instance '$i'"
  done
}

resolve_instances() {
  local i
  if [ -n "$INSTANCES" ]; then
    for i in $INSTANCES; do printf '%s\n' "$i"; done
  else
    enabled_instances
  fi
}

# Refuse to bounce an instance that has players unless --yes.
check_players() { # instance...
  local i out players port
  for i in "$@"; do
    priv systemctl is-active --quiet "gage-server@$i.service" || continue
    port=$(server_port "$i")
    if out=$(probe_json 127.0.0.1 "$port" 2>/dev/null); then
      players=$(printf '%s' "$out" | json_int_field players)
      if [ "${players:-0}" -gt 0 ] && [ "$YES" != 1 ]; then
        die "instance $i has $players player(s) online: announce a restart (admin channel: say ...), then re-run with --yes, or leave it out of --instances"
      fi
      log "instance $i: ${players:-0} player(s) online"
    else
      warn "instance $i is active but did not answer the UDP query: restarting it anyway"
    fi
  done
}

# ---------------------------------------------------------------------------------------------
# release flows
# ---------------------------------------------------------------------------------------------
precheck() {
  [ -d "$REL_DIR" ] || die "$REL_DIR does not exist: create the directories first (README, 'Directories')"
  [ -w "$REL_DIR" ] || die "$REL_DIR is not writable by $(id -un): it must be owned by the deploy user"
  [ -f "$SYSTEMD_DIR/$UNIT" ] || die "$SYSTEMD_DIR/$UNIT is not installed (README, 'systemd')"
  need_cmd flock curl ss sudo
  exec 9>"$APP_ROOT/.release.lock"
  flock -n 9 || die "another release or rollback of $APP is running (lock $APP_ROOT/.release.lock)"
}

release_node() {
  need_cmd npm rsync "$NODE_BIN"
  local src_app=$SRC/apps/$APP id stage dir old prev_before ver
  [ -f "$src_app/package.json" ] || die "$src_app/package.json not found (is --src the repository root?)"
  [ -f "$src_app/package-lock.json" ] || die "$src_app/package-lock.json missing: npm ci needs the lockfile"
  [ -f "$ETC_DIR/$APP.env" ] || die "$ETC_DIR/$APP.env missing (README, 'Environment files')"
  ver=$("$NODE_BIN" --version | sed 's/^v//')
  [[ $ver =~ ^([0-9]+)\.([0-9]+) ]] || die "cannot parse node version '$ver'"
  if [ "${BASH_REMATCH[1]}" -lt 22 ] || { [ "${BASH_REMATCH[1]}" -eq 22 ] && [ "${BASH_REMATCH[2]}" -lt 12 ]; }; then
    die "node $ver is older than the 22.12 the apps require"
  fi
  if ! grep -q -- "$ENTRY" "$SYSTEMD_DIR/$UNIT"; then
    die "$SYSTEMD_DIR/$UNIT does not run $ENTRY: the build layout and the unit disagree (update ExecStart)"
  fi
  id=${RELEASE_ID:-$(make_release_id)}
  stage=$REL_DIR/.staging-$id
  dir=$REL_DIR/$id
  [ ! -e "$dir" ] || die "release $id already exists"
  old=$(link_id "$CURRENT")
  prev_before=$(link_id "$PREVIOUS")

  if [ "$DRY_RUN" = 1 ]; then
    log "dry run for $APP release $id (current: ${old:-none}, previous: ${prev_before:-none})"
    log "  1 rsync $src_app/ -> $stage (no node_modules, dist, .env*)"
    log "  2 npm ci; npm run build; $([ "$RUN_TESTS" = 1 ] && printf 'npm test; ')npm prune --omit=dev; check $ENTRY"
    [ "$APP" != platform ] || log "  3 backup-db.sh --label pre-$id; node $MIGRATE as $SVC_USER"
    log "  4 smoke: transient unit on 127.0.0.1:$SMOKE_PORT, require fresh $HEALTH_PATH"
    log "  5 switch: previous <- ${old:-none}, current <- $id"
    log "  6 systemctl restart $UNIT; require a fresh health answer within ${WAIT}s"
    log "  7 failure -> links back to ${old:-none} and restart"
    log "  8 prune to $KEEP releases"
    return 0
  fi

  STAGE=$stage
  trap cleanup_exit EXIT
  mkdir -p -- "$stage"
  log "staging $src_app -> $stage"
  rsync -a --delete --exclude=node_modules --exclude=dist --exclude='.env*' --exclude=.git \
    --exclude='*.log' --exclude=.pglite "$src_app/" "$stage/"
  log "npm ci (all dependencies, needed to compile)"
  (cd "$stage" && env -u NODE_ENV npm ci --no-audit --no-fund) || die "npm ci failed"
  log "npm run build"
  (cd "$stage" && env -u NODE_ENV npm run build) || die "build failed"
  if [ "$RUN_TESTS" = 1 ]; then
    log "npm test"
    (cd "$stage" && env -u NODE_ENV npm test) || die "tests failed: nothing was switched"
  fi
  log "npm prune --omit=dev"
  (cd "$stage" && env -u NODE_ENV npm prune --omit=dev --no-audit --no-fund) || die "npm prune failed"
  [ -f "$stage/$ENTRY" ] || die "build produced no $ENTRY (the compiled layout changed: update the unit and this script)"
  if [ "$APP" = platform ]; then
    [ -f "$stage/$MIGRATE" ] || die "build produced no $MIGRATE"
    [ -f "$stage/drizzle/meta/_journal.json" ] || die "drizzle/meta/_journal.json missing from the release"
  fi
  {
    printf 'app=%s\nid=%s\nbuilt=%s\nbuilt_by=%s\nnode=%s\n' "$APP" "$id" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(id -un)" "$ver"
    printf 'git=%s\n' "$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo none)"
  } >"$stage/BUILD-INFO"
  chmod -R a+rX,go-w "$stage"
  mv -T "$stage" "$dir"
  STAGE=''
  log "release directory ready: $dir"

  if [ "$APP" = platform ]; then
    if [ "$SKIP_BACKUP" = 1 ]; then
      warn "--skip-backup: no database backup before the migration"
    else
      log "database backup before migrating (label pre-$id)"
      priv bash "$BACKUP_SCRIPT" --label "pre-$id" || die "pre-release backup failed: not migrating"
    fi
    log "applying migrations ($MIGRATE as $SVC_USER)"
    # Compiled migrate script: needs only production dependencies. Read DATABASE_URL from the same
    # env file the service uses, so a bad configuration fails here, before anything is switched.
    (cd "$dir" && as_user "$SVC_USER" env NODE_ENV=production "$NODE_BIN" --env-file="$ETC_DIR/$APP.env" "$dir/$MIGRATE") ||
      die "migration failed: the live release is untouched; the schema may be partly changed only if a migration was not transactional (backup: pre-$id)"
  fi

  if [ "$SKIP_SMOKE" = 1 ]; then
    warn "--skip-smoke: the new release has not been run before the switch"
  else
    smoke_node "$dir"
  fi

  log "switch: previous <- ${old:-none}, current <- $id"
  [ -z "$old" ] || set_link "$PREVIOUS" "$old"
  set_link "$CURRENT" "$id"
  if priv systemctl is-enabled --quiet "$UNIT"; then :; else
    warn "$UNIT is not enabled at boot: run 'sudo systemctl enable $UNIT' once"
  fi
  if restart_node_and_check; then
    log "release $id of $APP is live"
    prune_releases
    return 0
  fi
  warn "release $id failed its health check after the restart"
  if [ -n "$old" ]; then
    set_link "$CURRENT" "$old"
    if [ -n "$prev_before" ]; then set_link "$PREVIOUS" "$prev_before"; else rm -f -- "$PREVIOUS"; fi
    if restart_node_and_check; then
      die "ROLLED BACK to $old (release $id is kept in $REL_DIR for inspection)"
    fi
    die "ROLLBACK FAILED: $old does not come up healthy either. Manual intervention needed (journalctl -u $UNIT)"
  fi
  priv systemctl stop "$UNIT" || true
  die "first release failed and there is no earlier release: $UNIT was stopped. Fix the cause (journalctl -u $UNIT) and release again"
}

release_server() {
  need_cmd cmake "$NODE_BIN"
  local id stage dir old prev_before inst bin
  local -a insts=() done_insts=()
  id=${RELEASE_ID:-$(make_release_id)}
  stage=$REL_DIR/.staging-$id
  dir=$REL_DIR/$id
  bin=$SRC/build/server/server/gage-server
  [ ! -e "$dir" ] || die "release $id already exists"
  old=$(link_id "$CURRENT")
  prev_before=$(link_id "$PREVIOUS")
  if [ "$NO_RESTART" != 1 ]; then
    validate_instances
    while IFS= read -r inst; do [ -z "$inst" ] || insts+=("$inst"); done < <(resolve_instances)
    [ "${#insts[@]}" -gt 0 ] || warn "no enabled gage-server@ instances: the binary is installed but nothing will be restarted"
  fi

  if [ "$DRY_RUN" = 1 ]; then
    log "dry run for server release $id (current: ${old:-none}, previous: ${prev_before:-none})"
    log "  1 build-server.sh --src $SRC (cmake Release + netcode_test + server_test must pass)"
    log "  2 install $bin -> $stage/gage-server, rename to $dir"
    log "  3 smoke: transient unit on a spare UDP port, require a valid GAGE query reply"
    log "  4 refuse when an instance to restart has players online (unless --yes)"
    log "  5 switch: previous <- ${old:-none}, current <- $id"
    log "  6 restart in order: ${insts[*]:-(none)}; require a UDP query reply from each"
    log "  7 failure -> links back to ${old:-none}; restart the instances already restarted plus the failing one"
    log "  8 prune to $KEEP releases"
    return 0
  fi

  if [ "$NO_RESTART" != 1 ] && [ "${#insts[@]}" -gt 0 ]; then
    [ -f "$PROBE_MODULE" ] || [ "$SKIP_SMOKE" = 1 ] || die "master's probe module missing: $PROBE_MODULE (deploy master first)"
    check_players "${insts[@]}"
  fi

  log "building (this runs both test programs)"
  bash "$SCRIPT_DIR/build-server.sh" --src "$SRC" ${JOBS:+--jobs "$JOBS"} || die "build or tests failed: nothing was switched"
  [ -x "$bin" ] || die "$bin missing after build"

  STAGE=$stage
  trap cleanup_exit EXIT
  mkdir -p -- "$stage"
  install -m 0755 "$bin" "$stage/gage-server"
  {
    printf 'app=server\nid=%s\nbuilt=%s\nbuilt_by=%s\n' "$id" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(id -un)"
    printf 'git=%s\n' "$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo none)"
    printf 'sha256=%s\n' "$(sha256sum "$stage/gage-server" | cut -d' ' -f1)"
    printf 'gcc=%s\n' "$(g++ --version | head -n1)"
  } >"$stage/BUILD-INFO"
  chmod -R a+rX,go-w "$stage"
  mv -T "$stage" "$dir"
  STAGE=''
  log "release directory ready: $dir"

  if [ "$SKIP_SMOKE" = 1 ]; then
    warn "--skip-smoke: the new binary has not been run before the switch"
  else
    smoke_server "$dir"
  fi

  # Building takes minutes: players may have joined since the first check.
  if [ "$NO_RESTART" != 1 ] && [ "${#insts[@]}" -gt 0 ]; then check_players "${insts[@]}"; fi

  log "switch: previous <- ${old:-none}, current <- $id"
  [ -z "$old" ] || set_link "$PREVIOUS" "$old"
  set_link "$CURRENT" "$id"

  if [ "$NO_RESTART" = 1 ] || [ "${#insts[@]}" -eq 0 ]; then
    warn "no instance restarted: running instances keep the old binary until their next restart"
    prune_releases
    return 0
  fi
  for inst in "${insts[@]}"; do
    if restart_instance_and_check "$inst"; then
      done_insts+=("$inst")
      continue
    fi
    warn "instance $inst failed after the restart with release $id"
    done_insts+=("$inst")
    if [ -z "$old" ]; then
      priv systemctl stop "gage-server@$inst.service" || true
      die "first release failed and there is no earlier release: instance $inst was stopped"
    fi
    set_link "$CURRENT" "$old"
    if [ -n "$prev_before" ]; then set_link "$PREVIOUS" "$prev_before"; else rm -f -- "$PREVIOUS"; fi
    local bad=0
    for inst in "${done_insts[@]}"; do
      restart_instance_and_check "$inst" || bad=1
    done
    [ "$bad" = 0 ] || die "ROLLBACK INCOMPLETE: at least one instance is not healthy on $old. Manual intervention needed"
    die "ROLLED BACK to $old (release $id is kept in $REL_DIR for inspection)"
  done
  log "release $id of server is live on: ${insts[*]}"
  prune_releases
}

do_rollback() {
  local cur target prev inst
  local -a insts=()
  cur=$(link_id "$CURRENT")
  prev=$(link_id "$PREVIOUS")
  target=${ROLLBACK_TO:-$prev}
  [ -n "$target" ] || die "no previous release to roll back to (see --list)"
  [ "$target" != "$cur" ] || die "$target is already the current release"
  [ -d "$REL_DIR/$target" ] || die "release $target does not exist in $REL_DIR"
  if [ "$APP" = server ]; then
    if [ "$NO_RESTART" != 1 ]; then
      validate_instances
      while IFS= read -r inst; do [ -z "$inst" ] || insts+=("$inst"); done < <(resolve_instances)
      [ "${#insts[@]}" -eq 0 ] || check_players "${insts[@]}"
    fi
  fi
  log "rollback $APP: current ${cur:-none} -> $target (previous becomes ${cur:-none})"
  if [ "$DRY_RUN" = 1 ]; then
    log "dry run: would swap the links and restart ${insts[*]:-$UNIT}"
    return 0
  fi
  [ -z "$cur" ] || set_link "$PREVIOUS" "$cur"
  set_link "$CURRENT" "$target"
  if [ "$APP" = server ]; then
    for inst in "${insts[@]}"; do
      restart_instance_and_check "$inst" || die "instance $inst is not healthy after the rollback: manual intervention needed"
    done
  else
    restart_node_and_check || die "$APP is not healthy after the rollback: manual intervention needed (journalctl -u $UNIT)"
  fi
  if [ "$APP" = platform ]; then
    warn "code is rolled back; the database schema is NOT. If the rolled-back release cannot run on the migrated schema, restore the pre-release dump (restore-db.sh)"
  fi
  log "rolled back to $target"
}

do_list() {
  local cur prev id
  cur=$(link_id "$CURRENT")
  prev=$(link_id "$PREVIOUS")
  if [ -z "$(list_releases)" ]; then
    log "no releases under $REL_DIR"
    return 0
  fi
  while IFS= read -r id; do
    printf '%s%s%s\n' "$id" "$([ "$id" = "$cur" ] && printf '  <- current')" "$([ "$id" = "$prev" ] && printf '  <- previous')"
  done < <(list_releases)
}

# ---------------------------------------------------------------------------------------------
case $MODE in
  list) do_list ;;
  rollback) precheck; do_rollback ;;
  release)
    [ -d "$SRC" ] || die "source directory $SRC not found (--src)"
    precheck
    if [ "$APP" = server ]; then release_server; else release_node; fi
    ;;
  *) die "internal error: mode $MODE" ;;
esac
