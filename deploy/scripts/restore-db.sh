#!/usr/bin/env bash
# Restore a dump made by backup-db.sh into a fresh database.
#
#   restore-db.sh --file DUMP|latest [--target-db NAME] [--force] [--drop-after] [--dry-run]
#
# Run as root (sudo) or as the postgres OS user.
#
# Safety rules (all enforced before anything is dropped or written):
#   * the dump must exist, match its .sha256 sidecar (when present) and be readable by pg_restore
#   * a target database that exists AND holds any table is "non-empty": refused without --force
#   * restoring over the LIVE database (BACKUP_DB, default gage_platform) is refused while
#     gage-platform is running, even with --force: stop it first
#   * --force drops the target and recreates it (DROP DATABASE ... WITH (FORCE) disconnects clients)
#   * --drop-after (restore drills) drops the target again afterwards; never allowed on the live DB
#
# Restore drill (recommended monthly and once before go-live):
#   sudo bash restore-db.sh --file latest --target-db gage_restore_drill --drop-after
#
# Settings: BACKUP_DB, BACKUP_DIR, DB owner via RESTORE_DB_OWNER (default: BACKUP_DB), read from the
# environment or /etc/gage/backup.env as in backup-db.sh.
# Exit codes: 0 restored, 1 refused or failed, 2 usage.
set -euo pipefail

SCRIPT_NAME=restore-db
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"

CONF=${GAGE_ETC:-/etc/gage}/backup.env
FILE=''
TARGET=''
FORCE=0
DROP_AFTER=0

usage() {
  sed -n '2,/^set -euo pipefail$/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'
}

pgrun() {
  if [ "$(id -un)" = postgres ]; then
    "$@"
  elif [ "$(id -u)" -eq 0 ]; then
    runuser -u postgres -- "$@"
  else
    die "run as root (sudo) or as the postgres user"
  fi
}

# sql DB QUERY  -> one unaligned value per row
sql() { pgrun psql -X -q -At -v ON_ERROR_STOP=1 -d "$1" -c "$2"; }

db_exists() { [ "$(sql postgres "select count(*) from pg_database where datname = '$1'")" = 1 ]; }

main() {
  while [ $# -gt 0 ]; do
    case $1 in
      --file) [ $# -ge 2 ] || usage_die "--file needs a value"; FILE=$2; shift 2 ;;
      --target-db) [ $# -ge 2 ] || usage_die "--target-db needs a value"; TARGET=$2; shift 2 ;;
      --force) FORCE=1; shift ;;
      --drop-after) DROP_AFTER=1; shift ;;
      --dry-run) DRY_RUN=1; shift ;;
      -h | --help) usage; exit 0 ;;
      *) usage_die "unknown argument: $1 (try --help)" ;;
    esac
  done
  [ -n "$FILE" ] || usage_die "--file is required (a dump path, or 'latest')"

  local live dir owner target
  live=$(conf_value BACKUP_DB gage_platform "$CONF")
  dir=$(conf_value BACKUP_DIR /var/backups/gage "$CONF")
  owner=$(conf_value RESTORE_DB_OWNER "$live" "$CONF")
  target=${TARGET:-$live}
  [[ $target =~ ^[a-z_][a-z0-9_]{0,62}$ ]] || usage_die "bad target database name '$target'"
  [[ $owner =~ ^[a-z_][a-z0-9_]{0,62}$ ]] || usage_die "bad owner role name '$owner'"
  if [ "$DROP_AFTER" = 1 ] && [ "$target" = "$live" ]; then
    usage_die "--drop-after is not allowed on the live database '$live'"
  fi

  need_cmd psql pg_restore sha256sum
  if [ "$FILE" = latest ]; then
    FILE=$(find "$dir" -maxdepth 1 -type f -name "$live-*.dump" | sort | tail -n1)
    [ -n "$FILE" ] || die "no dumps named $live-*.dump in $dir"
    log "latest dump: $FILE"
  fi
  [ -f "$FILE" ] || die "dump not found: $FILE"
  if [ -f "$FILE.sha256" ]; then
    (cd "$(dirname "$FILE")" && sha256sum -c --quiet -- "$(basename "$FILE").sha256") || die "checksum mismatch: $FILE is corrupt or was modified"
    log "checksum ok"
  else
    warn "no $FILE.sha256 sidecar: skipping the checksum check"
  fi
  pg_restore --list "$FILE" >/dev/null || die "pg_restore cannot read $FILE"
  pg_isready -q || die "PostgreSQL is not accepting connections"

  local exists=0 tables=0 conns=0
  if db_exists "$target"; then
    exists=1
    tables=$(sql "$target" "select count(*) from information_schema.tables where table_schema not in ('pg_catalog','information_schema')")
    conns=$(sql postgres "select count(*) from pg_stat_activity where datname = '$target' and pid <> pg_backend_pid()")
  fi

  if [ "$target" = "$live" ] && systemctl is-active --quiet gage-platform.service; then
    die "gage-platform is running and '$target' is the live database: stop it first (sudo systemctl stop gage-platform)"
  fi
  if [ "$exists" = 1 ] && [ "$tables" -gt 0 ] && [ "$FORCE" != 1 ]; then
    die "database '$target' exists and holds $tables table(s): refusing without --force (it would be DROPPED)"
  fi
  if [ "$exists" = 1 ] && [ "$conns" -gt 0 ] && [ "$FORCE" != 1 ]; then
    die "database '$target' has $conns open connection(s): refusing without --force"
  fi
  if [ "$exists" = 1 ] && [ "$tables" -eq 0 ] && [ "$FORCE" != 1 ]; then
    log "database '$target' exists but is empty: it will be dropped and recreated"
  fi

  if [ "$DRY_RUN" = 1 ]; then
    local plan="create '$target' owned by '$owner', then pg_restore --exit-on-error --single-transaction $FILE"
    if [ "$exists" = 1 ]; then plan="drop '$target' (it holds $tables table(s), $conns connection(s)), $plan"; fi
    if [ "$DROP_AFTER" = 1 ]; then plan="$plan, then drop '$target' again (--drop-after)"; fi
    log "dry run: would $plan"
    [ "$exists" = 1 ] || log "dry run: '$target' does not exist yet"
    return 0
  fi

  if [ "$exists" = 1 ]; then
    log "dropping '$target'"
    sql postgres "drop database \"$target\" with (force)" >/dev/null
  fi
  log "creating '$target' owned by '$owner'"
  sql postgres "create database \"$target\" owner \"$owner\"" >/dev/null
  log "restoring $FILE into '$target'"
  # --no-owner/--role: every object ends up owned by $owner, whatever role dumped it.
  if ! pgrun pg_restore --exit-on-error --single-transaction --no-owner --no-privileges \
    --role="$owner" --dbname="$target" "$FILE"; then
    warn "pg_restore failed: '$target' was left as is (the single transaction rolled back); inspect it or drop it"
    die "restore failed"
  fi

  local n_tables n_mig size
  n_tables=$(sql "$target" "select count(*) from information_schema.tables where table_schema not in ('pg_catalog','information_schema')")
  size=$(sql "$target" "select pg_size_pretty(pg_database_size('$target'))")
  # drizzle keeps applied migrations in drizzle.__drizzle_migrations; a plain restore must bring it back.
  if n_mig=$(sql "$target" 'select count(*) from drizzle.__drizzle_migrations' 2>/dev/null); then
    :
  else
    n_mig='n/a (table drizzle.__drizzle_migrations not found)'
  fi
  log "restored '$target': $n_tables tables, $size, applied migrations: $n_mig"
  [ "$n_tables" -gt 0 ] || die "the restore produced a database with no tables: treat the dump as bad"

  if [ "$DROP_AFTER" = 1 ]; then
    log "dropping '$target' (--drop-after)"
    sql postgres "drop database \"$target\" with (force)" >/dev/null
  fi
  log "done"
}

main "$@"
