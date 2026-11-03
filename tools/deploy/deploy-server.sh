#!/usr/bin/env bash
# Build, test and roll out gage-server on the VPS (native Linux build, systemd unit gage-server).
#
#   deploy-server.sh [--dry-run] [--build-only] [--wait SECONDS] [--allow-players] [--no-announce]
#                    [--no-auto-rollback] [--jobs N]
#   deploy-server.sh --activate <stamp|previous> [--allow-players] [--wait S] ...   switch to a staged release
#   deploy-server.sh --rollback                      same as --activate previous
#   deploy-server.sh --status                        what runs now, which releases exist
#
# Flow: cmake/ninja build of apps/server into /opt/gage/build/server-<stamp> -> netcode_test and server_test
# run BY NAME (never by glob) -> canary: the new binary starts with the production server.json (admin channel,
# master announce and bans switched off) on a spare port -> install to /opt/gage/server/releases/<stamp> ->
# only then: player check, in-game warning through the admin channel, atomic `current` symlink swap,
# `systemctl restart gage-server`, health check (unit active AND the UDP query answers). A failed health check
# rolls back to the previous release on its own (exit 4; exit 5 if the rollback fails too).
#
# Players: if anyone is online the script stops BEFORE touching anything live (exit 3) unless --allow-players.
# With --allow-players it sends `say` through the admin channel (needs adminPort in server.json and the token in
# $GAGE_ADMIN_TOKEN or /etc/gage/server.env), waits --wait seconds (default 30), then restarts.
#
# Output contract: once a release is staged (a normal run, or --build-only) a line `RELEASE_STAMP=<stamp>` is printed
# on stdout by itself; tools/deploy/release.sh reads it to activate exactly that release later.
#
# Exit codes: 0 ok | 1 failure | 2 usage | 3 players online | 4 new release failed its health check, rolled back
#             | 5 failed and the rollback failed too | 6 something else already answers on the game port
#             | 75 another deploy is running
set -euo pipefail

SCRIPT_NAME=deploy-server
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }

BUILD_ONLY=0; ACTIVATE=''; STATUS=0; WAIT=30; ALLOW_PLAYERS=0; ANNOUNCE=1; AUTO_ROLLBACK=1; JOBS=''
while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) DRY_RUN=1; shift ;;
    --build-only) BUILD_ONLY=1; shift ;;
    --activate) [ $# -ge 2 ] || usage_die "--activate needs a stamp or 'previous'"; ACTIVATE=$2; shift 2 ;;
    --rollback) ACTIVATE=previous; shift ;;
    --status) STATUS=1; shift ;;
    --wait) [ $# -ge 2 ] && is_uint "$2" && [ "$2" -le 600 ] || usage_die "--wait needs seconds (0..600)"; WAIT=$2; shift 2 ;;
    --allow-players) ALLOW_PLAYERS=1; shift ;;
    --no-announce) ANNOUNCE=0; shift ;;
    --no-auto-rollback) AUTO_ROLLBACK=0; shift ;;
    --jobs) [ $# -ge 2 ] && is_uint "$2" && [ "$2" -ge 1 ] || usage_die "--jobs needs a positive number"; JOBS=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage_die "unknown argument $1" ;;
  esac
done

deploy_config
SRV=$GAGE_SERVER_DIR
REL=$SRV/releases
CUR=$SRV/current
PREV=$SRV/previous
CONF=$SRV/server.json
ENVF="${GAGE_SERVER_ENV_FILE:-/etc/gage/server.env}"
UNIT=${GAGE_SERVER_UNIT:-gage-server}
LOGDIR=$GAGE_OPT/logs/deploy
KEEP_RELEASES=5

unit_active() { command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet "$UNIT"; }

# game_port, players_online and admin_token live in lib/common.sh (release.sh uses them too); they read $CONF and $ENVF.

announce() {
  local players=$1 port token
  if [ "$ANNOUNCE" = 0 ]; then log "announce skipped (--no-announce)"; return 0; fi
  port=0; [ -f "$CONF" ] && port=$(ops_js json-get "$CONF" adminPort 0)
  token=$(admin_token)
  if [ "$port" = 0 ]; then log "no in-game warning: adminPort is 0 in $CONF (add \"adminPort\": 24900 and a token to enable it)"; return 0; fi
  if [ -z "$token" ]; then log "no in-game warning: adminPort is $port but no token (set GAGE_ADMIN_TOKEN in $ENVF)"; return 0; fi
  log "warning $players player(s): 'say' through the admin channel on 127.0.0.1:$port, then waiting ${WAIT}s"
  if [ "$DRY_RUN" = 1 ]; then log "[dry-run] admin-cmd $port say Server restarting in ${WAIT} seconds for an update."; return 0; fi
  GAGE_ADMIN_TOKEN=$token ops_js admin-cmd "$port" say "Server restarting in ${WAIT} seconds for an update. Please finish what you are doing." >/dev/null \
    || warn "the admin channel did not accept the warning; players will see a disconnect without notice"
  log "waiting ${WAIT}s (wall clock) before the restart"
  sleep "$WAIT"
}

health_check() { # health_check SECONDS: unit active AND the UDP query answers, polled once a second
  local deadline=$((SECONDS + $1)) port
  port=$(game_port)
  while [ "$SECONDS" -lt "$deadline" ]; do
    if unit_active && ops_js udp-query 127.0.0.1 "$port" --timeout-ms 1000 --attempts 1 >"$LOGDIR/.query.out" 2>&1; then
      log "healthy: unit active, UDP query answered: $(cat "$LOGDIR/.query.out")"
      return 0
    fi
    sleep 1
  done
  return 1
}

set_link() { # set_link NAME TARGET: atomic symlink replace (rename(2) over the old link)
  local name=$1 target=$2 tmp
  tmp="$SRV/.$name.new.$$"
  ln -sfn "$target" "$tmp"
  mv -T -- "$tmp" "$SRV/$name"
}

list_releases() { find -H "$REL" -mindepth 1 -maxdepth 1 -type d -name '2*Z' -printf '%f\n' 2>/dev/null | sort; }

# ---- --status ----------------------------------------------------------------------------
if [ "$STATUS" = 1 ]; then
  log "current  -> $(readlink "$CUR" 2>/dev/null || echo '(none)')"
  log "previous -> $(readlink "$PREV" 2>/dev/null || echo '(none)')"
  log "unit $UNIT: $(command -v systemctl >/dev/null 2>&1 && systemctl is-active "$UNIT" 2>&1 || echo 'systemctl not available')"
  log "UDP query on 127.0.0.1:$(game_port): $(ops_js udp-query 127.0.0.1 "$(game_port)" --timeout-ms 1200 --attempts 2 2>&1 || true)"
  log "releases: $(list_releases | tr '\n' ' ')"
  exit 0
fi

# ---- activation --------------------------------------------------------------------------
# check_bans_path: gage-server.service runs with ProtectSystem=strict and only $SRV/data writable (a compromised
# server must not be able to swap its own binary under releases/ or current). A bansFile anywhere else would
# load fine and then fail on the first ban. "" (memory only) is allowed.
check_bans_path() {
  [ -f "$CONF" ] || return 0
  local b
  b=$(ops_js json-get "$CONF" bansFile bans.json) || die "cannot read bansFile from $CONF"
  [ -n "$b" ] || return 0
  case $b in
    *..*) die "$CONF: bansFile '$b' contains '..'" ;;
    data/*|"$SRV"/data/*) return 0 ;;
  esac
  die "$CONF: \"bansFile\" is '$b', but the service can only write under $SRV/data/. Set \"bansFile\": \"data/bans.json\" (and move the old bans.json there), or \"\" for memory-only bans."
}

# warn_pinned_rates: a server.json that pins rateNearHz below tickHz is VALID (the server starts) but caps what players
# near each other receive at rateNearHz per second instead of tickHz (the 0.4.2 live test ran like that for a whole
# session). The server logs the same sentence at start (apps/server/config.cpp ConfigWarnings); this prints it BEFORE the
# activation, where the operator can still stop. It only reads $CONF and never rewrites it. Defaults mirror the
# server's: tickHz 30, rateNearHz 30 (apps/server/replconfig.h).
warn_pinned_rates() {
  [ -f "$CONF" ] || return 0
  local tick near
  tick=$(ops_js json-get "$CONF" tickHz 30 2>/dev/null) || return 0
  near=$(ops_js json-get "$CONF" rateNearHz 30 2>/dev/null) || return 0
  is_uint "$tick" && is_uint "$near" || return 0
  if [ "$near" -lt "$tick" ]; then
    warn "$CONF: rateNearHz ($near) is below tickHz ($tick): players within nearDistance reach each other at $near/s, not $tick/s (the world still ticks at $tick Hz); raise rateNearHz to tickHz or remove it from server.json. This script does not change the file."
  fi
}

# activation_gate STAMP: the two reasons not to touch the live server. Exits 6 when something that is not our
# unit holds the game port (the by-hand tmux server), exits 3 when players are online and --allow-players was
# not given; otherwise warns the players (when any) and returns.
activation_gate() {
  local stamp=$1 players
  check_bans_path
  warn_pinned_rates
  if ! unit_active; then
    if ops_js udp-query 127.0.0.1 "$(game_port)" --timeout-ms 1000 --attempts 2 >/dev/null 2>&1; then
      die "unit $UNIT is not running but something already answers the GAGE query on UDP $(game_port): the server started by hand in tmux? Stop it first (docs/OPS.md, 'First cutover'), then re-run." 6
    fi
    players=0
  else
    players=$(players_online)
  fi
  if [ "$players" != 0 ] && [ "$ALLOW_PLAYERS" = 0 ]; then
    if [ "$players" = '?' ]; then
      warn "unit $UNIT is active but the UDP query does not answer, so the player count is unknown"
    else
      warn "$players player(s) are online"
    fi
    warn "release $stamp is staged in $REL/$stamp but NOT activated."
    warn "Get GI's OK, then: $0 --activate $stamp --allow-players"
    exit 3
  fi
  [ "$players" = 0 ] || announce "$players"
  log "activation gate passed (players: $players)"
}

activate_release() {
  local stamp=$1 new_dir old_target
  if [ "$stamp" = previous ]; then
    stamp=$(readlink "$PREV" 2>/dev/null || true); stamp=${stamp#releases/}
    [ -n "$stamp" ] || die "there is no previous release to go back to"
  fi
  [[ $stamp =~ ^[0-9]{8}T[0-9]{6}Z$ ]] || die "'$stamp' is not a release stamp (see --status)"
  new_dir=$REL/$stamp
  [ -x "$new_dir/gage-server" ] || die "$new_dir/gage-server is missing or not executable"
  old_target=$(readlink "$CUR" 2>/dev/null || true)
  if [ "releases/$stamp" = "$old_target" ] && unit_active; then log "release $stamp is already current and running"; return 0; fi

  activation_gate "$stamp"

  if [ "$DRY_RUN" = 1 ]; then
    log "[dry-run] ln: previous -> '${old_target:-none}', current -> releases/$stamp (atomic rename)"
    log "[dry-run] systemctl restart $UNIT, then health check for 30 s (unit active + UDP query on $(game_port))"
    return 0
  fi

  [ -z "$old_target" ] || [ "$old_target" = "releases/$stamp" ] || set_link previous "$old_target"
  set_link current "releases/$stamp"
  log "current -> releases/$stamp (previous -> ${old_target:-none})"
  systemctl restart "$UNIT"
  if health_check 30; then
    return 0
  fi
  warn "the new release did not become healthy within 30 s. Last log lines:"
  journalctl -u "$UNIT" -n 40 --no-pager >&2 || true
  if [ "$AUTO_ROLLBACK" = 1 ] && [ -n "$old_target" ] && [ "$old_target" != "releases/$stamp" ] && [ -x "$SRV/$old_target/gage-server" ]; then
    warn "rolling back to $old_target"
    set_link current "$old_target"
    set_link previous "releases/$stamp"
    systemctl restart "$UNIT"
    if health_check 30; then
      warn "rolled back; $stamp is left in $REL for inspection. The old release is serving."
      exit 4
    fi
    warn "the rollback is NOT healthy either: $(systemctl is-active "$UNIT" 2>&1 || true). Look at: journalctl -u $UNIT -n 100"
    exit 5
  fi
  die "release $stamp failed its health check and there was nothing to roll back to (journalctl -u $UNIT -n 100)" 1
}

prune() {
  local keep_cur keep_prev r n=0 dirs=()
  keep_cur=$(readlink "$CUR" 2>/dev/null || true); keep_cur=${keep_cur#releases/}
  keep_prev=$(readlink "$PREV" 2>/dev/null || true); keep_prev=${keep_prev#releases/}
  mapfile -t dirs < <(list_releases | sort -r)
  for r in "${dirs[@]}"; do
    n=$((n + 1))
    [ "$n" -gt "$KEEP_RELEASES" ] || continue
    [ "$r" != "$keep_cur" ] && [ "$r" != "$keep_prev" ] || continue
    [[ $r =~ ^[0-9]{8}T[0-9]{6}Z$ ]] || continue
    log "pruning old release $r"
    rm -rf -- "${REL:?}/$r"
  done
  # old build trees, keep the newest two
  # find -H: /opt/gage/src/build is a symlink to /opt/gage/build
  mapfile -t dirs < <(find -H "$GAGE_BUILD_ROOT" -mindepth 1 -maxdepth 1 -type d -name 'server-2*Z' -printf '%f\n' 2>/dev/null | sort -r)
  n=0
  for r in "${dirs[@]}"; do
    n=$((n + 1))
    [ "$n" -gt 2 ] || continue
    [[ $r =~ ^server-[0-9]{8}T[0-9]{6}Z$ ]] || continue
    log "pruning old build tree $r"
    rm -rf -- "${GAGE_BUILD_ROOT:?}/$r"
  done
}

acquire_lock deploy-server
mkdir -p "$LOGDIR" 2>/dev/null || [ "$DRY_RUN" = 1 ] || die "cannot create $LOGDIR"

if [ -n "$ACTIVATE" ]; then
  [ "$DRY_RUN" = 1 ] || [ -d "$REL" ] || die "$REL does not exist"
  activate_release "$ACTIVATE"
  [ "$DRY_RUN" = 1 ] || prune
  exit 0
fi

# ---- build, test, canary, stage ----------------------------------------------------------
need_cmd cmake ninja g++ timeout node sha256sum
[ -n "$JOBS" ] || { JOBS=$(nproc 2>/dev/null || echo 2); [ "$JOBS" -le 4 ] || JOBS=4; }
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
BUILD=$GAGE_BUILD_ROOT/server-$STAMP
NEW_REL=$REL/$STAMP
CMAKE_SRC=$GAGE_SRC/apps/server     # the standalone entry point: independent of edits to the root CMakeLists.txt
[ -f "$CMAKE_SRC/CMakeLists.txt" ] || die "missing $CMAKE_SRC/CMakeLists.txt (is GAGE_SRC=$GAGE_SRC the repository root?)"

if [ "$DRY_RUN" = 1 ]; then
  log "dry-run plan for release $STAMP (nothing is built or changed):"
  log "  1 cmake -S $CMAKE_SRC -B $BUILD -G Ninja -DCMAKE_BUILD_TYPE=Release ; cmake --build $BUILD --target gage-server netcode_test server_test --parallel $JOBS"
  log "  2 run by name in a scratch dir: $BUILD/selftest/netcode_test, $BUILD/selftest/server_test (timeout 900 s each)"
  log "  3 canary: the new binary with $CONF (sanitized: no admin, no master, memory-only bans) on a spare UDP port for 5 s"
  log "  4 install -m 755 gage-server -> $NEW_REL/ ; BUILD-INFO with sha256 and a source fingerprint"
  if [ -f "$CONF" ]; then log "  server.json: $CONF (game port $(game_port))"
  else log "  server.json: $CONF does not exist yet (the first deploy seeds it from apps/server/server.json.example)"; fi
  if [ "$BUILD_ONLY" = 1 ]; then log "  5 stop (--build-only)"; exit 0; fi
  log "  5 activation: gate (a stranger on the game port / players online), announce, swap symlinks, restart, health check"
  activation_gate "$STAMP"
  log "dry-run complete"
  exit 0
fi

log "configure + build into $BUILD with $JOBS job(s)"
cmake -S "$CMAKE_SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release > "$LOGDIR/$STAMP-configure.log" 2>&1 \
  || { tail -n 40 "$LOGDIR/$STAMP-configure.log" >&2; die "cmake configure failed (full log: $LOGDIR/$STAMP-configure.log)"; }
cmake --build "$BUILD" --target gage-server netcode_test server_test --parallel "$JOBS" > "$LOGDIR/$STAMP-build.log" 2>&1 \
  || { tail -n 60 "$LOGDIR/$STAMP-build.log" >&2; die "build failed (full log: $LOGDIR/$STAMP-build.log)"; }
BIN=$BUILD/server/gage-server
for f in "$BIN" "$BUILD/selftest/netcode_test" "$BUILD/selftest/server_test"; do
  [ -x "$f" ] || die "build finished but $f is missing"
done

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/gage-deploy-server.XXXXXX")
defer "rm -rf -- $(printf '%q' "$SCRATCH")"
tests_run=0
for t in netcode_test server_test; do
  log "running $t (timeout 900 s)"
  set +e
  ( cd "$SCRATCH" && timeout -k 10 900 "$BUILD/selftest/$t" ) < /dev/null > "$LOGDIR/$STAMP-$t.log" 2>&1
  rc=$?
  set -e
  if [ "$rc" -ne 0 ]; then
    tail -n 40 "$LOGDIR/$STAMP-$t.log" >&2
    die "$t failed with exit $rc (full log: $LOGDIR/$STAMP-$t.log). Nothing was deployed."
  fi
  tests_run=$((tests_run + 1))
  grep -iE 'passed|skipped' "$LOGDIR/$STAMP-$t.log" | tail -n 3 || true
done
[ "$tests_run" -eq 2 ] || die "expected to run 2 tests, ran $tests_run"

# canary: does the NEW binary accept the PRODUCTION config? (an unknown key is a fatal error at start)
CANARY_PORT=$(ops_js free-udp-port)
CANARY_ARGS=(--port "$CANARY_PORT")
if [ -f "$CONF" ]; then
  ops_js sanitize-config "$CONF" "$SCRATCH/canary.json"
  CANARY_ARGS=(--config "$SCRATCH/canary.json" --port "$CANARY_PORT")
else
  log "no $CONF yet: the canary runs on the built-in defaults"
fi
set +e
# stdin from /dev/null: gage-server reads console commands from stdin, and timeout puts it in its own process
# group, so with the terminal as stdin the read stops the whole process (SIGTTIN) and the 5 s timeout never ends it.
( cd "$SCRATCH" && timeout -k 5 -s TERM 5 "$BIN" "${CANARY_ARGS[@]}" ) < /dev/null > "$SCRATCH/canary.log" 2>&1
rc=$?
set -e
if [ "$rc" -ne 124 ] || ! grep -q 'listening on UDP' "$SCRATCH/canary.log"; then
  cat "$SCRATCH/canary.log" >&2
  die "canary: the new binary did not start with the production config (exit $rc). Nothing was deployed."
fi
log "canary ok: the new binary started with the production config and stayed up for 5 s"

# stage the release (root-owned, so the service user can run but not rewrite it)
install -d -m 755 "$REL" "$NEW_REL"
install -m 755 "$BIN" "$NEW_REL/gage-server"
{
  echo "built:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "binary:  sha256 $(sha256_of "$NEW_REL/gage-server")"
  echo "compiler: $(g++ --version | head -n 1)"
  echo "cmake:   $(cmake --version | head -n 1)"
  echo "source:  $GAGE_SRC fingerprint $( (cd "$GAGE_SRC" && LC_ALL=C find apps/server libs cmake third_party/enet -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum) | cut -c1-16 )"
  echo "tests:   netcode_test, server_test passed; canary ok"
} > "$NEW_REL/BUILD-INFO"
log "staged $NEW_REL ($(file_size "$NEW_REL/gage-server") bytes, sha256 $(sha256_of "$NEW_REL/gage-server"))"
# Machine-readable (tools/deploy/release.sh reads it): the stamp of the release that was just staged, on its own line.
printf 'RELEASE_STAMP=%s\n' "$STAMP"

if [ ! -f "$CONF" ]; then
  log "seeding $CONF from apps/server/server.json.example (edit name, maxPlayers, adminPort as needed)"
  # root-owned, group-readable: the service can read its config but not rewrite it. The ban list moves to data/.
  seed_tmp=$(mktemp "$SRV/.server.json.XXXXXX")
  defer "rm -f -- $(printf '%q' "$seed_tmp")"
  # SERVER_NAME (default SOL, the one live server; LUNA and TERRA are named the same way) is what the launcher's list shows.
  [[ ${SERVER_NAME:-SOL} =~ ^[A-Za-z0-9][A-Za-z0-9\ ._-]{0,39}$ ]] || die "SERVER_NAME '${SERVER_NAME:-}' is not a plain server name (letters, digits, space . _ -)"
  sed -e 's#"bansFile": "bans.json"#"bansFile": "data/bans.json"#' -e "s#\"name\": \"GAGE Server\"#\"name\": \"${SERVER_NAME:-SOL}\"#" \
    "$GAGE_SRC/apps/server/server.json.example" > "$seed_tmp"
  [ "$(ops_js json-get "$seed_tmp" bansFile)" = data/bans.json ] || die "could not point bansFile at data/bans.json in the seeded config (did the example change?)"
  [ "$(ops_js json-get "$seed_tmp" name)" = "${SERVER_NAME:-SOL}" ] || die "could not set the server name in the seeded config (did the example's \"name\" change?)"
  chmod 640 "$seed_tmp"
  chgrp gage "$seed_tmp" 2>/dev/null || warn "no 'gage' group yet (bootstrap-vps.sh creates it): $CONF stays root:root"
  mv -T -- "$seed_tmp" "$CONF"
fi
install -d -m 750 -o gage -g gage "$SRV/data" 2>/dev/null || install -d -m 750 "$SRV/data"

if [ "$BUILD_ONLY" = 1 ]; then
  log "--build-only: staged as $STAMP, not activated. Activate with: $0 --activate $STAMP"
  exit 0
fi

activate_release "$STAMP"
prune
log "done: release $STAMP is live"
