#!/usr/bin/env bash
# Pack the VPS kit: ONE self-extracting file with the source tree, GI's Claude setup (skills, rules, agents, commands, tools,
# memory, a sanitised settings.json, the plugin list) and the installer. Run on GI's PC in Git Bash, from anywhere (it finds the
# repository from its own location).
#
#   pack-vps-kit.sh [--out DIR] [--name NAME] [--claude-home DIR] [--claude-json FILE] [--memory-dir DIR]
#                   [--src-tarball FILE] [--keep-env NAME,NAME] [--list] [--dry-run]
#
#   --out DIR          where solar-rp-vps-kit-<UTC stamp>.run goes (default: the folder ABOVE the repository, never inside it)
#   --name NAME        file name without extension (default solar-rp-vps-kit-<UTC stamp>)
#   --claude-home DIR  the Claude config folder to take the setup from (default $CLAUDE_CONFIG_DIR, else ~/.claude)
#   --claude-json FILE ~/.claude.json, read ONLY for MCP server names and commands (default: next to the Claude folder)
#   --memory-dir DIR   the project auto-memory folder (default: <claude-home>/projects/<repo path with every non-alphanumeric
#                      character as '-'>/memory)
#   --src-tarball FILE use this pack-source.sh tarball instead of making a fresh one (tests; it is checked the same way)
#   --keep-env A,B     keep these settings.json env entries when the name is not credential-like and the value is a short plain
#                      flag (default: the whole env block is dropped)
#   --list             stage the Claude part, print its file list and stop (no source pack, no .run)
#   --dry-run          do every check, print the summary, write no .run (the source part is only list-checked)
#
# Parts: (1) src/ - a fresh tools/deploy/pack-source.sh tarball; (2) claude/ - CLAUDE.md, rules/, skills/, agents/, commands/,
# tools/, hooks/, lessons-archive/, output-styles/, contexts/, statusline files, critic-rules.md, a sanitised settings.json and the
# project memory; (3) plugins.list and mcp-servers.txt (the plugin caches and ~/.claude.json are NOT copied: the installer
# re-installs plugins with `claude plugin ...`); (4) install.sh and its helpers. The .run header carries the SHA-256 and size of
# the payload and refuses to unpack on a mismatch.
# NEVER inside: .credentials.json, ~/.claude.json, history, transcripts, sessions, todos, shell snapshots, caches, *.pem, *.key,
# .env*, id_rsa*, anything named *token* or *secret* in the Claude part, .gage-keys, the update signing key, a private-key block, an
# Anthropic/GitHub/AWS/Slack/Google key, a JWT, an api-key literal. The pack STOPS (exit 1) when any of that is found, in the staged
# files, in the source tarball, in the payload listing or in the finished .run unpacked again. See vps-kit/kit-patterns.mjs.
# Exit codes: 0 ok, 1 failure or forbidden content, 2 usage.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
KITSRC=$ROOT/tools/deploy/vps-kit
VPS_HOST=${VPS_HOST:-root@5.175.253.187}
NODE=${NODE:-node}
OUT_DIR=''; NAME=''; CLAUDE_HOME=''; CLAUDE_JSON=''; MEMORY_DIR=''; SRC_TARBALL=''; KEEP_ENV=''; LIST_ONLY=0; DRY=0
log()  { printf '[pack-vps-kit] %s\n' "$*"; }
warn() { printf '[pack-vps-kit] WARN: %s\n' "$*" >&2; }
die()  { printf '[pack-vps-kit] ERROR: %s\n' "$1" >&2; exit "${2:-1}"; }

while [ $# -gt 0 ]; do
  case $1 in
    --out) [ $# -ge 2 ] || die "--out needs a folder" 2; OUT_DIR=$2; shift 2 ;;
    --name) [ $# -ge 2 ] || die "--name needs a value" 2; NAME=$2; shift 2 ;;
    --claude-home) [ $# -ge 2 ] || die "--claude-home needs a folder" 2; CLAUDE_HOME=$2; shift 2 ;;
    --claude-json) [ $# -ge 2 ] || die "--claude-json needs a file" 2; CLAUDE_JSON=$2; shift 2 ;;
    --memory-dir) [ $# -ge 2 ] || die "--memory-dir needs a folder" 2; MEMORY_DIR=$2; shift 2 ;;
    --src-tarball) [ $# -ge 2 ] || die "--src-tarball needs a file" 2; SRC_TARBALL=$2; shift 2 ;;
    --keep-env) [ $# -ge 2 ] || die "--keep-env needs NAME,NAME" 2; KEEP_ENV=$2; shift 2 ;;
    --list) LIST_ONLY=1; shift ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option $1 (try --help)" 2 ;;
  esac
done

[ -f "$ROOT/CMakeLists.txt" ] && [ -d "$ROOT/tools/deploy" ] || die "$ROOT does not look like the repository root"
for f in install.sh run-header.sh merge-settings.mjs sanitize-settings.mjs kit-lists.mjs kit-scan.mjs kit-patterns.mjs vps-linux-rule.md claude-md-block.md; do
  [ -s "$KITSRC/$f" ] || die "missing $KITSRC/$f"
done
[ -f "$ROOT/tools/deploy/pack-source.sh" ] || die "missing tools/deploy/pack-source.sh"
command -v "$NODE" >/dev/null 2>&1 || die "node is needed (set NODE=/path/to/node)"
nv=$("$NODE" --version); nmajor=${nv#v}; nmajor=${nmajor%%.*}
[ "$nmajor" -ge 18 ] 2>/dev/null || die "node $nv is too old (need 18 or newer)"
for c in tar gzip sha256sum find sed grep awk mktemp xargs cut; do command -v "$c" >/dev/null 2>&1 || die "missing command: $c"; done

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
[ -n "$NAME" ] || NAME="solar-rp-vps-kit-$STAMP"
[[ $NAME =~ ^[A-Za-z0-9._-]+$ ]] || die "--name may only contain letters, digits, . _ -" 2
[ -n "$OUT_DIR" ] || OUT_DIR=$(dirname "$ROOT")
mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
case "$OUT_DIR/" in "$ROOT"/*) die "--out must not be inside the repository ($OUT_DIR)" 2 ;; esac
OUT=$OUT_DIR/$NAME.run
[ ! -e "$OUT" ] || die "$OUT already exists; pick another --name"

[ -n "$CLAUDE_HOME" ] || CLAUDE_HOME=${CLAUDE_CONFIG_DIR:-$HOME/.claude}
[ -d "$CLAUDE_HOME" ] || die "no Claude folder at $CLAUDE_HOME (use --claude-home)"
for need in CLAUDE.md settings.json rules skills agents; do
  [ -e "$CLAUDE_HOME/$need" ] || die "$CLAUDE_HOME/$need is missing: this does not look like GI's Claude folder"
done
if [ -z "$CLAUDE_JSON" ]; then CLAUDE_JSON=$(dirname "$CLAUDE_HOME")/.claude.json; fi

# project memory folder: Claude Code names it after the project path, every non-alphanumeric character becoming '-'
repo_win=$ROOT
if command -v cygpath >/dev/null 2>&1; then repo_win=$(cygpath -w "$ROOT"); fi
REPO_SLUG=$(printf '%s' "$repo_win" | sed 's/[^A-Za-z0-9]/-/g')
if [ -z "$MEMORY_DIR" ]; then MEMORY_DIR=$CLAUDE_HOME/projects/$REPO_SLUG/memory; fi
[ -f "$MEMORY_DIR/MEMORY.md" ] || die "no MEMORY.md in $MEMORY_DIR (the computed project folder was '$REPO_SLUG'; use --memory-dir)"
log "project memory: $MEMORY_DIR (folder name '$REPO_SLUG' computed from the repository path by the 'every non-alphanumeric character becomes -' rule)"

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/pack-vps-kit.XXXXXX")
trap 'rm -rf -- "$SCRATCH"' EXIT
STAGE=$SCRATCH/kit
mkdir -p "$STAGE/claude" "$STAGE/lib" "$STAGE/src"

# ---- the kit's own scripts: syntax, line endings -----------------------------------------------------------------------------
no_cr() { # no_cr FILE...: fail when a file holds a carriage return (tr + wc, not grep: grep matches every line under Git Bash)
  local f
  for f in "$@"; do
    if [ "$(tr -d '\r' < "$f" | wc -c)" != "$(wc -c < "$f")" ]; then die "CRLF line endings in $f (convert with: sed -i 's/\\r\$//' <file>)"; fi
  done
}
for f in "$KITSRC"/*.sh "$ROOT/tools/deploy/pack-vps-kit.sh"; do
  bash -n "$f" || die "bash -n fails for $f"
done
no_cr "$KITSRC"/*.sh "$KITSRC"/*.mjs "$KITSRC"/*.md "$ROOT/tools/deploy/pack-vps-kit.sh"

# ---- stage the Claude part ---------------------------------------------------------------------------------------------------
copy_tree() { # copy_tree SRC DST: a whole folder, without node_modules/.git, as plain files
  mkdir -p "$2"
  ( cd "$1" && tar --exclude=node_modules --exclude=.git --exclude=.DS_Store -cf - . ) | ( cd "$2" && tar -xf - --no-same-owner )
}
SKIPPED=()
for d in rules skills agents commands tools hooks lessons-archive output-styles contexts; do
  if [ -d "$CLAUDE_HOME/$d" ]; then copy_tree "$CLAUDE_HOME/$d" "$STAGE/claude/$d"; else SKIPPED+=("$d/ (not present)"); fi
done
for f in CLAUDE.md statusline.js statusline.sh critic-rules.md; do
  if [ -f "$CLAUDE_HOME/$f" ]; then cp -- "$CLAUDE_HOME/$f" "$STAGE/claude/$f"; else SKIPPED+=("$f (not present)"); fi
done
[ -s "$STAGE/claude/CLAUDE.md" ] || die "CLAUDE.md is empty or missing"
if grep -q 'BEGIN solar-rp-vps-kit' "$STAGE/claude/CLAUDE.md"; then die "CLAUDE.md already holds the kit's VPS block markers"; fi
[ ! -e "$STAGE/claude/rules/00-vps-linux.md" ] || die "rules/00-vps-linux.md already exists in the Claude folder; the kit installs its own under that name"
mkdir -p "$STAGE/claude/rules"
cp -- "$KITSRC/vps-linux-rule.md" "$STAGE/claude/rules/00-vps-linux.md"
copy_tree "$MEMORY_DIR" "$STAGE/claude/memory"
cp -- "$KITSRC/claude-md-block.md" "$STAGE/claude-md-block.md"
cp -- "$KITSRC/install.sh" "$STAGE/install.sh"
cp -- "$KITSRC/merge-settings.mjs" "$STAGE/lib/merge-settings.mjs"

if [ -n "$(find "$STAGE" -type l)" ]; then die "symbolic links in the staged kit: $(find "$STAGE" -type l | head -n 3 | tr '\n' ' ')"; fi
# shell scripts that will run on Linux: LF only, and they parse
while IFS= read -r -d '' f; do
  no_cr "$f"; bash -n "$f" || die "bash -n fails for ${f#"$STAGE"/}"
done < <(find "$STAGE" -type f -name '*.sh' -print0)

# settings.json: sanitised copy + the list of what was dropped (names only)
log "settings.json:"
san_args=("$CLAUDE_HOME/settings.json" "$STAGE/claude/settings.json" "$STAGE/claude/SETTINGS-DROPPED.txt")
if [ -n "$KEEP_ENV" ]; then san_args+=(--keep-env "$KEEP_ENV"); fi
"$NODE" "$KITSRC/sanitize-settings.mjs" "${san_args[@]}" | sed 's/^/    /' || die "sanitize-settings.mjs failed"

# plugins and MCP servers: commands and names, never caches or secrets
claude_json_in=$CLAUDE_JSON
if [ ! -f "$claude_json_in" ]; then warn "no $CLAUDE_JSON: mcp-servers.txt will say there is nothing to list"; printf '{}\n' > "$SCRATCH/empty.claude.json"; claude_json_in=$SCRATCH/empty.claude.json; fi
"$NODE" "$KITSRC/kit-lists.mjs" "$CLAUDE_HOME" "$claude_json_in" "$STAGE/claude/settings.json" "$STAGE/plugins.list" "$STAGE/mcp-servers.txt" || die "kit-lists.mjs failed"
grep -E '^(marketplace|plugin|skip)' "$STAGE/plugins.list" | tr '\t' ' ' | sed 's/^/    /'

# ---- scan the staged files (everything except the source tarball) --------------------------------------------------------------
if ! "$NODE" "$KITSRC/kit-scan.mjs" "$STAGE" --strict --words --label "staged kit (claude part and scripts)"; then
  die "forbidden content in the staged kit (names above; the pack was stopped, no .run exists)"
fi

count_in() { find "$1" -type f 2>/dev/null | wc -l | tr -d ' '; }
if [ "$LIST_ONLY" = 1 ]; then
  ( cd "$STAGE" && find . -type f | LC_ALL=C sort )
  [ ${#SKIPPED[@]} -eq 0 ] || warn "not present on this PC: ${SKIPPED[*]}"
  exit 0
fi

# ---- the source tarball ------------------------------------------------------------------------------------------------------
SRC_NAME=solar-rp-src-$STAMP
SRC_ENTRIES='n/a (dry run)'
SRC_SHA=''
if [ "$DRY" = 1 ] && [ -z "$SRC_TARBALL" ]; then
  log "source: pack-source.sh --dry-run (list and checks only)"
  bash "$ROOT/tools/deploy/pack-source.sh" --dry-run --out "$SCRATCH/srcdry" --name "$SRC_NAME" > "$SCRATCH/pack-source.log" 2>&1 \
    || { cat "$SCRATCH/pack-source.log" >&2; die "pack-source.sh --dry-run failed (its output is above)"; }
  { grep -E 'files,|top-level' "$SCRATCH/pack-source.log" || true; } | sed 's/^/    /'
else
  if [ -n "$SRC_TARBALL" ]; then
    [ -f "$SRC_TARBALL" ] || die "no such file: $SRC_TARBALL"
    cp -- "$SRC_TARBALL" "$STAGE/src/$SRC_NAME.tar.gz"
    log "source: using $SRC_TARBALL (--src-tarball), checked below like a fresh one"
  else
    log "source: tools/deploy/pack-source.sh"
    bash "$ROOT/tools/deploy/pack-source.sh" --out "$STAGE/src" --name "$SRC_NAME" > "$SCRATCH/pack-source.log" 2>&1 \
      || { cat "$SCRATCH/pack-source.log" >&2; die "pack-source.sh failed (its output is above)"; }
    { grep -E '^\[pack-source\] ([0-9]+ files,|wrote |sha256 )' "$SCRATCH/pack-source.log" || true; } | sed 's/^/    /'
  fi
  SRCF=$STAGE/src/$SRC_NAME.tar.gz
  [ -s "$SRCF" ] || die "no source tarball at $SRCF"
  tar -tzf "$SRCF" > "$SCRATCH/src.list" || die "the source tarball is not a readable .tar.gz"
  SRC_ENTRIES=$(wc -l < "$SCRATCH/src.list" | tr -d ' ')
  [ "$SRC_ENTRIES" -gt 500 ] || die "only $SRC_ENTRIES entries in the source tarball"
  for need in ./CMakeLists.txt ./tools/deploy/unpack-source.sh ./tools/deploy/lib/common.sh ./tools/deploy/bootstrap-vps.sh ./tools/deploy/deploy-site.sh ./tools/deploy/VPS-CLAUDE.md ./docs/OPS.md; do
    grep -q -x -F "$need" "$SCRATCH/src.list" || die "the source tarball lacks $need"
  done
  if ! "$NODE" "$KITSRC/kit-scan.mjs" --names-from "$SCRATCH/src.list" --label "source tarball names"; then die "forbidden file names in the source tarball"; fi
  mkdir -p "$SCRATCH/srcx"
  tar -xzf "$SRCF" -C "$SCRATCH/srcx" --no-same-owner || die "cannot extract the source tarball to scan it"
  if ! "$NODE" "$KITSRC/kit-scan.mjs" "$SCRATCH/srcx" --lenient --label "source tarball content"; then die "forbidden content in the source tarball"; fi
  SRC_SHA=$(sha256sum "$SRCF" | cut -d' ' -f1)
fi

# ---- kit-info, manifest, payload ---------------------------------------------------------------------------------------------------
CLIENT_VERSION=$(sed -n 's/.*kClientVersion = L"\([^"]*\)".*/\1/p' "$ROOT/apps/launcher/gage_gui.cpp" | head -n 1)
[[ $CLIENT_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "no usable kClientVersion in apps/launcher/gage_gui.cpp"
N_CLAUDE=$(count_in "$STAGE/claude")
N_MEMORY=$(count_in "$STAGE/claude/memory")
cat > "$STAGE/kit-info.txt" <<EOF
kit=solar-rp-vps-kit
stamp=$STAMP
client_version=$CLIENT_VERSION
source_tarball=$SRC_NAME.tar.gz
source_sha256=$SRC_SHA
source_entries=$SRC_ENTRIES
claude_files=$N_CLAUDE
memory_files=$N_MEMORY
packer=tools/deploy/pack-vps-kit.sh
EOF

summary() {
  local d n_all n_src n_plug
  n_all=$(count_in "$STAGE")
  n_src=$(count_in "$STAGE/src")
  n_plug=$(grep -c -E '^(marketplace|plugin)' "$STAGE/plugins.list" || true)
  log "parts: source tarball $SRC_ENTRIES entries | claude config $((N_CLAUDE - N_MEMORY)) files | project memory $N_MEMORY files | plugins.list $n_plug install lines | installer, helpers, lists, info $((n_all - N_CLAUDE - n_src)) files"
  for d in rules skills agents commands tools hooks lessons-archive output-styles contexts; do
    if [ -d "$STAGE/claude/$d" ]; then log "  claude/$d/: $(count_in "$STAGE/claude/$d") files"; fi
  done
  if [ ${#SKIPPED[@]} -gt 0 ]; then warn "not present on this PC, so not in the kit: ${SKIPPED[*]}"; fi
  return 0
}

if [ "$DRY" = 1 ]; then
  summary
  log "--dry-run: every check passed, no .run written. It would be $OUT"
  exit 0
fi

( cd "$STAGE" && find . -type f ! -name MANIFEST.sha256 -print0 | LC_ALL=C sort -z | xargs -0 sha256sum > MANIFEST.sha256 )
[ -s "$STAGE/MANIFEST.sha256" ] || die "could not write MANIFEST.sha256"
PAYLOAD=$SCRATCH/payload.tgz
tar --force-local --owner=0 --group=0 --numeric-owner --sort=name -czf "$PAYLOAD" -C "$STAGE" . || die "tar failed"
tar --force-local -tzf "$PAYLOAD" | sed 's#^\./##' | grep -v '^$' > "$SCRATCH/payload.list"
if ! "$NODE" "$KITSRC/kit-scan.mjs" --names-from "$SCRATCH/payload.list" --label "payload member names"; then die "a forbidden name is in the payload"; fi
PSHA=$(sha256sum "$PAYLOAD" | cut -d' ' -f1)
PBYTES=$(wc -c < "$PAYLOAD" | tr -d ' ')

# ---- the .run: header + payload ----------------------------------------------------------------------------------------------------
sed -e "s/@@STAMP@@/$STAMP/" -e "s/@@SHA256@@/$PSHA/" -e "s/@@BYTES@@/$PBYTES/" "$KITSRC/run-header.sh" > "$SCRATCH/header.sh"
if grep -q '@@' "$SCRATCH/header.sh"; then die "a placeholder is left in the header"; fi
no_cr "$SCRATCH/header.sh"
bash -n "$SCRATCH/header.sh" || die "bash -n fails for the finished header"
[ "$(tail -n 1 "$SCRATCH/header.sh")" = "__SOLAR_RP_KIT_PAYLOAD_BELOW__" ] || die "the header does not end with the payload marker"
cat "$SCRATCH/header.sh" "$PAYLOAD" > "$OUT.partial"
mv -T -- "$OUT.partial" "$OUT"
RUN_SHA=$(sha256sum "$OUT" | cut -d' ' -f1)
RUN_BYTES=$(wc -c < "$OUT" | tr -d ' ')

# ---- check the REAL artifact: verify it, unpack it again, scan and checksum what came out ------------------------------------------------
fail_artifact() { rm -f -- "$OUT"; die "$1 (the .run was deleted)"; }
bash "$OUT" --verify > "$SCRATCH/verify.log" 2>&1 || { cat "$SCRATCH/verify.log" >&2; fail_artifact "the finished .run does not verify"; }
KIT_EXTRACT_ROOT=$SCRATCH/check bash "$OUT" --extract-only > "$SCRATCH/extract.log" 2>&1 || { cat "$SCRATCH/extract.log" >&2; fail_artifact "the finished .run does not unpack"; }
CHECKED=$SCRATCH/check/$STAMP
( cd "$CHECKED" && sha256sum -c --quiet MANIFEST.sha256 ) > "$SCRATCH/manifest.log" 2>&1 || { head -n 5 "$SCRATCH/manifest.log" >&2; fail_artifact "the unpacked kit does not match its MANIFEST.sha256"; }
if ! "$NODE" "$KITSRC/kit-scan.mjs" "$CHECKED" --strict --label "finished .run, unpacked again" > "$SCRATCH/final-scan.log" 2>&1; then
  cat "$SCRATCH/final-scan.log" >&2; fail_artifact "forbidden content in the finished .run"
fi
head -n 1 "$SCRATCH/final-scan.log" | sed 's/^/    /'
[ "$(count_in "$CHECKED")" -ge 1 ] || fail_artifact "the unpacked kit is empty"

summary
log "wrote $OUT ($((RUN_BYTES / 1048576)) MiB, $RUN_BYTES bytes)"
log "sha256 of the file $RUN_SHA; payload sha256 $PSHA (the header refuses to unpack on a mismatch)"
cat <<EOF

Next, from this Git Bash window. scp reads "H:" as a host name, so go to the folder and use a relative name:

  cd "$OUT_DIR"
  scp ./$NAME.run $VPS_HOST:/root/

Then, on the VPS (the kit unpacks itself to /root/solar-rp-kit/$STAMP/ and installs from there):
  ssh $VPS_HOST 'bash /root/$NAME.run --dry-run'     # reads the plan, changes nothing
  ssh $VPS_HOST 'bash /root/$NAME.run'               # installs; run it again with a newer kit any time

Optional check that the upload arrived whole (the .run also checks its own payload):
  ssh $VPS_HOST 'echo "$RUN_SHA  /root/$NAME.run" | sha256sum -c -'

The update signing key is NOT in the kit. Copy it yourself, from the folder that holds it:
  scp ./update-signing-key.pem ./update-signing-pub.txt $VPS_HOST:/root/.gage-keys/
  ssh $VPS_HOST 'chmod 600 /root/.gage-keys/update-signing-key.pem'
EOF
