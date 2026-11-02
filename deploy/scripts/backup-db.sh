#!/usr/bin/env bash
# Dump the platform database to a verified, checksummed custom-format file and prune old dumps.
#
#   backup-db.sh [--label TEXT] [--db NAME] [--dir DIR] [--keep-days N] [--min-keep N] [--dry-run]
#
# Run as root (sudo) or as the postgres OS user; the nightly gage-backup.timer runs it as postgres.
# A dump becomes visible only after pg_restore --list has read it back and its sha256 is written.
# Old dumps are deleted when older than --keep-days, but the newest --min-keep are never deleted
# (so a stopped timer cannot age the last good backups away).
#
# Settings (environment variable, else /etc/gage/backup.env, else default):
#   BACKUP_DB=gage_platform       database to dump
#   BACKUP_DIR=/var/backups/gage  where dumps go (owned by postgres, mode 0755; dumps are 0600)
#   BACKUP_KEEP_DAYS=14           retention in days
#   BACKUP_MIN_KEEP=3             never delete the newest N dumps
#   BACKUP_MIN_FREE_MB=1024       refuse to start with less free space than this in BACKUP_DIR
#   BACKUP_POST_HOOK=             optional executable run as: HOOK <dump file> (off-site copy);
#                                 a non-zero exit fails the backup run loudly
# Exit codes: 0 done, 1 failed, 2 usage.
set -euo pipefail

SCRIPT_NAME=backup-db
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=lib.sh
. "$SCRIPT_DIR/lib.sh"

CONF=${GAGE_ETC:-/etc/gage}/backup.env
PARTIAL=''
LABEL=''
OPT_DB=''
OPT_DIR=''
OPT_KEEP_DAYS=''
OPT_MIN_KEEP=''

usage() {
  sed -n '2,/^set -euo pipefail$/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'
}

# retention_list DIR DB KEEP_DAYS MIN_KEEP  Print the dumps that may be deleted: not among the
# newest MIN_KEEP (names sort by their UTC timestamp) and older than KEEP_DAYS.
retention_list() {
  local dir=$1 db=$2 days=$3 min=$4 n=0 f
  while IFS= read -r f; do
    n=$((n + 1))
    [ "$n" -gt "$min" ] || continue
    if [ -n "$(find "$f" -maxdepth 0 -mtime +"$days")" ]; then
      printf '%s\n' "$f"
    fi
  done < <(find "$dir" -maxdepth 1 -type f -name "$db-*.dump" | sort -r)
}

pgrun() { # run a command as the postgres OS user
  if [ "$(id -un)" = postgres ]; then
    "$@"
  elif [ "$(id -u)" -eq 0 ]; then
    runuser -u postgres -- "$@"
  else
    die "run as root (sudo) or as the postgres user"
  fi
}

main() {
  while [ $# -gt 0 ]; do
    case $1 in
      --label) [ $# -ge 2 ] || usage_die "--label needs a value"; LABEL=$2; shift 2 ;;
      --db) [ $# -ge 2 ] || usage_die "--db needs a value"; OPT_DB=$2; shift 2 ;;
      --dir) [ $# -ge 2 ] || usage_die "--dir needs a value"; OPT_DIR=$2; shift 2 ;;
      --keep-days) [ $# -ge 2 ] || usage_die "--keep-days needs a value"; OPT_KEEP_DAYS=$2; shift 2 ;;
      --min-keep) [ $# -ge 2 ] || usage_die "--min-keep needs a value"; OPT_MIN_KEEP=$2; shift 2 ;;
      --dry-run) DRY_RUN=1; shift ;;
      -h | --help) usage; exit 0 ;;
      *) usage_die "unknown argument: $1 (try --help)" ;;
    esac
  done

  local db dir keep_days min_keep min_free hook
  db=${OPT_DB:-$(conf_value BACKUP_DB gage_platform "$CONF")}
  dir=${OPT_DIR:-$(conf_value BACKUP_DIR /var/backups/gage "$CONF")}
  keep_days=${OPT_KEEP_DAYS:-$(conf_value BACKUP_KEEP_DAYS 14 "$CONF")}
  min_keep=${OPT_MIN_KEEP:-$(conf_value BACKUP_MIN_KEEP 3 "$CONF")}
  min_free=$(conf_value BACKUP_MIN_FREE_MB 1024 "$CONF")
  hook=$(conf_value BACKUP_POST_HOOK '' "$CONF")

  [[ $db =~ ^[a-z_][a-z0-9_]{0,62}$ ]] || usage_die "bad database name '$db'"
  [ -z "$LABEL" ] || [[ $LABEL =~ ^[A-Za-z0-9._-]{1,40}$ ]] || usage_die "--label may contain only A-Z a-z 0-9 . _ - (max 40)"
  is_uint "$keep_days" && [ "$keep_days" -ge 1 ] || usage_die "keep-days must be a positive integer"
  is_uint "$min_keep" && [ "$min_keep" -ge 1 ] || usage_die "min-keep must be a positive integer (at least 1)"
  is_uint "$min_free" || usage_die "BACKUP_MIN_FREE_MB must be an integer"
  case $dir in /*) ;; *) usage_die "BACKUP_DIR must be an absolute path" ;; esac
  [ "$dir" != / ] || usage_die "BACKUP_DIR must not be /"
  if [ -n "$hook" ]; then
    [ -x "$hook" ] || die "BACKUP_POST_HOOK $hook is not an executable file"
  fi

  need_cmd pg_dump pg_restore psql pg_isready sha256sum find sort df
  local ts name final
  ts=$(date -u +%Y%m%dT%H%M%SZ)
  name=$db-$ts${LABEL:+-$LABEL}.dump
  final=$dir/$name
  PARTIAL=$dir/.partial-$name

  if [ "$DRY_RUN" = 1 ]; then
    log "dry run: would dump database '$db' to $final (pg_dump --format=custom), verify it with pg_restore --list, write $name.sha256"
    if [ -d "$dir" ]; then
      log "dry run: retention ($keep_days days, newest $min_keep always kept) would delete:"
      retention_list "$dir" "$db" "$keep_days" "$min_keep" | sed 's/^/  /'
    else
      log "dry run: $dir does not exist yet (would be created)"
    fi
    [ -z "$hook" ] || log "dry run: would run post hook: $hook $final"
    return 0
  fi

  pg_isready -q || die "PostgreSQL is not accepting connections"
  pgrun psql -X -q -At -d "$db" -c 'select 1' >/dev/null || die "cannot connect to database '$db'"
  if [ "$(id -u)" -eq 0 ]; then
    install -d -m 0755 -o postgres -g postgres "$dir"
  else
    mkdir -p -- "$dir"
  fi
  local free_mb
  free_mb=$(df -Pm "$dir" | awk 'NR==2 {print $4}')
  is_uint "$free_mb" || die "cannot read free space of $dir"
  [ "$free_mb" -ge "$min_free" ] || die "only ${free_mb} MB free in $dir (BACKUP_MIN_FREE_MB=$min_free): not dumping"

  trap '[ -z "${PARTIAL:-}" ] || rm -f -- "$PARTIAL"' EXIT
  log "dumping '$db' to $final"
  umask 077
  pgrun pg_dump --format=custom --compress=6 --no-owner --no-privileges --dbname="$db" --file="$PARTIAL" ||
    die "pg_dump failed"
  # Prove the file is a readable archive before it gets a final name.
  pg_restore --list "$PARTIAL" >/dev/null || die "the new dump cannot be read back by pg_restore --list: discarded"
  [ -s "$PARTIAL" ] || die "the new dump is empty: discarded"
  mv -T -- "$PARTIAL" "$final"
  PARTIAL=''
  chmod 0600 "$final"
  (cd "$dir" && sha256sum -- "$name" >"$name.sha256")
  chmod 0644 "$final.sha256"
  local size
  size=$(du -h -- "$final" | cut -f1)
  log "dump complete: $final ($size, sha256 $(cut -d' ' -f1 "$final.sha256"))"

  local f
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    log "retention: deleting $f"
    rm -f -- "$f" "$f.sha256"
  done < <(retention_list "$dir" "$db" "$keep_days" "$min_keep")

  if [ -n "$hook" ]; then
    log "running post hook: $hook"
    "$hook" "$final" || die "post hook failed (the local dump is intact: $final)"
  fi
}

# Run main only when executed, so tests can source this file for retention_list.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
