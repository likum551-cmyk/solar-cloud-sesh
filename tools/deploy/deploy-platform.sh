#!/usr/bin/env bash
# Deploy the Solar RP platform API (apps/platform: accounts, login, join tokens, roles, shop) on the VPS: PostgreSQL,
# its role and database, the settings file, a tested build in a new release folder, the migrations, the systemd unit
# gage-platform and Caddy's /api route. Run on the VPS as root, after tools/deploy/unpack-source.sh.
#
#   deploy-platform.sh [--skip-build] [--keep N] [--no-caddy] [--no-http-check] [--dry-run]
#   deploy-platform.sh --rollback [--no-http-check] [--dry-run]
#
#   --skip-build     use apps/platform/dist as it is (it must exist); default: npm ci, npm test, npm run build there.
#   --rollback       switch back to the newest release older than the live one and restart (no build, no migration;
#                    the database schema is NOT rolled back: the dump taken before each migration is in backups/).
#   --keep N         release folders to keep, the live one included (default 3, minimum 2 so a rollback target exists).
#   --no-caddy       leave /etc/caddy/Caddyfile alone (default: install tools/deploy/Caddyfile when it differs,
#                    after `caddy validate` passed on a copy, then reload Caddy).
#   --no-http-check  skip the check through Caddy (the direct check on 127.0.0.1:8787 always runs).
#   --dry-run        run every read-only check, print every change, change nothing. Exit 1 if a real run would stop.
#
# Layout: releases in /opt/gage/platform/releases/<UTC stamp>/ (dist, drizzle, production node_modules; root-owned),
# the live one is the symlink /opt/gage/platform/current; database dumps in /opt/gage/platform/backups/ (root, 0700).
# Settings: /opt/gage/platform.env (owner gage-platform, 0600), written ONCE from tools/deploy/platform.env.example
# with a random database password and webhook secret that are never printed. Later runs never change a value in it:
# they only append the template's missing non-secret keys and name them. Service user: gage-platform (not gage,
# which runs the game server: a process can read the environment of another process of the same user).
#
# Order of effects (a failure before step 8 leaves the running release untouched):
#   1 preflight        2 PostgreSQL (apt install postgresql if missing; started and enabled)
#   3 user gage-platform, settings file    4 role and database gage_platform (created when missing)
#   5 build (npm ci, npm test, npm run build)      6 new release (copy, npm ci --omit=dev)
#   7 dump the database (when it has a schema), then migrate with the new release
#   8 unit, switch the symlink, restart, check 127.0.0.1:8787/health (a failure switches back on its own)
#   9 Caddyfile (validated copy first) and reload    10 check https://solar-rp.com/api/health through Caddy
#   11 keep the newest N releases
#
# First admin (no public way exists). Register the account in the launcher or on the site, then on the VPS as root:
#   node --env-file=/opt/gage/platform.env /opt/gage/platform/current/dist/scripts/grant-role.js --user <name-or-email> --role admin --dry-run
#   node --env-file=/opt/gage/platform.env /opt/gage/platform/current/dist/scripts/grant-role.js --user <name-or-email> --role admin
# (the compiled form of `npm run admin:grant-role`: the release has no tsx. Audited as cli:root; `--remove` takes a role
# away; the last admin cannot be removed.) A game server's credentials: dist/scripts/create-server.js the same way.
#
# Exit codes: 0 ok, 1 failure, 2 usage, 4 the new release failed its health check and the previous one is live again,
#             5 failed and the switch back failed too, 75 another deploy-platform is running.
set -euo pipefail

SCRIPT_NAME=deploy-platform
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }

SKIP_BUILD=0
ROLLBACK=0
KEEP=3
CADDY=1
HTTP_CHECK=1
while [ $# -gt 0 ]; do
  case $1 in
    --skip-build) SKIP_BUILD=1; shift ;;
    --rollback) ROLLBACK=1; shift ;;
    --keep) [ $# -ge 2 ] || usage_die "--keep needs a number"; KEEP=$2; shift 2 ;;
    --no-caddy) CADDY=0; shift ;;
    --no-http-check) HTTP_CHECK=0; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage_die "unknown option $1" ;;
  esac
done
is_uint "$KEEP" && [ "$KEEP" -ge 2 ] || usage_die "--keep must be a number >= 2"
[ "$ROLLBACK" = 0 ] || [ "$SKIP_BUILD" = 0 ] || usage_die "--rollback builds nothing; drop --skip-build"

deploy_config
acquire_lock deploy-platform

FAILED=0
# fail_check MESSAGE: stops a real run; in a dry run it is counted and the run goes on.
fail_check() {
  if [ "$DRY_RUN" = 1 ]; then warn "would stop: $1"; FAILED=1; else die "$1"; fi
}

PLATFORM_SRC=$GAGE_SRC/apps/platform
DIST=$PLATFORM_SRC/dist
PLATFORM_DIR=$GAGE_OPT/platform
RELEASES=$PLATFORM_DIR/releases
BACKUPS=$PLATFORM_DIR/backups
LIVE=$PLATFORM_DIR/current
ENV_FILE=${PLATFORM_ENV_FILE:-$GAGE_OPT/platform.env}
TEMPLATE=$DEPLOY_DIR/platform.env.example
UNIT=gage-platform
UNIT_SRC=$DEPLOY_DIR/gage-platform.service
# The three system paths can be pointed elsewhere only for a test harness; the defaults are the real ones.
: "${GAGE_SYSTEMD_DIR:=/etc/systemd/system}"
: "${GAGE_CADDYFILE:=/etc/caddy/Caddyfile}"
# the node the unit's ExecStart names
: "${NODE_BIN:=/usr/bin/node}"
UNIT_DST=$GAGE_SYSTEMD_DIR/gage-platform.service
SVC_USER=gage-platform
PG_ROLE=gage_platform
PG_DB=gage_platform
# Caddy proxies exactly this port (tools/deploy/Caddyfile, gage_api).
API_PORT=8787
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
APT_ENV=(env DEBIAN_FRONTEND=noninteractive)
# Non-secret template keys a later run appends when missing. DATABASE_URL and FAKE_WEBHOOK_SECRET are secrets, and the
# payment lines are a decision: those are only reported.
ADDABLE_KEYS=(NODE_ENV PUBLIC_API_URL HOST PORT INTERNAL_HOST INTERNAL_PORT TRUST_PROXY CORS_ORIGINS EMAIL_DRIVER
              REQUIRE_VERIFIED_EMAIL EMAIL_FROM EMAIL_LINK_BASE_URL LOG_LEVEL SHUTDOWN_GRACE_SECONDS)
REPORT_ONLY_KEYS=(DATABASE_URL PAYMENT_PROVIDER ALLOW_FAKE_PAYMENTS_IN_PRODUCTION FAKE_WEBHOOK_SECRET)

# ---- helpers ------------------------------------------------------------------------------------------------------
live_target() { [ -L "$LIVE" ] && readlink -f -- "$LIVE" || true; }
# the live release's NAME: compare names, never paths (readlink -f canonicalises, $RELEASES may not be canonical)
live_name() { local t; t=$(live_target); [ -n "$t" ] && basename -- "$t" || true; }
# releases, oldest first (the names are UTC stamps, so a name sort is a time sort)
list_releases() { [ -d "$RELEASES" ] && find "$RELEASES" -mindepth 1 -maxdepth 1 -type d ! -name '.*' -printf '%f\n' | sort || true; }

switch_to() {
  local target=$1 tmp="$PLATFORM_DIR/.current.new.$$"
  run ln -sfn -- "$target" "$tmp"
  run mv -Tf -- "$tmp" "$LIVE"
  log "live platform -> $target"
}

# install_file SRC DST MODE: copy when different; the previous file is kept as DST.gage-bak-<stamp> (bootstrap-vps.sh's contract).
install_file() {
  local src=$1 dst=$2 mode=$3
  [ -f "$src" ] || die "missing $src"
  if [ -f "$dst" ] && cmp -s "$src" "$dst"; then log "up to date: $dst"; return 1; fi
  if [ -f "$dst" ]; then
    [ "$DRY_RUN" = 0 ] || { log "[dry-run] $dst would change:"; diff -u -- "$dst" "$src" | sed 's/^/    /' || true; }
    run cp -a -- "$dst" "$dst.gage-bak-$STAMP"
  fi
  run install -D -m "$mode" -- "$src" "$dst"
  if [ "$DRY_RUN" = 1 ]; then log "[dry-run] would install $dst"; else log "installed $dst"; fi
  return 0
}

# env_has KEY: the settings file assigns KEY (a commented line does not count).
env_has() { [ -f "$ENV_FILE" ] && grep -q -E "^$1=" -- "$ENV_FILE"; }
# env_get FILE KEY: the value of the LAST assignment (what systemd and node use), quotes and CR removed. Callers never
# print the value of a secret.
env_get() {
  local v
  v=$(sed -n "s/^$2=//p" -- "$1" | tail -n 1 | tr -d '\r')
  case $v in \"*\") v=${v#\"}; v=${v%\"} ;; \'*\') v=${v#\'}; v=${v%\'} ;; esac
  printf '%s' "$v"
}
# effective KEY: the value the service will see after this run (the file's, else the template's when it would be added).
effective() {
  if env_has "$1"; then env_get "$ENV_FILE" "$1"; else env_get "$TEMPLATE" "$1"; fi
}
is_loopback() { case $1 in 127.*|::1|'[::1]'|localhost) return 0 ;; *) return 1 ;; esac; }

pg_installed() { command -v psql >/dev/null 2>&1 && command -v pg_isready >/dev/null 2>&1; }
# pg_psql [DB] ARGS...: psql as the postgres superuser over the local socket (peer auth). cd / first: postgres cannot
# enter root's working directory.
pg_psql() {
  local db=$1; shift
  ( cd / && runuser -u postgres -- psql -X -q -v ON_ERROR_STOP=1 -d "$db" "$@" )
}
# pg_value DB SQL: one value, no decoration. Never used with a secret in SQL.
pg_value() { pg_psql "$1" -tA -c "$2"; }
# pg_secret_sql: SQL on stdin that carries a secret. The secret never appears in a process argument list (printf is a
# builtin), and the session stops PostgreSQL from writing the statement text to its log, even on an error.
pg_secret_sql() {
  { printf "SET log_statement = 'none';\nSET log_min_error_statement = 'panic';\n"; cat; } | pg_psql postgres
}

# health_local: the new process answers /health with "ok" and runs from the expected release (its working directory).
# The X-Forwarded-For header keeps TRUST_PROXY=1 from logging a missing-header warning for this direct call.
health_local() {
  local want=$1 deadline=$((SECONDS + 40)) body='' pid cwd
  while [ "$SECONDS" -lt "$deadline" ]; do
    if body=$(curl -fsS --max-time 3 -H 'X-Forwarded-For: 127.0.0.1' "http://127.0.0.1:$API_PORT/health" 2>/dev/null) \
       && grep -q '"status":"ok"' <<<"$body"; then
      pid=$(systemctl show -p MainPID --value "$UNIT" 2>/dev/null || echo 0)
      cwd=$( [ "${pid:-0}" != 0 ] && readlink -f "/proc/$pid/cwd" 2>/dev/null || true)
      if [ -n "$cwd" ] && [ "$(basename -- "$cwd")" = "$(basename -- "$want")" ]; then
        log "health ok: http://127.0.0.1:$API_PORT/health -> $body (pid $pid runs $(basename -- "$cwd"))"
        return 0
      fi
    fi
    sleep 1
  done
  warn "no healthy answer from release $(basename -- "$want") within 40 s (last body: '${body:-none}')"
  warn "read the FIRST error line: journalctl -u $UNIT -n 60 --no-pager"
  return 1
}

# internal_loopback_only: the game-server listener must answer on loopback only.
internal_loopback_only() {
  local port addrs a
  port=$(effective INTERNAL_PORT); port=${port:-8788}
  command -v ss >/dev/null 2>&1 || { warn "ss not available: cannot check where :$port listens"; return 0; }
  addrs=$(ss -ltnH "sport = :$port" 2>/dev/null | awk '{print $4}' || true)
  [ -n "$addrs" ] || { warn "nothing listens on :$port (INTERNAL_PORT): the game servers cannot reach the internal API"; return 0; }
  for a in $addrs; do
    case $a in 127.*:"$port"|'[::1]':"$port") ;; *) die "the internal listener answers on $a: it must be loopback only (INTERNAL_HOST)" ;; esac
  done
  log "internal listener loopback only: $(tr '\n' ' ' <<<"$addrs")"
}

http_check() {
  [ "$HTTP_CHECK" = 1 ] || { log "HTTP check through Caddy skipped (--no-http-check)"; return 0; }
  [ "$DRY_RUN" = 0 ] || { log "[dry-run] would GET /api/health through Caddy, expect 404 for /api/internal/*, and check the journal for a missing X-Forwarded-For"; return 0; }
  local base t0 body code n
  local -a res=()
  # The API is served over HTTPS on the domain only (the Caddyfile's bare-IP :80 block has no /api: passwords in clear).
  [ -n "$DOMAIN" ] || { log "HTTP check through Caddy skipped: DOMAIN is empty and the API is not proxied over plain HTTP on the bare IP"; return 0; }
  base="https://$DOMAIN/api"; res=(--resolve "$DOMAIN:443:127.0.0.1")
  t0=$(date +%s)
  body=$(curl -fsS --max-time 15 "${res[@]}" "$base/health") || die "GET $base/health through Caddy failed (journalctl -u caddy -n 50; is the /api route in /etc/caddy/Caddyfile?)"
  grep -q '"status":"ok"' <<<"$body" || die "GET $base/health answered without \"status\":\"ok\": '$body' (is Caddy serving the website there instead?)"
  # The game-server API must not be reachable through the proxy: the public listener does not mount /internal/*.
  code=$(curl -sS --max-time 15 -o /dev/null -w '%{http_code}' -X POST "${res[@]}" "$base/internal/game/verify" || true)
  [ "$code" = 404 ] || die "POST $base/internal/game/verify answered $code, expected 404: the internal API must never be proxied"
  # Caddy must pass the client address: the platform warns on every proxied request that comes without it.
  sleep 1
  n=$(journalctl -u "$UNIT" --since "@$t0" --no-pager -o cat 2>/dev/null | grep -c 'X-Forwarded-For is missing' || true)
  [ "${n:-0}" = 0 ] || die "the platform logged 'X-Forwarded-For is missing' for the proxied request: rate limits would see every client as 127.0.0.1"
  log "HTTP check ok: $base/health -> $body; /api/internal/* -> 404; X-Forwarded-For arrives"
}

# ---- rollback -----------------------------------------------------------------------------------------------------
if [ "$ROLLBACK" = 1 ]; then
  [ "$DRY_RUN" = 1 ] || [ "$(id -u)" = 0 ] || die "run as root"
  cur=$(live_name)
  [ -n "$cur" ] || die "no live platform to roll back from ($LIVE is not a symlink)"
  prev=''; found=0
  while IFS= read -r r; do
    if [ "$r" = "$cur" ]; then found=1; break; fi
    prev=$r
  done < <(list_releases)
  [ "$found" = 1 ] || die "the live platform points at $(live_target), which is not a release in $RELEASES; fix the symlink by hand"
  [ -n "$prev" ] || die "no release older than $cur in $RELEASES"
  log "rolling back: $cur -> $prev (code only; the database keeps the newer schema, dumps are in $BACKUPS)"
  switch_to "$RELEASES/$prev"
  run systemctl restart "$UNIT"
  if [ "$DRY_RUN" = 0 ]; then health_local "$RELEASES/$prev" || die "the rolled-back release $prev is not healthy either" 5; fi
  http_check
  log "done (rollback). The newer release stays in $RELEASES until a deploy prunes it."
  exit 0
fi

# ---- 1 preflight --------------------------------------------------------------------------------------------------
log "== 1/11 preflight ($([ "$DRY_RUN" = 1 ] && echo DRY RUN || echo real run))"
if [ "$DRY_RUN" = 0 ]; then
  [ "$(id -u)" = 0 ] || die "run as root"
fi
[ -f "$PLATFORM_SRC/package.json" ] && [ -f "$PLATFORM_SRC/package-lock.json" ] || die "$PLATFORM_SRC is not the platform source (no package.json / package-lock.json). Unpack the source first."
[ -f "$TEMPLATE" ] || die "missing $TEMPLATE"
[ -f "$UNIT_SRC" ] || die "missing $UNIT_SRC"
need_cmd node npm openssl curl runuser systemctl cmp
if [ -x "$NODE_BIN" ]; then
  nv=$("$NODE_BIN" -p 'process.versions.node')
  [ "$(ver_cmp "$nv" 22.12.0)" -ge 0 ] || fail_check "Node $nv is too old: apps/platform needs >= 22.12.0 (bootstrap-vps.sh installs Node 22)"
  log "node $nv ($NODE_BIN)"
else
  fail_check "$NODE_BIN is missing: gage-platform.service starts the platform with it (bootstrap-vps.sh installs Node 22)"
fi
getent passwd gage >/dev/null 2>&1 || warn "user gage does not exist: has bootstrap-vps.sh run on this machine?"

# ---- 2 PostgreSQL -------------------------------------------------------------------------------------------------
log "== 2/11 PostgreSQL"
PG_READY=0
if pg_installed; then
  log "PostgreSQL client tools present ($(psql --version 2>/dev/null | head -n 1))"
else
  log "PostgreSQL is not installed: installing Ubuntu's postgresql package"
  run "${APT_ENV[@]}" apt-get update
  run "${APT_ENV[@]}" apt-get install -y -o Dpkg::Options::=--force-confold postgresql
fi
if [ "$DRY_RUN" = 1 ] && ! pg_installed; then
  log "[dry-run] systemctl enable --now postgresql; wait for pg_isready -h 127.0.0.1"
else
  run systemctl enable --now postgresql
  if [ "$DRY_RUN" = 1 ] && ! systemctl is-active --quiet postgresql; then
    log "[dry-run] PostgreSQL is installed but not running; a real run starts it"
  else
    deadline=$((SECONDS + 30))
    until pg_isready -q -h 127.0.0.1 -p 5432; do
      [ "$SECONDS" -lt "$deadline" ] || die "PostgreSQL does not answer on 127.0.0.1:5432 after 30 s (journalctl -u postgresql -n 50)"
      sleep 1
    done
    PG_READY=1
    pv=$(pg_value postgres 'SHOW server_version_num')
    is_uint "$pv" && [ "$pv" -ge 150000 ] || die "PostgreSQL server_version_num '$pv': the platform needs 15 or newer"
    log "PostgreSQL $(pg_value postgres 'SHOW server_version') answers on 127.0.0.1:5432"
    la=$(pg_value postgres 'SHOW listen_addresses')
    [ "$la" = localhost ] || [ "$la" = 127.0.0.1 ] || warn "PostgreSQL listen_addresses is '$la' (Ubuntu's default is localhost); ufw keeps 5432 closed, but check it was meant"
  fi
fi

# ---- 3 user and settings file -------------------------------------------------------------------------------------
log "== 3/11 user $SVC_USER and $ENV_FILE"
if getent passwd "$SVC_USER" >/dev/null 2>&1; then log "user $SVC_USER exists"
else run adduser --system --group --no-create-home --home /nonexistent --shell /usr/sbin/nologin "$SVC_USER"; fi

DB_PW=''          # set only when this run creates the settings file or must create the role from it; never printed
NEW_ENV=0
if [ -e "$ENV_FILE" ]; then
  [ -f "$ENV_FILE" ] || die "$ENV_FILE exists and is not a regular file"
  log "kept $ENV_FILE (values untouched)"
  # Mode and owner: the file holds the database password.
  perm=$(stat -c '%a %U:%G' -- "$ENV_FILE")
  if [ "$perm" != "600 $SVC_USER:$SVC_USER" ]; then
    warn "$ENV_FILE is '$perm', expected '600 $SVC_USER:$SVC_USER': fixing mode and owner (not the content)"
    run chown "$SVC_USER:$SVC_USER" -- "$ENV_FILE"
    run chmod 600 -- "$ENV_FILE"
  fi
  missing=()
  for k in "${ADDABLE_KEYS[@]}"; do env_has "$k" || missing+=("$k"); done
  if [ ${#missing[@]} -gt 0 ]; then
    log "appending the missing non-secret keys with the template's values: ${missing[*]}"
    if [ "$DRY_RUN" = 0 ]; then
      { printf '\n# added by deploy-platform.sh at %s (values from tools/deploy/platform.env.example)\n' "$STAMP"
        for k in "${missing[@]}"; do grep -m 1 -E "^$k=" -- "$TEMPLATE"; done; } >> "$ENV_FILE"
    fi
  else
    log "every non-secret template key is present"
  fi
  for k in "${REPORT_ONLY_KEYS[@]}"; do env_has "$k" || warn "$ENV_FILE has no $k (not added: a secret or a decision); the platform may refuse to start"; done
  # Values that differ from the template are kept; name them (never the value: some are secrets).
  differ=()
  for k in "${ADDABLE_KEYS[@]}" PAYMENT_PROVIDER ALLOW_FAKE_PAYMENTS_IN_PRODUCTION; do
    env_has "$k" || continue
    [ "$(env_get "$ENV_FILE" "$k")" = "$(env_get "$TEMPLATE" "$k")" ] || differ+=("$k")
  done
  [ ${#differ[@]} -eq 0 ] || log "kept values that differ from the template: ${differ[*]}"
else
  NEW_ENV=1
  if [ "$DRY_RUN" = 1 ]; then
    log "[dry-run] create $ENV_FILE ($SVC_USER, 0600) from $(basename -- "$TEMPLATE") with a random DATABASE_URL password and FAKE_WEBHOOK_SECRET (not shown)"
  else
    DB_PW=$(openssl rand -hex 24)
    [[ $DB_PW =~ ^[0-9a-f]{48}$ ]] || die "could not generate a database password"
  fi
fi

# Rules the service depends on (checked on the values it will see after this run).
[ "$(effective NODE_ENV)" = production ] || fail_check "NODE_ENV in $ENV_FILE must be production"
[ "$(effective ENABLE_DEV_ROUTES)" != 1 ] || fail_check "ENABLE_DEV_ROUTES=1 in $ENV_FILE: never on this machine"
[ "$(effective PORT)" = "$API_PORT" ] || fail_check "PORT in $ENV_FILE must be $API_PORT: Caddy proxies /api/* to 127.0.0.1:$API_PORT"
is_loopback "$(effective INTERNAL_HOST)" || fail_check "INTERNAL_HOST in $ENV_FILE is not loopback: the game-server API must not be reachable from outside"
if [ "$(effective TRUST_PROXY)" = 1 ]; then
  is_loopback "$(effective HOST)" || fail_check "TRUST_PROXY=1 with HOST=$(effective HOST): any client could then write its own X-Forwarded-For. Keep HOST=127.0.0.1"
else
  warn "TRUST_PROXY is not 1: behind Caddy every client would be rate-limited and locked out as 127.0.0.1"
fi

# ---- 4 role and database ------------------------------------------------------------------------------------------
log "== 4/11 PostgreSQL role and database $PG_DB"
MANAGE_DB=1
if [ "$NEW_ENV" = 0 ] && env_has DATABASE_URL; then
  url=$(env_get "$ENV_FILE" DATABASE_URL)
  if [[ $url =~ ^postgres(ql)?://$PG_ROLE:([0-9A-Za-z]+)@(127\.0\.0\.1|localhost):5432/$PG_DB$ ]]; then
    url_pw=${BASH_REMATCH[2]}
  else
    MANAGE_DB=0
    warn "DATABASE_URL in $ENV_FILE is not postgres://$PG_ROLE:<password>@127.0.0.1:5432/$PG_DB: role and database are not managed by this script"
  fi
  unset url
fi
if [ "$MANAGE_DB" = 1 ]; then
  if [ "$PG_READY" = 0 ]; then
    log "[dry-run] create role $PG_ROLE (LOGIN, password from the settings file) and database $PG_DB owned by it, when missing"
  else
    role_exists=$(pg_value postgres "SELECT count(*) FROM pg_roles WHERE rolname = '$PG_ROLE'")
    if [ "$role_exists" = 1 ]; then
      log "role $PG_ROLE exists"
      if [ "$NEW_ENV" = 1 ]; then
        # A new settings file means a new password: the old one is not known any more.
        if [ "$DRY_RUN" = 1 ]; then log "[dry-run] set a new password on role $PG_ROLE (not shown)"
        else printf "ALTER ROLE %s WITH LOGIN PASSWORD '%s';\n" "$PG_ROLE" "$DB_PW" | pg_secret_sql; log "role $PG_ROLE: new password set (not shown)"; fi
      fi
    else
      [ "$NEW_ENV" = 1 ] || DB_PW=${url_pw:-}
      if [ "$DRY_RUN" = 1 ]; then log "[dry-run] create role $PG_ROLE (LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE, password not shown)"
      else
        [ -n "$DB_PW" ] || die "no password for the new role $PG_ROLE"
        printf "CREATE ROLE %s WITH LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE PASSWORD '%s';\n" "$PG_ROLE" "$DB_PW" | pg_secret_sql
        log "created role $PG_ROLE"
      fi
    fi
    db_owner=$(pg_value postgres "SELECT pg_get_userbyid(datdba) FROM pg_database WHERE datname = '$PG_DB'")
    if [ -z "$db_owner" ]; then
      run_sql="CREATE DATABASE $PG_DB OWNER $PG_ROLE ENCODING 'UTF8' TEMPLATE template0"
      if [ "$DRY_RUN" = 1 ]; then log "[dry-run] $run_sql"
      else pg_psql postgres -c "$run_sql"; log "created database $PG_DB (owner $PG_ROLE)"; fi
    elif [ "$db_owner" = "$PG_ROLE" ]; then
      log "database $PG_DB exists (owner $PG_ROLE)"
    else
      fail_check "database $PG_DB exists but is owned by '$db_owner', not $PG_ROLE: fix it by hand"
    fi
  fi
fi
unset url_pw

# The settings file is written only after the role has its password, so a failure above leaves no file that
# names a password the database does not know (the next run starts over).
if [ "$NEW_ENV" = 1 ] && [ "$DRY_RUN" = 0 ]; then
  [ -n "$DB_PW" ] || die "internal: no database password to write"
  hook=$(openssl rand -hex 32)
  [[ $hook =~ ^[0-9a-f]{64}$ ]] || die "could not generate FAKE_WEBHOOK_SECRET"
  tmp_env=$(mktemp "$GAGE_OPT/.platform.env.XXXXXX")
  defer "rm -f -- $(printf '%q' "$tmp_env")"
  {
    printf '# written by deploy-platform.sh at %s from tools/deploy/platform.env.example. Secrets: never print this file.\n' "$STAMP"
    while IFS= read -r line || [ -n "$line" ]; do
      case $line in
        DATABASE_URL=*) printf 'DATABASE_URL=postgres://%s:%s@127.0.0.1:5432/%s\n' "$PG_ROLE" "$DB_PW" "$PG_DB" ;;
        FAKE_WEBHOOK_SECRET=*) printf 'FAKE_WEBHOOK_SECRET=%s\n' "$hook" ;;
        *) printf '%s\n' "$line" ;;
      esac
    done < "$TEMPLATE"
  } > "$tmp_env"
  unset hook
  # assignments only: the template's comments mention the placeholder too
  ! grep -q -E '^[A-Za-z_][A-Za-z0-9_]*=.*CHANGE_ME' -- "$tmp_env" || die "internal: a CHANGE_ME placeholder survived in the new settings file"
  chown "$SVC_USER:$SVC_USER" -- "$tmp_env"
  chmod 600 -- "$tmp_env"
  mv -T -- "$tmp_env" "$ENV_FILE"
  log "created $ENV_FILE ($SVC_USER, 0600) with a fresh database password and webhook secret (not shown)"
fi
unset DB_PW

# ---- 5 build ------------------------------------------------------------------------------------------------------
if [ "$SKIP_BUILD" = 0 ]; then
  log "== 5/11 build ($PLATFORM_SRC)"
  run bash -c 'cd "$1" && npm ci --no-audit --no-fund && npm test && npm run build' _ "$PLATFORM_SRC" || die "platform build failed"
else
  log "== 5/11 build skipped (--skip-build): using $DIST as it is"
fi
for f in dist/src/index.js dist/scripts/migrate.js dist/scripts/grant-role.js drizzle/meta/_journal.json; do
  if [ ! -f "$PLATFORM_SRC/$f" ]; then
    if [ "$DRY_RUN" = 1 ] && [ "$SKIP_BUILD" = 0 ] && [ "${f%%/*}" = dist ]; then log "[dry-run] $PLATFORM_SRC/$f does not exist yet; the build would create it"
    else fail_check "$PLATFORM_SRC/$f is missing"; fi
  fi
done

# ---- 6 new release ------------------------------------------------------------------------------------------------
NEW=$RELEASES/$STAMP
INCOMING=$RELEASES/.incoming-$STAMP
log "== 6/11 release -> $NEW"
[ ! -e "$NEW" ] || fail_check "$NEW already exists (two deploys in one second?)"
run install -d -m 755 -- "$PLATFORM_DIR" "$RELEASES"
run install -d -m 700 -- "$BACKUPS"
run install -d -m 755 -- "$INCOMING"
[ "$DRY_RUN" = 1 ] || defer "rm -rf -- $(printf '%q' "$INCOMING")"
run cp -a -- "$DIST" "$PLATFORM_SRC/drizzle" "$PLATFORM_SRC/package.json" "$PLATFORM_SRC/package-lock.json" "$INCOMING/"
[ ! -f "$PLATFORM_SRC/.npmrc" ] || run cp -a -- "$PLATFORM_SRC/.npmrc" "$INCOMING/"
# A clean production install from the lockfile: no tsx, vitest, drizzle-kit or PGlite in a release. No
# production dependency has an install script (@node-rs/argon2 ships prebuilt per-platform packages), so
# none runs here as root.
run bash -c 'cd "$1" && npm ci --omit=dev --ignore-scripts --no-audit --no-fund' _ "$INCOMING" || die "npm ci --omit=dev failed in $INCOMING"
run chown -R root:root -- "$INCOMING"
run chmod -R a+rX,go-w -- "$INCOMING"
run mv -T -- "$INCOMING" "$NEW"

if [ "$FAILED" = 1 ]; then
  die "dry run found problems (above); a real run would have stopped"
fi

# ---- 7 dump, migrate ----------------------------------------------------------------------------------------------
log "== 7/11 database dump and migrations"
if [ "$DRY_RUN" = 1 ]; then
  log "[dry-run] pg_dump $PG_DB -> $BACKUPS/$STAMP.dump (when it has a schema), then node dist/scripts/migrate.js as $SVC_USER"
else
  if [ "$MANAGE_DB" = 1 ] && [ "$(pg_value "$PG_DB" "SELECT to_regclass('drizzle.__drizzle_migrations') IS NOT NULL")" = t ]; then
    # the redirect inside the subshell, so the umask applies to the file it creates
    ( umask 077; cd / && runuser -u postgres -- pg_dump -Fc "$PG_DB" > "$BACKUPS/$STAMP.dump.part" ) || die "pg_dump failed; nothing migrated"
    mv -T -- "$BACKUPS/$STAMP.dump.part" "$BACKUPS/$STAMP.dump"
    log "dumped $PG_DB to $BACKUPS/$STAMP.dump ($(du -h -- "$BACKUPS/$STAMP.dump" | cut -f1)); restore: pg_restore --clean -d $PG_DB"
    # keep the newest 10 dumps
    mapfile -t dumps < <(find "$BACKUPS" -maxdepth 1 -type f -name '*.dump' -printf '%f\n' | sort)
    for (( i=0; i<${#dumps[@]}-10; i++ )); do rm -f -- "$BACKUPS/${dumps[$i]}"; log "removed old dump ${dumps[$i]}"; done
  else
    log "no schema yet (first deploy) or an unmanaged database: no dump"
  fi
  # A clean environment: the settings file is the only configuration (the migrate script loads and checks all of it,
  # so a bad setting stops here, before the switch).
  if ! ( cd "$NEW" && runuser -u "$SVC_USER" -- env -i PATH=/usr/bin:/bin HOME=/ "$NODE_BIN" --env-file="$ENV_FILE" "$NEW/dist/scripts/migrate.js" ); then
    # releases/ holds only releases that went live, so --rollback and the pruning never pick one that never ran
    rm -rf -- "$NEW"
    die "migration failed (above). The running release is untouched; $STAMP was removed; the database dump is in $BACKUPS"
  fi
fi

# ---- 8 unit, switch, restart, check -------------------------------------------------------------------------------
log "== 8/11 unit, switch, restart"
if install_file "$UNIT_SRC" "$UNIT_DST" 644; then run systemctl daemon-reload; fi
if [ "$DRY_RUN" = 1 ] || ! systemctl is-enabled --quiet "$UNIT" 2>/dev/null; then run systemctl enable "$UNIT"; fi
if [ -e "$LIVE" ] && [ ! -L "$LIVE" ]; then
  die "$LIVE exists and is not a symlink; move it aside by hand first"
fi
PREV=$(live_target)
switch_to "$NEW"
run systemctl restart "$UNIT"
if [ "$DRY_RUN" = 1 ]; then
  log "[dry-run] would wait for http://127.0.0.1:$API_PORT/health from release $STAMP; on failure switch back to ${PREV:-nothing}"
elif ! health_local "$NEW"; then
  if [ -n "$PREV" ]; then
    warn "switching back to $(basename -- "$PREV")"
    switch_to "$PREV"
    systemctl restart "$UNIT" || true
    health_local "$PREV" || die "the new release failed and the previous one does not answer either" 5
    rm -rf -- "$NEW"
    die "the new release $STAMP failed its health check and was removed; $(basename -- "$PREV") is live again (the schema stays migrated)" 4
  fi
  die "the first release failed its health check (there is no previous one to switch back to)"
fi
[ "$DRY_RUN" = 1 ] || internal_loopback_only

# ---- 9 Caddy ------------------------------------------------------------------------------------------------------
log "== 9/11 Caddy"
if [ "$CADDY" = 0 ]; then
  log "--no-caddy: $GAGE_CADDYFILE untouched"
elif cmp -s "$DEPLOY_DIR/Caddyfile" "$GAGE_CADDYFILE"; then
  log "up to date: $GAGE_CADDYFILE"
else
  need_cmd caddy
  # Validate a copy first, as the caddy user (a root run would leave root-owned files in caddy's data directories):
  # a bad file is never installed, and Caddy keeps serving its current config.
  tmp_caddy=$(mktemp "${TMPDIR:-/tmp}/gage-caddy.XXXXXX")
  defer "rm -f -- $(printf '%q' "$tmp_caddy") $(printf '%q' "$tmp_caddy.err")"
  cp -- "$DEPLOY_DIR/Caddyfile" "$tmp_caddy"
  chmod 644 -- "$tmp_caddy"
  caddy_ok=0
  if command -v caddy >/dev/null 2>&1; then
    validate=(caddy validate --config "$tmp_caddy" --adapter caddyfile)
    if getent passwd caddy >/dev/null 2>&1; then validate=(runuser -u caddy -- env HOME=/var/lib/caddy "${validate[@]}"); fi
    if "${validate[@]}" >/dev/null 2>"$tmp_caddy.err"; then
      caddy_ok=1
      log "tools/deploy/Caddyfile validates"
    else
      cat -- "$tmp_caddy.err" >&2
      fail_check "tools/deploy/Caddyfile does not validate (above); $GAGE_CADDYFILE untouched, Caddy keeps its current config"
    fi
  fi
  if [ "$caddy_ok" = 1 ] || [ "$DRY_RUN" = 1 ]; then
    install_file "$DEPLOY_DIR/Caddyfile" "$GAGE_CADDYFILE" 644 || true
    run systemctl reload caddy
  fi
fi
if [ "$FAILED" = 1 ]; then
  die "dry run found problems (above); a real run would have stopped"
fi

# ---- 10 HTTP check through Caddy ----------------------------------------------------------------------------------
log "== 10/11 HTTP check through Caddy"
http_check

# ---- 11 prune -----------------------------------------------------------------------------------------------------
log "== 11/11 keep the newest $KEEP releases"
mapfile -t all < <(list_releases)
drop=$(( ${#all[@]} - KEEP ))
live_now=$(live_name)
[ -n "$live_now" ] || [ "$DRY_RUN" = 1 ] || die "cannot tell the live release after the switch; pruning nothing"
for (( i=0; i<drop; i++ )); do
  [ "${all[$i]}" = "$live_now" ] && continue
  run rm -rf -- "$RELEASES/${all[$i]}"
  log "removed old release ${all[$i]}"
done
log "done: https://${DOMAIN:-$PUBLIC_IP}/api/ serves platform release $STAMP"
if [ "$NEW_ENV" = 1 ]; then
  log "first deploy: register GI's account, then grant admin (see --help, 'First admin')"
fi
