#!/usr/bin/env bash
# Replace the Solar RP source tree on the VPS with the tarball made by tools/deploy/pack-source.sh on GI's PC.
#
#   unpack-source.sh TARBALL [--sha256 HEX] [--force] [--dry-run]
#
#   TARBALL        the .tar.gz that pack-source.sh wrote and scp copied to the VPS
#   --sha256 HEX   the checksum pack-source.sh printed; the run stops if the file differs (strongly recommended)
#   --force        unpack even when this exact tarball (same sha256) is already the current tree
#   --dry-run      do every check, including a real extraction into a scratch folder, and change nothing under /opt/gage
#
# What it does (the tree is REPLACED, not overlaid, so files deleted on the PC are gone here too):
#   1  checks the tarball: readable, checksum, only plain files and folders, every path under ./, no "..", nothing forbidden
#      (keys, .env files, node_modules, game files, build output)
#   2  extracts into /opt/gage/src.new and checks it (bash -n on the scripts, no CRLF, the files the kit needs)
#   3  makes the scripts executable (a tar made on NTFS may lose the bit) and links src.new/build -> /opt/gage/build,
#      because installer/build-installer.sh writes to <repo>/build/win/installer
#   4  src -> src.prev (one previous copy is kept), src.new -> src, and installs tools/deploy/VPS-CLAUDE.md as /opt/gage/CLAUDE.md
# Takes the "build" lock, so it waits for (refuses during) a running build-client.sh. Nothing in /opt/gage/build, /opt/gage/server,
# /var/www/gage, /etc/gage or /root/.gage-keys is touched. To undo: mv /opt/gage/src /opt/gage/src.bad && mv /opt/gage/src.prev /opt/gage/src.
set -euo pipefail

SCRIPT_NAME=unpack-source
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

# The whole body is one function: bash has parsed all of it before the first command runs, and this script's own folder is
# renamed (src -> src.prev) while it is running.
main() {
  local TARBALL='' WANT_SHA='' FORCE=0
  while [ $# -gt 0 ]; do
    case $1 in
      --sha256) [ $# -ge 2 ] || usage_die "--sha256 needs a value"; WANT_SHA=$2; shift 2 ;;
      --force) FORCE=1; shift ;;
      --dry-run) DRY_RUN=1; shift ;;
      -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; return 0 ;;
      -*) usage_die "unknown option $1" ;;
      *) [ -z "$TARBALL" ] || usage_die "more than one tarball given"; TARBALL=$1; shift ;;
    esac
  done
  [ -n "$TARBALL" ] || usage_die "give the tarball path"
  [ -f "$TARBALL" ] || die "no such file: $TARBALL"
  if [ -n "$WANT_SHA" ]; then
    WANT_SHA=$(printf '%s' "$WANT_SHA" | tr 'A-F' 'a-f')
    [[ $WANT_SHA =~ ^[0-9a-f]{64}$ ]] || usage_die "--sha256 must be 64 hex characters"
  fi
  need_cmd tar gzip sha256sum find sed grep awk df

  deploy_config
  local SRC=$GAGE_OPT/src PREV=$GAGE_OPT/src.prev NEW=$GAGE_OPT/src.new BUILD=$GAGE_OPT/build
  local INFO=$GAGE_OPT/src.info CLAUDE_MD=$GAGE_OPT/CLAUDE.md
  log "tarball $TARBALL | tree $SRC | $([ "$DRY_RUN" = 1 ] && echo DRY RUN || echo real run)"
  if [ "$DRY_RUN" = 0 ]; then
    [ "$(id -u)" = 0 ] || warn "not root: the tree under $GAGE_OPT is meant to belong to root"
    mkdir -p "$GAGE_OPT"
  fi
  acquire_lock build

  local SCRATCH
  SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/unpack-source.XXXXXX")
  defer "rm -rf -- $(printf '%q' "$SCRATCH")"

  # ---- 1 the tarball ------------------------------------------------------------------------------------------
  log "== 1/4 check the tarball"
  local GOT_SHA
  GOT_SHA=$(sha256_of "$TARBALL")
  if [ -n "$WANT_SHA" ]; then
    [ "$GOT_SHA" = "$WANT_SHA" ] || die "checksum mismatch: file $GOT_SHA, expected $WANT_SHA (the upload is damaged or it is another file)"
    log "sha256 matches ($GOT_SHA)"
  else
    warn "no --sha256 given; the file is $GOT_SHA (compare it with what pack-source.sh printed)"
  fi
  if [ "$FORCE" = 0 ] && [ -f "$INFO" ]; then
    local last_info
    last_info=$(tail -n 1 "$INFO")
    if [[ $last_info == *"$GOT_SHA"* ]]; then
      log "this tarball is already the current tree ($last_info). Nothing to do (--force to unpack it again)."
      return 0
    fi
  fi

  local LIST=$SCRATCH/tar.list LONG=$SCRATCH/tar.long
  tar -tzf "$TARBALL" > "$LIST" 2> "$SCRATCH/tar.err" || { cat "$SCRATCH/tar.err" >&2; die "$TARBALL is not a readable .tar.gz"; }
  tar -tvzf "$TARBALL" > "$LONG" 2> "$SCRATCH/tar.err" || { cat "$SCRATCH/tar.err" >&2; die "cannot list $TARBALL"; }
  local N_ENTRIES
  N_ENTRIES=$(wc -l < "$LIST")
  [ "$N_ENTRIES" -gt 500 ] || die "only $N_ENTRIES entries in the tarball: not a Solar RP source tree"
  [ "$(wc -l < "$LONG")" = "$N_ENTRIES" ] || die "tar listings disagree (a file name with a newline in it?)"
  local bad
  bad=$(grep -v -E '^[-d]' "$LONG" | head -n 5 || true)
  [ -z "$bad" ] || { printf '%s\n' "$bad" >&2; die "the tarball holds links or special files (shown above); the tree has none"; }
  bad=$(grep -v -E '^\./' "$LIST" | head -n 5 || true)
  [ -z "$bad" ] || { printf '%s\n' "$bad" >&2; die "entries that do not start with ./ (shown above)"; }
  bad=$(grep -E '(^|/)\.\.(/|$)' "$LIST" | head -n 5 || true)
  [ -z "$bad" ] || { printf '%s\n' "$bad" >&2; die "entries with .. in the path (shown above)"; }
  bad=$(grep -E -i '(^|/)(node_modules|\.git|\.gage-keys|\.impeccable|enhanced EXE|winlibs[^/]*|ffmpeg[^/]*)(/|$)|\.pem$|\.key$|(^|/)\.env|update-signing-key|^\./build(/|$)|^\./logs(/|$)|\.tgz$|\.tar\.gz$' "$LIST" | head -n 10 || true)
  [ -z "$bad" ] || { printf '%s\n' "$bad" >&2; die "forbidden paths in the tarball (shown above): keys, .env files, node_modules, game files, build output or big toolchains do not belong on the VPS"; }
  grep -q -x -F './CMakeLists.txt' "$LIST" || die "no ./CMakeLists.txt in the tarball"
  grep -q -x -F './tools/deploy/unpack-source.sh' "$LIST" || die "no ./tools/deploy/unpack-source.sh in the tarball (is it an old pack?)"
  log "$N_ENTRIES entries: plain files and folders, all under ./, nothing forbidden"

  # free space: the tree (~150 MB) twice (new + prev), plus the build tree and the wine prefix grow after it
  local free_kb
  free_kb=$(df -Pk "${GAGE_OPT}" 2>/dev/null | awk 'NR==2 {print $4}' || true)
  if is_uint "${free_kb:-}"; then
    if [ "$free_kb" -lt 2097152 ]; then warn "only $((free_kb / 1024)) MiB free under $GAGE_OPT (the tree twice, plus the build, needs about 2 GiB)"; fi
  fi

  # ---- 2 extract and check -----------------------------------------------------------------------------------------
  log "== 2/4 extract and check"
  local STAGE=$NEW
  if [ "$DRY_RUN" = 1 ]; then STAGE=$SCRATCH/stage; fi
  if [ "$DRY_RUN" = 0 ]; then
    # Only ever remove the staging folder this script owns.
    [ "$NEW" = "$GAGE_OPT/src.new" ] && [ -n "$GAGE_OPT" ] || die "refusing to remove '$NEW'"
    if [ -e "$NEW" ] || [ -L "$NEW" ]; then log "removing the leftover staging folder $NEW"; rm -rf -- "$NEW"; fi
    # a check that fails below must not leave a half-checked tree behind (after the swap $NEW no longer exists: harmless)
    defer "rm -rf -- $(printf '%q' "$NEW")"
  fi
  mkdir -p "$STAGE"
  tar -xzf "$TARBALL" -C "$STAGE" --no-same-owner 2> "$SCRATCH/tar.err" || { cat "$SCRATCH/tar.err" >&2; die "extraction failed"; }
  local N_FILES EXPECT_FILES
  N_FILES=$(find "$STAGE" -type f | wc -l)
  EXPECT_FILES=$(grep -c -v '/$' "$LIST" || true)
  [ "$N_FILES" = "$EXPECT_FILES" ] || die "extracted $N_FILES files but the tarball lists $EXPECT_FILES"
  [ "$N_FILES" -gt 500 ] || die "only $N_FILES files extracted"

  local f need=(CMakeLists.txt cmake/toolchain-mingw-linux.cmake
    tools/deploy/build-client.sh tools/deploy/publish-client.sh tools/deploy/deploy-server.sh tools/deploy/release.sh tools/deploy/bootstrap-vps.sh
    tools/deploy/unpack-source.sh tools/deploy/lib/common.sh tools/deploy/lib/wine-env.sh tools/deploy/lib/ops.mjs
    tools/deploy/wine-selftests.list tools/deploy/shim/cygpath tools/deploy/shim/iscc tools/deploy/shim/wine-exe
    tools/deploy/VPS-CLAUDE.md tools/deploy/Caddyfile tools/deploy/gage-server.service
    tools/update-sign/sign.mjs installer/build-installer.sh installer/gage-mp.iss
    apps/launcher/gage_gui.cpp apps/launcher-ui/package-lock.json apps/server/CMakeLists.txt
    .toolchain/innosetup-6.7.3/ISCC.exe docs/OPS.md)
  for f in "${need[@]}"; do
    [ -s "$STAGE/$f" ] || die "the tarball lacks $f (or it is empty)"
  done
  local scripts=("$STAGE"/tools/deploy/*.sh "$STAGE"/tools/deploy/lib/*.sh "$STAGE"/tools/deploy/shim/* "$STAGE"/installer/build-installer.sh)
  local crlf='' syntax=''
  for f in "${scripts[@]}"; do
    # tr + wc, not grep: grep for a carriage return matches every line under Git Bash
    if [ "$(tr -d '\r' < "$f" | wc -c)" != "$(wc -c < "$f")" ]; then crlf="$crlf ${f#"$STAGE"/}"; fi
    if ! bash -n "$f" 2>/dev/null; then syntax="$syntax ${f#"$STAGE"/}"; fi
  done
  [ -z "$crlf" ] || die "CRLF line endings in:$crlf (the PC copy was edited with a Windows editor; fix there and pack again)"
  [ -z "$syntax" ] || die "bash -n fails for:$syntax"
  local VERSION
  VERSION=$(sed -n 's/.*kClientVersion = L"\([^"]*\)".*/\1/p' "$STAGE/apps/launcher/gage_gui.cpp")
  [[ $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "no usable kClientVersion in apps/launcher/gage_gui.cpp (got '$VERSION')"
  log "$N_FILES files extracted; ${#need[@]} required files present; ${#scripts[@]} scripts pass bash -n and have no CRLF; kClientVersion $VERSION"

  # ---- 3 permissions and the build link ---------------------------------------------------------------------------------
  log "== 3/4 permissions and the build link"
  local x
  for x in "$STAGE"/tools/deploy/*.sh "$STAGE"/tools/deploy/lib/*.sh "$STAGE"/tools/deploy/lib/*.mjs "$STAGE"/tools/deploy/shim/* \
           "$STAGE"/installer/build-installer.sh "$STAGE"/tools/update-sign/*.mjs; do
    if [ -f "$x" ]; then chmod +x "$x"; fi
  done
  chmod -R go-w "$STAGE"
  # The tree is replaced on every unpack, the build tree is not: the link keeps /opt/gage/build across unpacks.
  if [ -e "$STAGE/build" ] || [ -L "$STAGE/build" ]; then die "the tarball contains ./build"; fi
  ln -s "$BUILD" "$STAGE/build"
  [ "$(readlink "$STAGE/build")" = "$BUILD" ] || die "could not create the build link"
  if [ ! -d "$BUILD" ] && [ "$DRY_RUN" = 0 ]; then mkdir -p "$BUILD"; log "created $BUILD (the persistent build tree)"; fi
  log "$STAGE/build -> $BUILD; scripts and shims are executable"

  # ---- 4 swap ----------------------------------------------------------------------------------------------------------
  log "== 4/4 swap the tree and install CLAUDE.md"
  if [ "$DRY_RUN" = 1 ]; then
    log "[dry-run] would: rm -rf $PREV; mv $SRC $PREV (if it exists); mv $NEW $SRC"
    log "[dry-run] would: install tools/deploy/VPS-CLAUDE.md as $CLAUDE_MD (previous kept as $CLAUDE_MD.prev when different)"
    log "[dry-run] would: append '$(date -u +%Y-%m-%dT%H:%M:%SZ) $GOT_SHA $(basename -- "$TARBALL") $VERSION' to $INFO"
    log "dry-run finished: every check passed; nothing under $GAGE_OPT was changed."
    return 0
  fi
  [ "$PREV" = "$GAGE_OPT/src.prev" ] || die "refusing to remove '$PREV'"
  if [ -e "$PREV" ] || [ -L "$PREV" ]; then rm -rf -- "$PREV"; fi
  if [ -e "$SRC" ] || [ -L "$SRC" ]; then
    mv -T -- "$SRC" "$PREV"
  fi
  if ! mv -T -- "$NEW" "$SRC"; then
    # put the old tree back before failing
    if [ -e "$PREV" ] && [ ! -e "$SRC" ]; then mv -T -- "$PREV" "$SRC" || warn "could not restore $PREV to $SRC; do it by hand"; fi
    die "could not move $NEW to $SRC"
  fi
  if [ -f "$SRC/tools/deploy/VPS-CLAUDE.md" ]; then
    if [ -f "$CLAUDE_MD" ] && ! cmp -s "$CLAUDE_MD" "$SRC/tools/deploy/VPS-CLAUDE.md"; then cp -p -- "$CLAUDE_MD" "$CLAUDE_MD.prev"; fi
    install -m 644 "$SRC/tools/deploy/VPS-CLAUDE.md" "$CLAUDE_MD"
  fi
  printf '%s %s %s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$GOT_SHA" "$(basename -- "$TARBALL")" "$VERSION" >> "$INFO"
  log "done: $SRC is now $(basename -- "$TARBALL") (kClientVersion $VERSION); previous tree kept as $PREV; $CLAUDE_MD installed."
  log "next: bash $SRC/tools/deploy/build-client.sh --dry-run   (docs/OPS.md, 'Release a client update')"
}

main "$@"
exit $?
