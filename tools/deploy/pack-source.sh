#!/usr/bin/env bash
# Pack the source tree into a tarball for the VPS. Run on GI's PC in Git Bash, from anywhere (it finds the repo from its
# own location). The repository is not a git repository, so the file list is built from the working tree.
#
#   pack-source.sh [--out DIR] [--name NAME] [--list] [--dry-run]
#
#   --out DIR     where the tarball goes (default: the folder ABOVE the repository, never inside it)
#   --name NAME   file name without extension (default: solar-rp-src-<UTC stamp>)
#   --list        print every file that would be packed (and stop)
#   --dry-run     build and check the file list, print the summary, write no tarball
#
# Excluded: build/ (all build output), every node_modules and .impeccable, .git, logs/, "enhanced EXE/" (GTA game files must never
# leave the PC), everything in .toolchain/ EXCEPT innosetup-6.7.3 (the VPS runs it under wine; WinLibs, ffmpeg
# and the installers stay home), .env*, *.pem, *.key, update-signing-key*, .gage-keys, *.log, *.tgz, *.tar.gz.
# After the list is built it is CHECKED: nothing forbidden may be in it, the files the VPS needs must be, and no text file may
# contain a private key block. Any failure stops before a tarball exists.
#
# The tarball is non-overlay by design: tools/deploy/unpack-source.sh swaps the whole tree on the VPS, so files you deleted
# here are gone there too. Entries are owned by root (numeric 0), so they extract as root:root on the VPS.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT_DIR=''; NAME=''; LIST_ONLY=0; DRY=0
log()  { printf '[pack-source] %s\n' "$*"; }
die()  { printf '[pack-source] ERROR: %s\n' "$1" >&2; exit "${2:-1}"; }

while [ $# -gt 0 ]; do
  case $1 in
    --out) [ $# -ge 2 ] || die "--out needs a folder" 2; OUT_DIR=$2; shift 2 ;;
    --name) [ $# -ge 2 ] || die "--name needs a value" 2; NAME=$2; shift 2 ;;
    --list) LIST_ONLY=1; shift ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option $1 (try --help)" 2 ;;
  esac
done

[ -f "$ROOT/CMakeLists.txt" ] && [ -d "$ROOT/tools/deploy" ] || die "$ROOT does not look like the repository root"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
[ -n "$NAME" ] || NAME="solar-rp-src-$STAMP"
[[ $NAME =~ ^[A-Za-z0-9._-]+$ ]] || die "--name may only contain letters, digits, . _ -" 2
[ -n "$OUT_DIR" ] || OUT_DIR=$(dirname "$ROOT")
mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)
case "$OUT_DIR/" in "$ROOT"/*) die "--out must not be inside the repository ($OUT_DIR)" 2 ;; esac
OUT="$OUT_DIR/$NAME.tar.gz"
[ ! -e "$OUT" ] || die "$OUT already exists; pick another --name"

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/pack-source.XXXXXX")
trap 'rm -rf -- "$SCRATCH"' EXIT
LISTFILE=$SCRATCH/files.list0

# ---- the file list ---------------------------------------------------------------------------------------
cd "$ROOT"
# Directories pruned early (find would otherwise walk 280 MB of node_modules and the WinLibs toolchain).
# .toolchain: every direct child directory except innosetup-6.7.3 is pruned; loose files are filtered below.
find . \
  \( -type d \( -name node_modules -o -name .git -o -name .gage-keys -o -path ./build -o -path ./logs \
       -o -path './enhanced EXE' -o -name .impeccable \
       -o \( -path './.toolchain/*' ! -path './.toolchain/innosetup-6.7.3' ! -path './.toolchain/innosetup-6.7.3/*' \) \) -prune \) \
  -o \( \( -type f -o -type l \) \
        ! -name '.env*' ! -name '*.pem' ! -name '*.key' ! -name 'update-signing-key*' ! -name '*.log' \
        ! -name '*.tgz' ! -name '*.tar.gz' ! -name '.DS_Store' ! -name 'Thumbs.db' \
        -print0 \) \
  | LC_ALL=C sort -z > "$LISTFILE"
# loose files directly in .toolchain/ (zips, installers, nupkg) are not part of the source
tr '\0' '\n' < "$LISTFILE" | grep -v -E '^\./\.toolchain/[^/]+$' | tr '\n' '\0' > "$LISTFILE.2"
mv -f "$LISTFILE.2" "$LISTFILE"

COUNT=$(tr -cd '\0' < "$LISTFILE" | wc -c)
[ "$COUNT" -gt 500 ] || die "only $COUNT files in the list: something is wrong with the file selection"
tr '\0' '\n' < "$LISTFILE" > "$SCRATCH/files.txt"

if [ "$LIST_ONLY" = 1 ]; then cat "$SCRATCH/files.txt"; exit 0; fi

# ---- checks on the list (defence in depth: the find rules above must have done their job) -----------------
bad=$(grep -E -i '(^|/)(node_modules|\.git|\.gage-keys|\.impeccable|enhanced EXE|winlibs|ffmpeg)(/|$)|\.pem$|\.key$|(^|/)\.env|update-signing-key|^\./build/|^\./logs/|\.tgz$|\.tar\.gz$' "$SCRATCH/files.txt" || true)
[ -z "$bad" ] || { printf '%s\n' "$bad" | head -n 20 >&2; die "forbidden files are in the list (shown above)"; }

need=(CMakeLists.txt cmake/toolchain-mingw-linux.cmake tools/deploy/build-client.sh tools/deploy/publish-client.sh tools/deploy/deploy-server.sh tools/deploy/release.sh
      tools/deploy/bootstrap-vps.sh tools/deploy/unpack-source.sh tools/deploy/lib/common.sh tools/deploy/wine-selftests.list
      tools/update-sign/sign.mjs installer/build-installer.sh installer/gage-mp.iss apps/launcher/gage_gui.cpp apps/launcher-ui/package.json
      apps/server/CMakeLists.txt .toolchain/innosetup-6.7.3/ISCC.exe docs/OPS.md)
for f in "${need[@]}"; do
  grep -q -x -F "./$f" "$SCRATCH/files.txt" || die "the VPS needs ./$f but it is not in the list (or does not exist)"
done
grep -q '^\./installer/art/out/wizard-.*\.png$' "$SCRATCH/files.txt" || die "installer/art/out/wizard-*.png missing: run 'node installer/art/render.mjs' first"
grep -q '^\./\.toolchain/innosetup-6.7.3/' "$SCRATCH/files.txt" || die "Inno Setup is not in the list"
if grep -q '^\./\.toolchain/winlibs' "$SCRATCH/files.txt"; then die "WinLibs would be packed"; fi

# A private key block in any text file stops the pack (grep -I skips binaries).
secrets=$(tr '\n' '\0' < "$SCRATCH/files.txt" | xargs -0 grep -l -I -E 'BEGIN [A-Z ]*PRIVATE KEY' 2>/dev/null || true)
[ -z "$secrets" ] || { printf '%s\n' "$secrets" >&2; die "a private key block is inside the files above; it must not leave the PC"; }

# CRLF in shell scripts breaks them on Linux (the VPS scripts check this too).
crlf=''
while IFS= read -r f; do
  case $f in *.sh|./tools/deploy/shim/*)
    if [ "$(tr -d '\r' < "$f" | wc -c)" != "$(wc -c < "$f")" ]; then crlf="$crlf $f"; fi ;;
  esac
done < "$SCRATCH/files.txt"
[ -z "$crlf" ] || die "CRLF line endings in:$crlf (convert with: sed -i 's/\\r\$//' <file>)"

BYTES=0
while IFS= read -r f; do BYTES=$((BYTES + $(wc -c < "$f"))); done < "$SCRATCH/files.txt"
log "$COUNT files, $((BYTES / 1048576)) MiB uncompressed; Inno Setup included, WinLibs/ffmpeg/build/node_modules/game files/keys excluded"
log "top-level folders: $(cut -d/ -f2 "$SCRATCH/files.txt" | sort -u | tr '\n' ' ')"

if [ "$DRY" = 1 ]; then
  log "--dry-run: no tarball written. It would be $OUT"
  exit 0
fi

# ---- the tarball -----------------------------------------------------------------------------------------
# --force-local: the output path contains "H:"; without it tar reads that as host:path and fails.
tar --force-local --null -T "$LISTFILE" --owner=0 --group=0 --numeric-owner --sort=name -czf "$OUT" \
  || { rm -f -- "$OUT"; die "tar failed"; }
GOT=$(tar --force-local -tzf "$OUT" | wc -l)
[ "$GOT" = "$COUNT" ] || { rm -f -- "$OUT"; die "tarball holds $GOT entries, the list had $COUNT"; }
# negative control on the real archive: nothing forbidden in it
if tar --force-local -tzf "$OUT" | grep -q -E '\.pem$|(^|/)\.env|node_modules/|winlibs|enhanced EXE|^\./build/'; then
  rm -f -- "$OUT"; die "the finished tarball contains a forbidden path"
fi
SHA=$(sha256sum "$OUT" | cut -d' ' -f1)
SIZE=$(wc -c < "$OUT")
log "wrote $OUT ($((SIZE / 1048576)) MiB, $GOT entries)"
log "sha256 $SHA"

cat <<EOF

Next, from this Git Bash window. scp reads "H:" as a host name, so go to the folder and use a relative name:

  cd "$OUT_DIR"
  scp ./$NAME.tar.gz root@5.175.253.187:/root/

FIRST TIME (nothing on the VPS yet). Open the Hetzner Cloud Firewall FIRST (inbound tcp 22, 80, 443 and udp 24800), then:
  1) ssh root@5.175.253.187 'echo "$SHA  /root/$NAME.tar.gz" | sha256sum -c - && mkdir -p /opt/gage/src && tar -xzf /root/$NAME.tar.gz -C /opt/gage/src --no-same-owner && bash /opt/gage/src/tools/deploy/bootstrap-vps.sh --dry-run'
  2) when the dry run reads right:   ssh root@5.175.253.187 'bash /opt/gage/src/tools/deploy/bootstrap-vps.sh'
  3) then the swap-in step (links src/build, installs /opt/gage/CLAUDE.md):
     ssh root@5.175.253.187 'bash /opt/gage/src/tools/deploy/unpack-source.sh /root/$NAME.tar.gz --sha256 $SHA'

EVERY LATER TIME (replaces the whole tree, keeps the previous one as /opt/gage/src.prev):
  ssh root@5.175.253.187 'bash /opt/gage/src/tools/deploy/unpack-source.sh /root/$NAME.tar.gz --sha256 $SHA --dry-run'
  ssh root@5.175.253.187 'bash /opt/gage/src/tools/deploy/unpack-source.sh /root/$NAME.tar.gz --sha256 $SHA'
EOF
