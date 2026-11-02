#!/usr/bin/env bash
# Solar RP VPS kit: the installer. Normally started by the .run file (bash solar-rp-vps-kit-<stamp>.run), which checks the
# payload checksum first. It can also be run from the extracted folder: bash /root/solar-rp-kit/<stamp>/install.sh
# Run as root on the VPS (Ubuntu/Debian x86-64).
#
#   install.sh [--dry-run] [--no-plugins] [--skip-claude] [--skip-memory] [--skip-source] [--force-source]
#
#   --dry-run       print every step and change nothing: no file written, no backup folder, no plugin command run
#   --no-plugins    never run `claude plugin ...` (the commands are printed instead)
#   --skip-claude   leave /root/.claude alone (this also skips the memory and the plugins)
#   --skip-memory   do not install the project auto-memory
#   --skip-source   do not call unpack-source.sh
#   --force-source  give --force to unpack-source.sh (unpack the same source tarball again)
#
# What it does, in this order. Every step checks first, so a second run changes nothing that is already right:
#   1 check    root; the kit's own MANIFEST.sha256 (every file in the folder); OS, claude and node are reported
#   2 claude   /root/.claude gets GI's CLAUDE.md (his text unchanged + a marked VPS section), rules/ (with 00-vps-linux.md),
#              skills/, agents/, commands/, tools/, hooks/, lessons-archive/, output-styles/, contexts/, the status line files and
#              critic-rules.md. A file that exists and differs is copied to /root/.claude/.kit-backup-<UTC stamp>/ first. Nothing is
#              ever deleted. settings.json is MERGED key by key (the VPS wins on any value it already has; permission rules and
#              hooks are united); the kit copy is already sanitised on the PC (no env values, no Windows hooks).
#   3 memory   the project auto-memory goes under the project folder names Claude Code computes for /opt/gage/src and /opt/gage
#              (every character that is not a letter or digit becomes '-': -opt-gage-src and -opt-gage)
#   4 plugins  marketplaces and plugins from plugins.list are added with `claude plugin ...` when claude is installed; otherwise
#              the exact commands are printed
#   5 source   the kit's source tarball is swapped into /opt/gage/src by tools/deploy/unpack-source.sh (taken from the same
#              tarball), which also installs /opt/gage/CLAUDE.md
#   6 report   claude / bootstrap / signing-key status and what to run next. It never runs bootstrap-vps.sh, a deploy, a publish
#              or the game server, and never touches /etc/gage/deploy.env.
#
# Where things go can be moved for tests: KIT_CLAUDE_HOME (default /root/.claude), GAGE_OPT (default /opt/gage; unpack-source.sh
# honours it too), KIT_PROJECT_DIR (default $GAGE_OPT/src; only used to compute the memory folder names), GAGE_ENV_FILE
# (default /etc/gage/deploy.env), GAGE_KEY_DIR (default /root/.gage-keys), KIT_ALLOW_NONROOT=1.
# Exit codes: 0 done (warnings are counted at the end), 1 failure, 2 usage, 3 the kit's files do not match MANIFEST.sha256.
set -euo pipefail

KIT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DRY=0; NO_PLUGINS=0; SKIP_CLAUDE=0; SKIP_MEMORY=0; SKIP_SOURCE=0; FORCE_SOURCE=0
WARNINGS=0
log()  { printf '[vps-kit] %s\n' "$*"; }
warn() { printf '[vps-kit] WARN: %s\n' "$*" >&2; WARNINGS=$((WARNINGS + 1)); }
die()  { printf '[vps-kit] ERROR: %s\n' "$1" >&2; exit "${2:-1}"; }
usage_die() { printf '[vps-kit] ERROR: %s (try --help)\n' "$1" >&2; exit 2; }

while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) DRY=1; shift ;;
    --no-plugins) NO_PLUGINS=1; shift ;;
    --skip-claude) SKIP_CLAUDE=1; shift ;;
    --skip-memory) SKIP_MEMORY=1; shift ;;
    --skip-source) SKIP_SOURCE=1; shift ;;
    --force-source) FORCE_SOURCE=1; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) usage_die "unknown option $1" ;;
  esac
done

CLAUDE_HOME=${KIT_CLAUDE_HOME:-/root/.claude}
GAGE_OPT=${GAGE_OPT:-/opt/gage}
PROJECT_DIR=${KIT_PROJECT_DIR:-$GAGE_OPT/src}
ENV_FILE=${GAGE_ENV_FILE:-/etc/gage/deploy.env}
KEY_DIR=${GAGE_KEY_DIR:-/root/.gage-keys}
RUN_STAMP=$(date -u +%Y%m%dT%H%M%SZ)
BACKUP_DIR=$CLAUDE_HOME/.kit-backup-$RUN_STAMP
MODE_TEXT=$([ "$DRY" = 1 ] && echo 'DRY RUN, nothing is changed' || echo 'real run')

kit_info() { awk -F= -v k="$1" '$1 == k { sub(/^[^=]*=/, ""); print; exit }' "$KIT_DIR/kit-info.txt"; }

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/vps-kit-install.XXXXXX")
trap 'rm -rf -- "$SCRATCH"' EXIT

# ---- file installation helpers -------------------------------------------------------------------------------------
T_NEW=0; T_UPD=0; T_SAME=0      # totals over the whole run
C_NEW=0; C_UPD=0; C_SAME=0      # the part being installed
SHOWN=0                          # detail lines printed for replaced files (the first 40 only)
BACKED_UP=0

backup_file() {
  local dst=$1 rel b
  rel=${dst#"$CLAUDE_HOME"/}
  b=$BACKUP_DIR/$rel
  [ "$DRY" = 1 ] && return 0
  mkdir -p -- "$(dirname -- "$b")"
  [ -e "$b" ] || cp -p -- "$dst" "$b"
  BACKED_UP=$((BACKED_UP + 1))
}

# install_file SRC DST: copy when DST is missing or different. A different DST is backed up first. DST is always under CLAUDE_HOME.
install_file() {
  local src=$1 dst=$2 mode=644
  case $dst in *.sh) mode=755 ;; esac
  if [ ! -e "$dst" ] && [ ! -L "$dst" ]; then
    C_NEW=$((C_NEW + 1))
    [ "$DRY" = 1 ] && return 0
    mkdir -p -- "$(dirname -- "$dst")"
    install -m "$mode" -- "$src" "$dst"
    return 0
  fi
  if [ -d "$dst" ]; then
    warn "$dst is a folder where the kit has a file: left alone"
    return 0
  fi
  if cmp -s -- "$src" "$dst"; then
    C_SAME=$((C_SAME + 1))
    if [ "$mode" = 755 ] && [ ! -x "$dst" ] && [ "$DRY" = 0 ]; then chmod 755 -- "$dst"; fi
    return 0
  fi
  C_UPD=$((C_UPD + 1))
  if [ "$SHOWN" -lt 40 ]; then
    log "  replaces ${dst#"$CLAUDE_HOME"/} (the VPS copy differs; it goes to ${BACKUP_DIR##*/}/)"
    SHOWN=$((SHOWN + 1))
  fi
  [ "$DRY" = 1 ] && return 0
  backup_file "$dst"
  install -m "$mode" -- "$src" "$dst"
}

# install_tree SRCDIR DSTDIR LABEL: every file below SRCDIR, same relative path below DSTDIR. Files only the VPS has stay.
install_tree() {
  local sdir=$1 ddir=$2 label=$3 f
  C_NEW=0; C_UPD=0; C_SAME=0
  [ -d "$sdir" ] || { warn "kit has no $label"; return 0; }
  while IFS= read -r -d '' f; do
    install_file "$f" "$ddir/${f#"$sdir"/}"
  done < <(find "$sdir" -type f -print0 | LC_ALL=C sort -z)
  T_NEW=$((T_NEW + C_NEW)); T_UPD=$((T_UPD + C_UPD)); T_SAME=$((T_SAME + C_SAME))
  log "$label: $C_NEW new, $C_UPD replaced (backed up), $C_SAME already identical"
}

path_slug() { printf '%s' "$1" | sed 's/[^A-Za-z0-9]/-/g'; }

# ---- 1 check ---------------------------------------------------------------------------------------------------------
STAMP=$(kit_info stamp)
[ -n "$STAMP" ] || die "kit-info.txt has no stamp: this is not an extracted kit folder"
log "Solar RP VPS kit $STAMP | $MODE_TEXT"
if [ "$(id -u)" != 0 ] && [ "${KIT_ALLOW_NONROOT:-0}" != 1 ]; then die "run as root (the Claude setup goes to $CLAUDE_HOME and the source to $GAGE_OPT)"; fi
for c in tar gzip sha256sum find sed grep awk cmp install mktemp; do
  command -v "$c" >/dev/null 2>&1 || die "missing command: $c"
done
log "== 1/6 check the kit's files"
[ -s "$KIT_DIR/MANIFEST.sha256" ] || die "no MANIFEST.sha256 next to install.sh" 3
if ! (cd "$KIT_DIR" && sha256sum -c --quiet MANIFEST.sha256 > "$SCRATCH/manifest.out" 2>&1); then
  head -n 5 "$SCRATCH/manifest.out" >&2
  die "the kit's files do not match MANIFEST.sha256 (shown above): damaged copy, build the kit again" 3
fi
log "MANIFEST.sha256 ok: $(wc -l < "$KIT_DIR/MANIFEST.sha256") files"
OS_NAME=unknown
if [ -r /etc/os-release ]; then OS_NAME=$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release | tr -d '"' | head -n 1); fi
log "system: ${OS_NAME:-unknown}, $(uname -m); client version in the kit: $(kit_info client_version)"

CLAUDE_BIN=$(command -v claude 2>/dev/null || true)
if [ -z "$CLAUDE_BIN" ] && [ -x "${HOME:-/root}/.local/bin/claude" ]; then CLAUDE_BIN=${HOME:-/root}/.local/bin/claude; fi
NODE_OK=0; NODE_TEXT='not installed (bootstrap-vps.sh installs Node 22)'
if command -v node >/dev/null 2>&1; then
  nv=$(node --version 2>/dev/null || true); nmajor=${nv#v}; nmajor=${nmajor%%.*}
  NODE_TEXT=${nv:-unknown}
  if [[ $nmajor =~ ^[0-9]+$ ]] && [ "$nmajor" -ge 18 ]; then NODE_OK=1; fi
fi
log "claude: ${CLAUDE_BIN:-not installed}; node: $NODE_TEXT"

# ---- 2 claude --------------------------------------------------------------------------------------------------------
SETTINGS_RESULT='skipped'
if [ "$SKIP_CLAUDE" = 1 ]; then
  log "== 2/6 claude setup: skipped (--skip-claude)"
else
  log "== 2/6 Claude setup into $CLAUDE_HOME"
  if [ ! -d "$CLAUDE_HOME" ]; then
    if [ "$DRY" = 1 ]; then log "[dry-run] would create $CLAUDE_HOME (mode 700)"; else install -d -m 700 -- "$CLAUDE_HOME"; fi
  fi
  # CLAUDE.md = GI's file byte for byte + the marked VPS block
  base=$KIT_DIR/claude/CLAUDE.md
  [ -s "$base" ] || die "kit has no claude/CLAUDE.md"
  if grep -q 'BEGIN solar-rp-vps-kit' "$base"; then die "claude/CLAUDE.md already holds the VPS block markers"; fi
  composed=$SCRATCH/CLAUDE.md
  cat "$base" > "$composed"
  if [ -n "$(tail -c 1 "$base")" ]; then printf '\n' >> "$composed"; fi
  printf '\n' >> "$composed"
  cat "$KIT_DIR/claude-md-block.md" >> "$composed"
  C_NEW=0; C_UPD=0; C_SAME=0
  install_file "$composed" "$CLAUDE_HOME/CLAUDE.md"
  T_NEW=$((T_NEW + C_NEW)); T_UPD=$((T_UPD + C_UPD)); T_SAME=$((T_SAME + C_SAME))
  log "CLAUDE.md: $([ "$C_NEW" = 1 ] && echo 'installed' || { [ "$C_UPD" = 1 ] && echo 'replaced (the old one is backed up)' || echo 'already identical'; }) (GI's text unchanged, marked VPS section appended)"

  for d in rules skills agents commands tools hooks lessons-archive output-styles contexts; do
    install_tree "$KIT_DIR/claude/$d" "$CLAUDE_HOME/$d" "$d/"
  done
  C_NEW=0; C_UPD=0; C_SAME=0
  for f in statusline.js statusline.sh critic-rules.md; do
    if [ -f "$KIT_DIR/claude/$f" ]; then install_file "$KIT_DIR/claude/$f" "$CLAUDE_HOME/$f"; else warn "kit has no $f"; fi
  done
  T_NEW=$((T_NEW + C_NEW)); T_UPD=$((T_UPD + C_UPD)); T_SAME=$((T_SAME + C_SAME))
  log "top-level files (statusline.js, statusline.sh, critic-rules.md): $C_NEW new, $C_UPD replaced, $C_SAME identical"

  # settings.json: merge, never overwrite
  kitset=$KIT_DIR/claude/settings.json
  tgt=$CLAUDE_HOME/settings.json
  if [ ! -s "$kitset" ]; then
    warn "kit has no claude/settings.json"
  elif [ ! -e "$tgt" ]; then
    C_NEW=0; C_UPD=0; C_SAME=0
    install_file "$kitset" "$tgt"
    T_NEW=$((T_NEW + C_NEW))
    SETTINGS_RESULT='installed (the VPS had none)'
    log "settings.json: $([ "$DRY" = 1 ] && echo 'would be installed' || echo 'installed') (the VPS had none); dropped on the PC: see claude/SETTINGS-DROPPED.txt"
  elif [ "$NODE_OK" = 0 ]; then
    SETTINGS_RESULT='NOT merged (node missing)'
    warn "settings.json exists on the VPS and node 18+ is missing, so it cannot be merged yet. Nothing was changed. Run bootstrap-vps.sh (Node 22) and run this installer again; the sanitised copy is $kitset"
  else
    rc=0
    mout=$(node "$KIT_DIR/lib/merge-settings.mjs" "$kitset" "$tgt" "$SCRATCH/settings.merged" 2>&1) || rc=$?
    if [ "$rc" = 3 ]; then
      SETTINGS_RESULT='NOT merged (VPS file is not valid JSON)'
      warn "$tgt is not valid JSON: left untouched. Fix it by hand, then run this installer again (sanitised copy: $kitset)"
    elif [ "$rc" != 0 ]; then
      SETTINGS_RESULT='NOT merged (merge failed)'
      warn "settings.json merge failed (exit $rc): $(printf '%s' "$mout" | head -n 3 | tr '\n' ' ')"
    else
      first=$(printf '%s\n' "$mout" | head -n 1)
      printf '%s\n' "$mout" | tail -n +2 | while IFS= read -r l; do log "  settings.json $l"; done
      if [ "$first" = CHANGED ]; then
        if [ "$DRY" = 1 ]; then log "[dry-run] settings.json would be merged (backup: ${BACKUP_DIR##*/}/settings.json)"
        else
          backup_file "$tgt"
          cat "$SCRATCH/settings.merged" > "$tgt"
          log "settings.json: merged key by key (the previous file is in ${BACKUP_DIR##*/}/settings.json)"
        fi
        SETTINGS_RESULT='merged'; T_UPD=$((T_UPD + 1))
      else
        SETTINGS_RESULT='already merged'; T_SAME=$((T_SAME + 1))
        log "settings.json: nothing to merge, already contains everything in the kit"
      fi
    fi
  fi
fi

# ---- 3 memory ---------------------------------------------------------------------------------------------------------
log "== 3/6 project auto-memory"
if [ "$SKIP_CLAUDE" = 1 ] || [ "$SKIP_MEMORY" = 1 ]; then
  log "skipped"
elif [ ! -d "$KIT_DIR/claude/memory" ]; then
  warn "kit has no claude/memory"
else
  SLUG_A=$(path_slug "$PROJECT_DIR")
  SLUG_B=$(path_slug "$(dirname -- "$PROJECT_DIR")")
  log "Claude Code names a project folder after the directory it was started in. Installing the memory for $PROJECT_DIR ($SLUG_A) and for $(dirname -- "$PROJECT_DIR") ($SLUG_B), where docs/OPS.md section 1 step 5 starts it. If you start claude somewhere else, copy the folder to projects/<that path with every non-alphanumeric character as '-'>/memory."
  for s in "$SLUG_A" "$SLUG_B"; do
    install_tree "$KIT_DIR/claude/memory" "$CLAUDE_HOME/projects/$s/memory" "projects/$s/memory/"
  done
fi

# ---- 4 plugins --------------------------------------------------------------------------------------------------------
log "== 4/6 plugins"
PLUGIN_CMDS=()
PLUGIN_FAILS=0
if [ "$SKIP_CLAUDE" = 1 ]; then
  log "skipped (--skip-claude)"
else
  list=$KIT_DIR/plugins.list
  have_markets=''; have_plugins=''
  if [ -n "$CLAUDE_BIN" ]; then
    # CLAUDE_CONFIG_DIR is set only when the Claude folder is not the default one (tests): with it set, Claude Code keeps its
    # .claude.json inside that folder instead of ~/.claude.json, which a normal install must not change.
    CLAUDE_RUN=("$CLAUDE_BIN")
    if [ "$CLAUDE_HOME" != "${HOME:-/root}/.claude" ]; then CLAUDE_RUN=(env "CLAUDE_CONFIG_DIR=$CLAUDE_HOME" "$CLAUDE_BIN"); fi
    have_markets=$("${CLAUDE_RUN[@]}" plugin marketplace list 2>/dev/null || true)
    have_plugins=$("${CLAUDE_RUN[@]}" plugin list 2>/dev/null || true)
  fi
  while IFS=$'\t' read -r kind a b; do
    case $kind in ''|'#'*) continue ;; esac
    case $kind in
      marketplace)
        [[ $a =~ ^[A-Za-z0-9._-]+$ ]] && [[ $b =~ ^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$ ]] || { warn "plugins.list: bad marketplace line, skipped"; continue; }
        # here-strings, not `printf | grep -q`: grep -q exits early and pipefail would report the pipe's SIGPIPE as "not found"
        if grep -q -F "($b)" <<< "$have_markets"; then log "marketplace $a ($b): already added"; continue; fi
        PLUGIN_CMDS+=("plugin marketplace add --scope user $b") ;;
      plugin)
        [[ $a =~ ^[A-Za-z0-9._-]+@[A-Za-z0-9._-]+$ ]] || { warn "plugins.list: bad plugin line, skipped"; continue; }
        if grep -q -E "(^|[[:space:]])$a\$" <<< "$have_plugins"; then log "plugin $a: already installed"; continue; fi
        PLUGIN_CMDS+=("plugin install --scope user $a") ;;
      skip) log "not installed: $a ($b)" ;;
      *) warn "plugins.list: unknown line kind, skipped" ;;
    esac
  done < "$list"
  if [ ${#PLUGIN_CMDS[@]} -eq 0 ]; then
    log "nothing to add"
  elif [ -z "$CLAUDE_BIN" ] || [ "$NO_PLUGINS" = 1 ] || [ "$DRY" = 1 ]; then
    why='claude is not installed yet'
    [ -z "$CLAUDE_BIN" ] || why='--no-plugins'
    [ "$DRY" = 0 ] || [ -z "$CLAUDE_BIN" ] || why='dry run'
    log "plugin commands NOT run ($why). Run them once claude is installed and logged in:"
    for c in "${PLUGIN_CMDS[@]}"; do log "  claude $c"; done
  else
    for c in "${PLUGIN_CMDS[@]}"; do
      log "claude $c"
      # shellcheck disable=SC2086
      if timeout 300 "${CLAUDE_RUN[@]}" $c > "$SCRATCH/plugin.out" 2>&1; then
        log "  ok"
      else
        PLUGIN_FAILS=$((PLUGIN_FAILS + 1))
        warn "'claude $c' failed (first line: $(head -n 1 "$SCRATCH/plugin.out" | cut -c1-200)). Run it by hand later."
      fi
    done
  fi
fi
if [ -s "$KIT_DIR/mcp-servers.txt" ]; then
  log "MCP servers: $(sed -n 's/^user-scope servers on the PC: //p' "$KIT_DIR/mcp-servers.txt" | head -n 1) at user scope on the PC. Details and optional re-add lines: $KIT_DIR/mcp-servers.txt"
fi

# ---- 5 source ---------------------------------------------------------------------------------------------------------
log "== 5/6 source tree"
SOURCE_RESULT='skipped'
if [ "$SKIP_SOURCE" = 1 ]; then
  log "skipped (--skip-source)"
else
  tarball=''
  for f in "$KIT_DIR"/src/*.tar.gz; do [ -f "$f" ] && tarball=$f; done
  [ -n "$tarball" ] || die "no source tarball in $KIT_DIR/src"
  want=$(kit_info source_sha256)
  [[ $want =~ ^[0-9a-f]{64}$ ]] || die "kit-info.txt has no usable source_sha256"
  helper=$SCRATCH/unpack-helper
  mkdir -p -- "$helper"
  tar -xzf "$tarball" -C "$helper" ./tools/deploy || die "cannot read tools/deploy out of the source tarball"
  [ -s "$helper/tools/deploy/unpack-source.sh" ] || die "the source tarball has no tools/deploy/unpack-source.sh"
  args=("$tarball" --sha256 "$want")
  [ "$FORCE_SOURCE" = 0 ] || args+=(--force)
  [ "$DRY" = 0 ] || args+=(--dry-run)
  log "bash unpack-source.sh ${args[*]##*/}   (unpack-source.sh comes from the same tarball; it replaces $GAGE_OPT/src, keeps the old one as src.prev)"
  rc=0
  GAGE_OPT=$GAGE_OPT bash "$helper/tools/deploy/unpack-source.sh" "${args[@]}" || rc=$?
  if [ "$rc" != 0 ]; then
    done_note='The Claude setup above is already installed.'
    [ "$DRY" = 0 ] || done_note='This was a dry run: nothing was changed.'
    die "unpack-source.sh failed (exit $rc); read its first error line above. $done_note" "$rc"
  fi
  SOURCE_RESULT=$([ "$DRY" = 1 ] && echo 'checked (dry run)' || echo "in place at $GAGE_OPT/src (unpacked now, or it already was this tarball)")
fi

# ---- 6 report ---------------------------------------------------------------------------------------------------------
log "== 6/6 report"
if [ "$SKIP_CLAUDE" = 0 ] && [ "$DRY" = 0 ]; then
  lastkit=''
  [ ! -f "$CLAUDE_HOME/.kit-installs.log" ] || lastkit=$(tail -n 1 "$CLAUDE_HOME/.kit-installs.log" | sed -n 's/.* kit=\([^ ]*\) .*/\1/p')
  if [ $((T_NEW + T_UPD)) -gt 0 ] || [ "$lastkit" != "$STAMP" ]; then
    printf '%s kit=%s source_sha256=%s new=%s replaced=%s\n' "$RUN_STAMP" "$STAMP" "$(kit_info source_sha256)" "$T_NEW" "$T_UPD" >> "$CLAUDE_HOME/.kit-installs.log"
  fi
fi
if [ "$DRY" = 1 ]; then
  if [ "$T_UPD" -gt 0 ]; then log "backups: $T_UPD existing file(s) would be copied to $BACKUP_DIR before being replaced (nothing is ever deleted)"; else log "backups: none would be needed"; fi
elif [ "$BACKED_UP" -gt 0 ]; then log "backups: $BACKED_UP file(s) in $BACKUP_DIR (nothing was deleted)"
else log "backups: none needed"; fi
log "totals: $T_NEW new, $T_UPD replaced, $T_SAME already identical; settings.json: $SETTINGS_RESULT; source: $SOURCE_RESULT"

echo
echo "================ what next (the kit ran none of this) ================"
if [ -n "$CLAUDE_BIN" ]; then
  cv=$("$CLAUDE_BIN" --version 2>/dev/null | head -n 1 || true)
  echo "Claude Code: installed (${cv:-version unknown}). If you have not logged in on this machine yet, run: claude"
else
  echo "Claude Code is not installed. Official installer (docs: https://code.claude.com/docs/en/setup, native install for Linux):"
  echo "    curl -fsSL https://claude.ai/install.sh | bash"
  echo "  then open a new shell, run 'claude' once to log in, then run this installer again (it adds the plugins)."
fi
if [ -f "$ENV_FILE" ]; then
  echo "Machine setup: $ENV_FILE exists, so bootstrap-vps.sh has run before."
  if ! grep -q -E '^[[:space:]]*GAGE_TEST_SERVER=[^[:space:]]' "$ENV_FILE" && ! grep -q -E '^[[:space:]]*GAGE_API_BASE=[^[:space:]]' "$ENV_FILE"; then
    echo "  HINT: $ENV_FILE has neither GAGE_TEST_SERVER nor GAGE_API_BASE set. Add this line to it BY HAND before build-client.sh"
    echo "  (build-client.sh refuses a launcher that would refuse every PLAY with code 13; this kit does not edit the file):"
    echo "    GAGE_TEST_SERVER=5.175.253.187:24800"
  fi
else
  echo "Machine setup: $ENV_FILE is missing, so this machine is not bootstrapped. Next step, after the Hetzner Cloud Firewall (tcp 22, 80, 443, udp 24800) and DNS are ready:"
  echo "    bash $GAGE_OPT/src/tools/deploy/bootstrap-vps.sh --dry-run     # read it"
  echo "    bash $GAGE_OPT/src/tools/deploy/bootstrap-vps.sh               # then run it"
fi
echo "Update signing key: it is NOT in this kit. From your PC, in the folder that holds it:"
echo "    scp ./update-signing-key.pem ./update-signing-pub.txt root@5.175.253.187:/root/.gage-keys/"
echo "    ssh root@5.175.253.187 'chmod 600 /root/.gage-keys/update-signing-key.pem && chmod 644 /root/.gage-keys/update-signing-pub.txt'"
if [ -s "$KEY_DIR/update-signing-key.pem" ]; then
  kmode=$(stat -c %a "$KEY_DIR/update-signing-key.pem" 2>/dev/null || echo '?')
  echo "  status here: update-signing-key.pem present, mode $kmode$([ "$kmode" = 600 ] || echo ' (should be 600)')"
else
  echo "  status here: update-signing-key.pem not found in $KEY_DIR yet"
fi
echo "Start the Claude session in the repository (loads /opt/gage/CLAUDE.md and the repo's CLAUDE.md, memory is installed for it):"
echo "    cd $GAGE_OPT/src && claude"
echo "Then, from $GAGE_OPT/src (every script takes --dry-run; run that first; docs/OPS.md has the reasons):"
echo "    client release:  bash tools/deploy/build-client.sh --dry-run  ->  build-client.sh --tests soft  ->  publish-client.sh <ver> --dry-run  ->  publish-client.sh <ver> --notes \"...\""
echo "    website:         bash tools/deploy/deploy-site.sh --dry-run  ->  deploy-site.sh   (undo: deploy-site.sh --rollback)"
echo "    game server:     bash tools/deploy/deploy-server.sh --dry-run  ->  deploy-server.sh   (exit 3 = players online: ask GI; first cutover from tmux: OPS.md section 3)"
echo "Refresh this setup later: build a new kit on the PC (bash tools/deploy/pack-vps-kit.sh), scp it, run: bash solar-rp-vps-kit-<new>.run --dry-run, then without --dry-run."
echo "======================================================================"
if [ "$WARNINGS" -gt 0 ]; then log "finished with $WARNINGS warning(s) (shown above)"; else log "finished, no warnings"; fi
exit 0
