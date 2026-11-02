#!/usr/bin/env bash
# Release the client AND the game server together, so they always move in lockstep and players are forced onto the
# matching launcher as fast as possible. Run on the VPS as root from the source tree (/opt/gage/src).
#
#   release.sh <version> --notes TEXT [--countdown SECONDS] [--jobs N] [--dry-run]
#
#   <version>        x.y.z. Must equal kClientVersion in apps/launcher/gage_gui.cpp (refused otherwise).
#   --notes TEXT     release notes the launcher shows (max 2000 characters). Required.
#   --countdown S    seconds between publishing the client and restarting the server (default 60, 0..600). The
#                    players on the server are told through the admin channel (`say`) at the start, at 30 s and at
#                    10 s (the marks that fit), then the server restarts. 0 = restart at once. No players online =
#                    no countdown.
#   --jobs N         parallel compile jobs for both builds.
#   --dry-run        run every sub-script's own --dry-run, print the rest of the plan, change nothing. Exit 1 if a real
#                    run would stop.
#
# Steps, in order. The first failure stops the run and says what is live and what is not. Everything that can fail
# for lack of a build, a test or a canary comes BEFORE the first live change (the publish), so a broken build never
# leaves players forced onto a client whose server does not exist.
#   1 build-client.sh                          launcher, DLLs, selftests under wine (gate tests), installer
#   2 deploy-server.sh --build-only            server build, netcode_test and server_test, canary, staged release;
#                                              the stamp is read from its `RELEASE_STAMP=<stamp>` line and checked
#                                              against /opt/gage/server/releases
#   3 publish-client.sh <version> --mandatory --notes TEXT      LIVE: launchers see it within 2 minutes and update
#   4 countdown                                the players hear it through the admin channel (say), then wait
#   5 deploy-server.sh --activate <stamp> --allow-players --no-announce      LIVE: the server restarts on the new release
#   6 deploy-server.sh --status                 what runs now
#   7 UDP query                                 must report the protocol compiled from libs/protocol/protocol.h
# Logs: /opt/gage/logs/release/<UTC stamp>-<step>.log (whole, never truncated). One run at a time (lock `release`).
# Last line of a good run: `RELEASE_RESULT=ok version=<v> stamp=<stamp> protocol=<n>`.
#
# Exit codes: 0 ok | 1 failure (build, test, canary, publish, a failed check) | 2 usage
#             | 4 the new server release failed its health check and was rolled back (the client IS published)
#             | 5 it failed and the rollback failed too (stop, tell GI) | 6 something else answers on the game port
#             | 75 another release (or build, publish, deploy) is running
set -euo pipefail

SCRIPT_NAME=release
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }

VERSION=''; NOTES=''; NOTES_SET=0; COUNTDOWN=60; JOBS=''
while [ $# -gt 0 ]; do
  case $1 in
    --notes) [ $# -ge 2 ] || usage_die "--notes needs a value"; NOTES=$2; NOTES_SET=1; shift 2 ;;
    --countdown) [ $# -ge 2 ] && is_uint "$2" && [ "$2" -le 600 ] || usage_die "--countdown needs seconds (0..600)"; COUNTDOWN=$2; shift 2 ;;
    --jobs) [ $# -ge 2 ] && is_uint "$2" && [ "$2" -ge 1 ] || usage_die "--jobs needs a positive number"; JOBS=$2; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) usage_die "unknown option $1" ;;
    *) [ -z "$VERSION" ] || usage_die "more than one version given"; VERSION=$1; shift ;;
  esac
done
[ -n "$VERSION" ] || usage_die "version required (release.sh 0.4.3 --notes \"...\")"
[[ $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || usage_die "version '$VERSION' must be x.y.z"
[ "$NOTES_SET" = 1 ] && [ -n "$NOTES" ] || usage_die "--notes TEXT is required (what changed, one or two sentences: the launcher shows it)"
[ ${#NOTES} -le 2000 ] || usage_die "notes are ${#NOTES} characters, the limit is 2000"

deploy_config
SRV=$GAGE_SERVER_DIR
REL=$SRV/releases
CONF=$SRV/server.json
ENVF="${GAGE_SERVER_ENV_FILE:-/etc/gage/server.env}"
LOGDIR=$GAGE_OPT/logs/release
RSTAMP=$(date -u +%Y%m%dT%H%M%SZ)
BUILD_SH=$DEPLOY_DIR/build-client.sh
PUBLISH_SH=$DEPLOY_DIR/publish-client.sh
SERVER_SH=$DEPLOY_DIR/deploy-server.sh
PROTOCOL_H=$GAGE_SRC/libs/protocol/protocol.h

PROBLEMS=0
# fail_check MESSAGE: stops a real run; in a dry run it is counted and the run goes on.
fail_check() {
  if [ "$DRY_RUN" = 1 ]; then warn "dry-run: a real run would stop here: $1"; PROBLEMS=$((PROBLEMS + 1))
  else die "$1"; fi
}

log "release $VERSION | countdown ${COUNTDOWN}s | jobs ${JOBS:-auto} | $([ "$DRY_RUN" = 1 ] && echo DRY RUN || echo real run)"
acquire_lock release
if [ "$DRY_RUN" = 0 ]; then
  [ "$(id -u)" = 0 ] || warn "not root: the scripts this runs are meant to run as root on the VPS"
  mkdir -p "$LOGDIR" || die "cannot create $LOGDIR"
fi

# ---- preflight: cheap checks first, before anything is built ---------------------------------------------------------
need_cmd node
for f in "$BUILD_SH" "$PUBLISH_SH" "$SERVER_SH"; do
  [ -f "$f" ] || fail_check "missing $f"
done
SRC_VERSION=$(client_version)
[ "$VERSION" = "$SRC_VERSION" ] || fail_check "version $VERSION is not kClientVersion ($SRC_VERSION) in apps/launcher/gage_gui.cpp: the installer would carry $SRC_VERSION. Bump it (and re-pack), or give $SRC_VERSION"
# The protocol this source speaks: the UDP query of the new server must report exactly this when the release is live.
EXPECTED_PROTO=''
if [ -f "$PROTOCOL_H" ]; then
  EXPECTED_PROTO=$(sed -n 's/^[[:space:]]*constexpr uint16_t kVersion = \([0-9][0-9]*\);.*/\1/p' "$PROTOCOL_H" | head -n 1)
fi
[[ $EXPECTED_PROTO =~ ^[0-9]+$ ]] || fail_check "cannot read gage::proto::kVersion from $PROTOCOL_H (got '$EXPECTED_PROTO')"
log "client $VERSION, protocol v${EXPECTED_PROTO:-?}, server dir $SRV"

# run_step NAME CMD...: the whole output goes to the screen and to $LOGDIR/<stamp>-NAME.log; returns the command's
# exit code (the pipe's own status is read from PIPESTATUS, not from tee).
run_step() {
  local name=$1 lf rc; shift
  lf="$LOGDIR/$RSTAMP-$name.log"
  log "== $name: $* (log: $lf)"
  if [ "$DRY_RUN" = 1 ]; then
    # a dry run writes no log; the sub-script's own --dry-run is on the command line
    set +e; "$@" 2>&1; rc=$?; set -e
  else
    set +e; "$@" 2>&1 | tee "$lf"; rc=${PIPESTATUS[0]}; set -e
  fi
  return "$rc"
}

# stop_or_note NAME RC STATE: a failed step ends a real run (exit RC if it is one of the documented codes, else 1) and
# says what is live; in a dry run it is counted and the run goes on.
stop_or_note() {
  local name=$1 rc=$2 state=$3 code=1
  case $rc in 4|5|6|75) code=$rc ;; esac
  if [ "$DRY_RUN" = 1 ]; then
    warn "dry-run: step '$name' reported exit $rc; a real run would stop there"
    PROBLEMS=$((PROBLEMS + 1))
    return 0
  fi
  warn "step '$name' failed with exit $rc. Whole log: $LOGDIR/$RSTAMP-$name.log"
  warn "STATE: $state"
  exit "$code"
}

# ---- 1 build the client --------------------------------------------------------------------------------------------
BC=("$BUILD_SH")
[ -z "$JOBS" ] || BC+=(--jobs "$JOBS")
[ "$DRY_RUN" = 0 ] || BC+=(--dry-run)
rc=0; run_step build-client "${BC[@]}" || rc=$?
[ "$rc" -eq 0 ] || stop_or_note build-client "$rc" "nothing live changed. Fix the build and run release.sh again."

# ---- 2 build, test and stage the server (no live change) ---------------------------------------------------------------
SS=("$SERVER_SH" --build-only)
[ -z "$JOBS" ] || SS+=(--jobs "$JOBS")
[ "$DRY_RUN" = 0 ] || SS+=(--dry-run)
before=''
[ "$DRY_RUN" = 1 ] || before=$(find -H "$REL" -mindepth 1 -maxdepth 1 -type d -name '2*Z' -printf '%f\n' 2>/dev/null | sort || true)
rc=0; run_step stage-server "${SS[@]}" || rc=$?
[ "$rc" -eq 0 ] || stop_or_note stage-server "$rc" "nothing live changed (the client is built, not published). Fix the server build and run release.sh again."

# Capture the stamp: the line deploy-server.sh prints, cross-checked against the releases directory (a stamp that
# is not there, or not executable, is not a release).
STAMP=''
if [ "$DRY_RUN" = 0 ]; then
  STAMP=$(sed -n 's/^RELEASE_STAMP=\([0-9]\{8\}T[0-9]\{6\}Z\)$/\1/p' "$LOGDIR/$RSTAMP-stage-server.log" | tail -n 1)
  if [ -z "$STAMP" ]; then
    # No such line (an older deploy-server.sh): the one release directory that appeared during the step.
    after=$(find -H "$REL" -mindepth 1 -maxdepth 1 -type d -name '2*Z' -printf '%f\n' 2>/dev/null | sort || true)
    new=$(comm -13 <(printf '%s\n' "$before") <(printf '%s\n' "$after") | sed '/^$/d')
    [ "$(printf '%s\n' "$new" | sed '/^$/d' | wc -l)" = 1 ] || die "cannot tell which release deploy-server.sh staged: no RELEASE_STAMP line and ${new:-no} new directories in $REL"
    STAMP=$new
    warn "no RELEASE_STAMP line in the output; using the one new release directory: $STAMP"
  fi
  [[ $STAMP =~ ^[0-9]{8}T[0-9]{6}Z$ ]] || die "the staged release stamp '$STAMP' is malformed"
  [ -x "$REL/$STAMP/gage-server" ] || die "release $STAMP is not staged: $REL/$STAMP/gage-server is missing or not executable"
  log "server release staged: $STAMP"
else
  STAMP='<stamp printed by step 2>'
fi

# ---- 3 publish the client, mandatory (the first live change) ---------------------------------------------------------
PC=("$PUBLISH_SH" "$VERSION" --mandatory --notes "$NOTES")
[ "$DRY_RUN" = 0 ] || PC+=(--dry-run)
rc=0; run_step publish-client "${PC[@]}" || rc=$?
[ "$rc" -eq 0 ] || stop_or_note publish-client "$rc" "the previous client manifest is still served (publish-client.sh switches version.json last); server release $STAMP is staged, NOT active. Fix and run release.sh again, or activate: $SERVER_SH --activate $STAMP --allow-players"

# ---- 4 countdown ---------------------------------------------------------------------------------------------------
# Marks: the start, then 30 s and 10 s when they fit. Each mark is one `say` through the admin channel (the same
# mechanism deploy-server.sh's own warning uses); the wait is wall-clock `sleep`.
ADMIN_PORT=0; ADMIN_TOKEN=''
[ -f "$CONF" ] && ADMIN_PORT=$(ops_js json-get "$CONF" adminPort 0)
is_uint "$ADMIN_PORT" || ADMIN_PORT=0
ADMIN_TOKEN=$(admin_token)

say_players() { # say_players TEXT: one line to every player; a failure is a warning, never a stop
  if [ "$ADMIN_PORT" = 0 ] || [ -z "$ADMIN_TOKEN" ]; then return 0; fi
  GAGE_ADMIN_TOKEN=$ADMIN_TOKEN ops_js admin-cmd "$ADMIN_PORT" say "$1" >/dev/null \
    || warn "the admin channel did not accept the announcement; players get no in-game notice"
}

marks=("$COUNTDOWN")
for m in 30 10; do [ "$m" -lt "$COUNTDOWN" ] && marks+=("$m"); done

if [ "$DRY_RUN" = 1 ]; then
  if [ "$COUNTDOWN" -eq 0 ]; then
    log "== countdown (plan): --countdown 0: no announcement, restart at once after the publish"
  else
    log "== countdown (plan): publish -> wait ${COUNTDOWN}s -> restart. Announcements at: ${marks[*]} seconds before the restart:"
    for m in "${marks[@]}"; do
      log "[dry-run]   admin-cmd ${ADMIN_PORT} say Solar RP $VERSION: the server restarts in $m seconds ..."
    done
  fi
  [ -f "$CONF" ] && log "[dry-run]   players online now: $(players_online)" || log "[dry-run]   no $CONF here"
  [ "$ADMIN_PORT" != 0 ] && [ -n "$ADMIN_TOKEN" ] || log "[dry-run]   NOTE: no admin channel (adminPort in $CONF + GAGE_ADMIN_TOKEN in $ENVF): players would get no in-game notice, the wait still happens"
else
  log "== countdown: ${COUNTDOWN}s"
  players=$(players_online)
  if [ "$COUNTDOWN" -eq 0 ]; then
    log "countdown 0: restarting at once"
  elif [ "$players" = 0 ]; then
    log "no players online: skipping the ${COUNTDOWN}s countdown"
  else
    log "players online: $players"
    if [ "$ADMIN_PORT" = 0 ] || [ -z "$ADMIN_TOKEN" ]; then
      warn "no admin channel (adminPort in $CONF, GAGE_ADMIN_TOKEN in $ENVF): players get no in-game notice; waiting the ${COUNTDOWN}s anyway so launchers can update"
    fi
    remaining=$COUNTDOWN
    for m in "${marks[@]}"; do
      if [ "$remaining" -gt "$m" ]; then
        log "waiting $((remaining - m))s (wall clock)"
        sleep $((remaining - m))
        remaining=$m
      fi
      log "announce: restart in ${m}s"
      say_players "Solar RP $VERSION: the server restarts in $m seconds for an update. Close the game now; Solar Launcher installs the update, then rejoin."
    done
    log "waiting ${remaining}s (wall clock), then restarting"
    sleep "$remaining"
  fi
fi

# ---- 5 activate the staged server release (the second live change) ---------------------------------------------------------
# --allow-players: the countdown above was the warning. --no-announce: do not repeat it (and do not wait again).
if [ "$DRY_RUN" = 1 ]; then
  log "== activate-server (plan): $SERVER_SH --activate $STAMP --allow-players --no-announce"
else
  rc=0; run_step activate-server "$SERVER_SH" --activate "$STAMP" --allow-players --no-announce || rc=$?
  [ "$rc" -eq 0 ] || stop_or_note activate-server "$rc" "client $VERSION IS published (mandatory), but the server is NOT on release $STAMP (exit $rc: 4 = rolled back to the old release and serving it; 5 = nothing healthy, tell GI; 6 = something else holds the game port). Players with the new launcher are told 'This server has not been updated yet'. Fix the cause, then: $SERVER_SH --activate $STAMP --allow-players"
fi

# ---- 6 status ------------------------------------------------------------------------------------------------------
if [ "$DRY_RUN" = 1 ]; then
  log "== status (plan): $SERVER_SH --status"
else
  rc=0; run_step status "$SERVER_SH" --status || rc=$?
  [ "$rc" -eq 0 ] || stop_or_note status "$rc" "client $VERSION is published and server release $STAMP was activated, but --status failed; check: $SERVER_SH --status"
fi

# ---- 7 the live server reports the new protocol ---------------------------------------------------------------------------
if [ "$DRY_RUN" = 1 ]; then
  log "== verify (plan): UDP query on 127.0.0.1:$([ -f "$CONF" ] && game_port || echo 24800) must report protocol $EXPECTED_PROTO"
  if [ "$PROBLEMS" -gt 0 ]; then
    warn "dry-run: $PROBLEMS problem(s) a real run would stop on (above)"
    exit 1
  fi
  log "dry-run complete: nothing was built, published or restarted"
  exit 0
fi

PORT=$(game_port)
deadline=$((SECONDS + 30))
got=''; last=''
while [ "$SECONDS" -lt "$deadline" ]; do
  if last=$(ops_js udp-query 127.0.0.1 "$PORT" --timeout-ms 1500 --attempts 2 2>&1); then
    got=${last#*\"protocol\":}; got=${got%%,*}
    [ "$got" = "$EXPECTED_PROTO" ] && break
  fi
  sleep 1
done
if [ "$got" != "$EXPECTED_PROTO" ]; then
  warn "STATE: client $VERSION is published (mandatory) and release $STAMP was activated, but the UDP query on 127.0.0.1:$PORT did not report protocol $EXPECTED_PROTO within 30 s (last answer: ${last:-none}). Look at: journalctl -u gage-server -n 100 ; $SERVER_SH --status"
  die "the live server does not report protocol $EXPECTED_PROTO" 1
fi
now_players=${last#*\"players\":}; now_players=${now_players%%,*}
log "verified: the live server reports protocol $got (players: $now_players)"
log "release $VERSION is live: client published (mandatory), server release $STAMP active"
printf 'RELEASE_RESULT=ok version=%s stamp=%s protocol=%s\n' "$VERSION" "$STAMP" "$got"
