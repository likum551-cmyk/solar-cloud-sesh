#!/usr/bin/env bash
# One-time (and safely repeatable) setup of the Solar RP VPS: Ubuntu 24.04, x86_64, run as root.
#
#   bootstrap-vps.sh [--dry-run] [--skip-dns-check] [--skip-https-check] [--no-hold] [--no-firewall]
#
#   --dry-run          print every change, make none. The read-only checks still run.
#   --skip-dns-check   do not insist that solar-rp.com and www.solar-rp.com already point at this VPS
#   --skip-https-check do not wait for, or fail on, the final https://solar-rp.com/update/ self-check
#   --no-hold          do not `apt-mark hold` the cross compiler and wine packages
#   --no-firewall      leave ufw alone (you manage it yourself)
#
# What it does, in order (each step checks first, so a second run changes nothing that is already right):
#   1 preflight    root, Ubuntu 24.04, x86_64, DNS for the domain and www point here
#   2 firewall     ufw: deny incoming; allow ssh (the ports sshd really listens on), 80/tcp, 443/tcp, 24800/udp; enable
#   3 apt          base tools and build chain (cmake, ninja, g++), mingw-w64 -posix cross compiler, i386 + wine + xvfb
#   4 nodejs 22    NodeSource apt repository (signed-by keyring), nodejs from node_22.x
#   5 caddy        the official Caddy apt repository (Cloudsmith), caddy package
#   6 layout       user gage, /opt/gage/{src,build,server,releases,logs,tmp}, /var/www/gage/update, /etc/gage/{deploy,server}.env
#   7 services     gage-server.service installed and enabled (NOT started: no binary yet), Caddyfile installed, validated, reloaded
#   8 check        waits for https://solar-rp.com/update/ (a valid Let's Encrypt certificate proves 80/443 are reachable from outside)
#   9 record       versions printed and written to /etc/gage/bootstrap-versions.txt; compiler and wine packages held
#
# NOT done here and cannot be: a firewall at the hosting provider, if the host has one (the current VPS has none;
# ufw is its firewall). Such a firewall sits in front of the VPS and ufw cannot see or change it: inbound tcp 22,
# 80, 443 and udp 24800 must be allowed there too, BEFORE this script starts Caddy, or the certificate cannot be issued.
set -euo pipefail

SCRIPT_NAME=bootstrap-vps
. "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; }

SKIP_DNS=0; SKIP_HTTPS=0; HOLD=1; FIREWALL=1
while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) DRY_RUN=1; shift ;;
    --skip-dns-check) SKIP_DNS=1; shift ;;
    --skip-https-check) SKIP_HTTPS=1; shift ;;
    --no-hold) HOLD=0; shift ;;
    --no-firewall) FIREWALL=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) usage_die "unknown option $1" ;;
  esac
done

deploy_config
GAGE_UID_NAME=gage
ETC=/etc/gage
APT_ENV=(env DEBIAN_FRONTEND=noninteractive)
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
BANNER_DONE=0

banner_cloud_firewall() {
  warn "=================================================================================================="
  warn "PROVIDER FIREWALL (only if your host puts one in front of the VPS; ufw cannot see it) must allow inbound"
  warn "    tcp 22 (ssh)   tcp 80 and tcp 443 (Caddy: certificate + update feed)   udp 24800 (game + query)"
  warn "Without 80/443 there Let's Encrypt cannot validate $DOMAIN and Caddy will not get a certificate."
  warn "=================================================================================================="
}

# ---- helpers ---------------------------------------------------------------------------------------------
pkg_installed() { dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'install ok installed'; }
apt_install() { run "${APT_ENV[@]}" apt-get install -y -o Dpkg::Options::=--force-confold "$@"; }

# install_file SRC DST MODE: copy when different; the previous file is kept as DST.gage-bak-<stamp>.
install_file() {
  local src=$1 dst=$2 mode=$3
  [ -f "$src" ] || die "missing $src"
  if [ -f "$dst" ] && cmp -s "$src" "$dst"; then log "up to date: $dst"; return 0; fi
  if [ -f "$dst" ]; then run cp -a -- "$dst" "$dst.gage-bak-$STAMP"; fi
  run install -D -m "$mode" -- "$src" "$dst"
  if [ "$DRY_RUN" = 1 ]; then log "[dry-run] would install $dst"; else log "installed $dst"; fi
}

# write_file DST MODE (content on stdin): same contract as install_file for generated text.
write_file() {
  local dst=$1 mode=$2 tmp
  tmp=$(mktemp "${TMPDIR:-/tmp}/gage-bootstrap.XXXXXX")
  defer "rm -f -- $(printf '%q' "$tmp")"
  cat > "$tmp"
  install_file "$tmp" "$dst" "$mode"
}

# ---- 1 preflight -----------------------------------------------------------------------------------------
log "== 1/9 preflight ($([ "$DRY_RUN" = 1 ] && echo DRY RUN || echo real run))"
banner_cloud_firewall
if [ "$DRY_RUN" = 0 ]; then
  [ "$(id -u)" = 0 ] || die "run as root"
  [ "$(uname -m)" = x86_64 ] || die "this is $(uname -m); the kit is for x86_64"
fi
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  if [ "${ID:-}" = ubuntu ] && [ "${VERSION_ID:-}" = 24.04 ]; then log "Ubuntu 24.04 (noble)"
  elif [ "$DRY_RUN" = 1 ]; then warn "dry-run: this is ${PRETTY_NAME:-unknown}, a real run needs Ubuntu 24.04"
  else die "this is ${PRETTY_NAME:-unknown}; the kit is written for Ubuntu 24.04 (noble)"; fi
elif [ "$DRY_RUN" = 0 ]; then
  die "no /etc/os-release"
fi

check_dns() {
  local name=$1 ips
  if ! command -v getent >/dev/null 2>&1; then warn "getent not available, cannot check DNS for $name"; return 0; fi
  ips=$(getent ahostsv4 "$name" 2>/dev/null | awk '{print $1}' | sort -u | tr '\n' ' ' || true)
  if [[ " $ips " == *" $PUBLIC_IP "* ]]; then log "DNS ok: $name -> $ips"; return 0; fi
  if [ "$SKIP_DNS" = 1 ] || [ "$DRY_RUN" = 1 ]; then warn "DNS: $name resolves to '${ips:-nothing}', not $PUBLIC_IP (continuing because of --skip-dns-check or --dry-run)"; return 0; fi
  die "DNS: $name resolves to '${ips:-nothing}', not $PUBLIC_IP. Caddy cannot get a certificate until the A record points at this VPS (--skip-dns-check to go on anyway)."
}
if [ -n "$DOMAIN" ]; then
  check_dns "$DOMAIN"
  check_dns "www.$DOMAIN"
else
  log "DOMAIN is empty: IP-only mode, no DNS or HTTPS checks"
  SKIP_HTTPS=1
fi

# ---- 2 firewall ------------------------------------------------------------------------------------------
log "== 2/9 firewall (ufw)"
if [ "$FIREWALL" = 0 ]; then
  log "--no-firewall: ufw untouched"
else
  # ssh first: enabling a default-deny firewall without it locks us out. Use the ports sshd really listens on.
  ssh_ports=''
  if command -v sshd >/dev/null 2>&1 && [ "$(id -u)" = 0 ]; then
    ssh_ports=$(sshd -T 2>/dev/null | awk '$1 == "port" {print $2}' | sort -un | tr '\n' ' ' || true)
  fi
  [ -n "$ssh_ports" ] || { ssh_ports=22; warn "could not read sshd's ports (sshd -T); assuming 22"; }
  run "${APT_ENV[@]}" apt-get update
  apt_install ufw
  run ufw default deny incoming
  run ufw default allow outgoing
  for p in $ssh_ports; do run ufw allow "$p/tcp"; done
  run ufw allow 80/tcp
  run ufw allow 443/tcp
  run ufw allow 24800/udp
  # 443/udp (HTTP/3) is deliberately not opened: browsers fall back to HTTP/2, and the launcher uses WinHTTP.
  run ufw --force enable
  log "ufw rules: ssh ($ssh_ports)/tcp, 80/tcp, 443/tcp, 24800/udp"
fi

# ---- 3 apt, build chain, wine ----------------------------------------------------------------------------
log "== 3/9 apt packages"
run "${APT_ENV[@]}" apt-get update
BASE_PKGS=(ca-certificates curl gnupg apt-transport-https xz-utils tar gzip unzip rsync file jq git tmux dnsutils
           build-essential cmake ninja-build pkg-config)
# The -posix variants on purpose: Ubuntu's default x86_64-w64-mingw32-g++ is the win32 thread model, and the project is
# built with posix threads (std::thread, std::mutex) exactly like the WinLibs toolchain in .toolchain/winlibs-gcc13.3.
MINGW_PKGS=(gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64 mingw-w64-x86-64-dev mingw-w64-common)
apt_install "${BASE_PKGS[@]}"
apt_install "${MINGW_PKGS[@]}"

# i386 for wine32: Inno Setup's ISCC.exe, ISCmplr.dll and ISPP.dll are 32-bit x86 programs.
if dpkg --print-foreign-architectures 2>/dev/null | grep -qx i386; then log "i386 architecture already enabled"
else run dpkg --add-architecture i386; fi

# The host's apt mirror may not carry i386 index files. If `apt-get update` fails now, restrict the existing Ubuntu
# sources to amd64 and add archive.ubuntu.com for i386 only (the previous files are kept as *.gage-bak).
restrict_sources_to_amd64() {
  local f tmp
  for f in /etc/apt/sources.list.d/*.sources; do
    [ -f "$f" ] || continue
    [ "$(basename "$f")" != gage-i386.sources ] || continue
    case $(basename "$f") in ubuntu*.sources) ;; *) continue ;; esac
    if grep -q '^Architectures:' "$f"; then log "$f already restricts Architectures"; continue; fi
    cp -a -- "$f" "$f.gage-bak"
    tmp=$(mktemp "${TMPDIR:-/tmp}/gage-sources.XXXXXX")
    # one stanza per paragraph; add the restriction to every stanza
    awk 'BEGIN { RS = ""; ORS = "\n\n"; FS = "\n" } { print $0 "\nArchitectures: amd64" }' "$f" > "$tmp"
    install -m 644 -- "$tmp" "$f"
    rm -f -- "$tmp"
    log "restricted $f to amd64 (backup $f.gage-bak)"
  done
  cat > /etc/apt/sources.list.d/gage-i386.sources <<'EOF'
Types: deb
URIs: http://archive.ubuntu.com/ubuntu
Suites: noble noble-updates noble-security
Components: main universe
Architectures: i386
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
EOF
}
if [ "$DRY_RUN" = 1 ]; then
  log "[dry-run] apt-get update (with i386); if that fails: restrict ubuntu*.sources to amd64 and add /etc/apt/sources.list.d/gage-i386.sources"
else
  if ! "${APT_ENV[@]}" apt-get update; then
    warn "apt-get update failed with i386 enabled: probably the mirror has no i386 indexes. Trying the archive.ubuntu.com i386-only source."
    restrict_sources_to_amd64
    "${APT_ENV[@]}" apt-get update || die "apt-get update still fails; read the errors above (docs/OPS.md, 'apt update fails after enabling i386')"
  fi
fi
apt_install wine wine64 wine32:i386 xvfb xauth
if [ "$DRY_RUN" = 0 ]; then
  cand=$(apt-cache policy wine32:i386 | sed -n 's/^ *Candidate: //p')
  [ -n "$cand" ] && [ "$cand" != "(none)" ] || die "wine32:i386 has no installation candidate: is the universe component enabled? (docs/OPS.md)"
  pkg_installed wine32:i386 || die "wine32:i386 is not installed"
fi

# ---- 4 Node.js 22 (NodeSource) ---------------------------------------------------------------------------
log "== 4/9 nodejs 22"
# Manual repository setup from the NodeSource wiki (keyring + deb822 sources), not the piped setup script.
run install -d -m 755 /etc/apt/keyrings
if [ -s /etc/apt/keyrings/nodesource.gpg ]; then
  log "NodeSource keyring present"
else
  run bash -c 'set -o pipefail; curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key | gpg --batch --yes --dearmor -o /etc/apt/keyrings/nodesource.gpg'
fi
write_file /etc/apt/sources.list.d/nodesource.sources 644 <<'EOF'
Types: deb
URIs: https://deb.nodesource.com/node_22.x/
Suites: nodistro
Components: main
Signed-By: /etc/apt/keyrings/nodesource.gpg
EOF
run "${APT_ENV[@]}" apt-get update
apt_install nodejs
if [ "$DRY_RUN" = 0 ]; then
  nv=$(node -p 'process.versions.node')
  [ "${nv%%.*}" = 22 ] || die "node is $nv, expected 22.x (the Ubuntu nodejs package may have won over NodeSource: apt-cache policy nodejs)"
  [ "$(ver_cmp "$nv" 22.12.0)" -ge 0 ] || die "node $nv is older than 22.12.0, which apps/launcher-ui requires"
fi

# ---- 5 Caddy (official repository) -----------------------------------------------------------------------
log "== 5/9 caddy"
if [ -s /usr/share/keyrings/caddy-stable-archive-keyring.gpg ]; then
  log "Caddy keyring present"
else
  run bash -c "set -o pipefail; curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | gpg --batch --yes --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg"
  run chmod o+r /usr/share/keyrings/caddy-stable-archive-keyring.gpg
fi
if [ -s /etc/apt/sources.list.d/caddy-stable.list ]; then
  log "Caddy apt source present"
else
  run bash -c "curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' -o /etc/apt/sources.list.d/caddy-stable.list"
  run chmod o+r /etc/apt/sources.list.d/caddy-stable.list
fi
run "${APT_ENV[@]}" apt-get update
# The Caddy package starts a caddy.service with its stock Caddyfile on port 80 as soon as it is installed; step 7
# replaces that with ours. ufw (step 2) already allows 80 and 443.
apt_install caddy

# ---- 6 users, directories, config files ------------------------------------------------------------------
log "== 6/9 user, directories, config"
if getent passwd "$GAGE_UID_NAME" >/dev/null 2>&1; then log "user $GAGE_UID_NAME exists"
else run adduser --system --group --home "$GAGE_OPT/server" --no-create-home --shell /usr/sbin/nologin "$GAGE_UID_NAME"; fi

for d in "$GAGE_OPT" "$GAGE_OPT/src" "$GAGE_OPT/build" "$GAGE_OPT/releases" "$GAGE_OPT/logs" "$GAGE_OPT/tmp" \
         "$GAGE_SERVER_DIR" "$GAGE_SERVER_DIR/releases" "$GAGE_WEB_ROOT" "$GAGE_WEB_ROOT/update" "$GAGE_WEB_ROOT/.staging" "$ETC"; do
  run install -d -m 755 -- "$d"
done
# The one directory the game server may write: bans.json and gamemode data/<resource>/ (gage-server.service ReadWritePaths).
run install -d -m 750 -o "$GAGE_UID_NAME" -g "$GAGE_UID_NAME" -- "$GAGE_SERVER_DIR/data"
# The signing key lives in root's home, never under a served or world-readable directory.
run install -d -m 700 -- "$GAGE_KEY_DIR"

# A placeholder so https://<domain>/update/ answers 200 before the first release exists (also what step 8 fetches).
if [ ! -e "$GAGE_WEB_ROOT/update/index.txt" ]; then
  if [ "$DRY_RUN" = 1 ]; then log "[dry-run] write $GAGE_WEB_ROOT/update/index.txt"
  else
    printf 'Solar RP update server\n' > "$GAGE_WEB_ROOT/update/index.txt"
    chmod 644 "$GAGE_WEB_ROOT/update/index.txt"
    log "wrote $GAGE_WEB_ROOT/update/index.txt"
  fi
fi

# /etc/gage/deploy.env: the knobs of this kit, all defaults commented out. Never overwritten.
if [ -e "$ETC/deploy.env" ]; then log "kept $ETC/deploy.env"
else run install -m 640 -- "$DEPLOY_DIR/deploy.env.example" "$ETC/deploy.env"; fi
# /etc/gage/server.env: the admin-channel token for the game server (and for deploy-server.sh's in-game warning).
# Generated once, never printed, readable by root and the gage group (the service reads it through systemd as root).
if [ -e "$ETC/server.env" ]; then log "kept $ETC/server.env"
elif [ "$DRY_RUN" = 1 ]; then log "[dry-run] create $ETC/server.env with a random GAGE_ADMIN_TOKEN (not shown)"
else
  token=$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')
  [ ${#token} -eq 48 ] || die "could not generate an admin token"
  ( umask 077; printf 'GAGE_ADMIN_TOKEN=%s\n' "$token" > "$ETC/server.env.new" )
  chown "root:$GAGE_UID_NAME" "$ETC/server.env.new" 2>/dev/null || true
  chmod 640 "$ETC/server.env.new"
  mv -T -- "$ETC/server.env.new" "$ETC/server.env"
  unset token
  log "created $ETC/server.env with a fresh GAGE_ADMIN_TOKEN (not shown; the server only opens the channel when server.json has \"adminPort\")"
fi

# ---- 7 services ------------------------------------------------------------------------------------------
log "== 7/9 gage-server.service and Caddy"
install_file "$DEPLOY_DIR/gage-server.service" /etc/systemd/system/gage-server.service 644
run systemctl daemon-reload
run systemctl enable gage-server.service
log "gage-server.service is enabled but NOT started: there is no binary yet (tools/deploy/deploy-server.sh builds and starts it)"

if [ -f /etc/caddy/Caddyfile ] && [ ! -e /etc/caddy/Caddyfile.dist ] && ! cmp -s "$DEPLOY_DIR/Caddyfile" /etc/caddy/Caddyfile; then
  run cp -a -- /etc/caddy/Caddyfile /etc/caddy/Caddyfile.dist
fi
install_file "$DEPLOY_DIR/Caddyfile" /etc/caddy/Caddyfile 644
if [ "$DRY_RUN" = 1 ]; then
  log "[dry-run] caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile ; systemctl enable --now caddy ; systemctl reload caddy"
else
  # validate as the caddy user: a root run would create root-owned files in caddy's data directories
  if command -v runuser >/dev/null 2>&1 && getent passwd caddy >/dev/null 2>&1; then
    runuser -u caddy -- env HOME=/var/lib/caddy caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile \
      || die "the Caddyfile does not validate (see above). The previous /etc/caddy/Caddyfile is kept as *.gage-bak-$STAMP; caddy keeps running its last good config."
  else
    caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile || die "the Caddyfile does not validate"
  fi
  systemctl enable --now caddy
  systemctl reload caddy || systemctl restart caddy
  log "caddy reloaded with the Solar RP Caddyfile"
fi

# ---- 8 HTTPS self-check ----------------------------------------------------------------------------------
log "== 8/9 https self-check"
if [ "$SKIP_HTTPS" = 1 ]; then
  log "skipped"
elif [ "$DRY_RUN" = 1 ]; then
  log "[dry-run] would poll curl -fsS https://$DOMAIN/update/ for up to 150 s (wall clock) and fail loudly if it never answers"
else
  # Certificate issuance takes 5-30 s on a good day. A valid certificate (curl verifies it against the system CAs) can
  # only exist if Let's Encrypt reached ports 80/443 from the internet, so this also proves the cloud firewall is open
  # for them. Caddy retries with back-off, so a first failure here is not final.
  deadline=$((SECONDS + 150)); ok_https=0; last_err=''
  while [ "$SECONDS" -lt "$deadline" ]; do
    if last_err=$(curl -fsS --max-time 10 -o /dev/null "https://$DOMAIN/update/" 2>&1); then ok_https=1; break; fi
    sleep 5
  done
  if [ "$ok_https" = 1 ]; then
    log "OK: https://$DOMAIN/update/ answers with a valid certificate (waited $((150 - (deadline - SECONDS))) s)"
    if curl -fsS --max-time 10 -o /dev/null "http://127.0.0.1/update/" 2>/dev/null; then log "OK: the plain-HTTP fallback on the bare IP answers too"
    else warn "http://127.0.0.1/update/ does not answer: the :80 fallback block is not working"; fi
  else
    warn "https://$DOMAIN/update/ did not answer within 150 s: $last_err"
    warn "likely causes, in order: 1) a provider firewall in front of the VPS (if any) lacks inbound tcp 80/443   2) DNS for $DOMAIN / www.$DOMAIN"
    warn "3) a port is held by another process (ss -tlnp | grep -E ':(80|443) ')   4) Let's Encrypt rate limit after repeated failures."
    warn "Caddy log: journalctl -u caddy -n 80 --no-pager"
    banner_cloud_firewall
    die "HTTPS is not live. Everything else is installed; fix the cause and run this script again (it is idempotent)."
  fi
fi

# ---- 9 versions, holds ---------------------------------------------------------------------------------
log "== 9/9 versions"
print_versions() {
  local mg w
  echo "ubuntu:   $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-?}")"
  echo "kernel:   $(uname -sr 2>/dev/null || echo ?)"
  mg=$(find_tool x86_64-w64-mingw32-g++-posix || true)
  [ -z "$mg" ] || echo "mingw g++ ($mg): $("$mg" --version | head -n 1)  [$("$mg" -dumpspecs 2>/dev/null | grep -c -- '-lmsvcrt') msvcrt spec(s)]"
  w=$(find_tool wine wine-stable wine64 || true)
  [ -z "$w" ] || echo "wine:     $("$w" --version 2>/dev/null || echo '?')"
  echo "node:     $(node -v 2>/dev/null || echo missing)   npm: $(npm -v 2>/dev/null || echo missing)"
  echo "cmake:    $(cmake --version 2>/dev/null | head -n 1)   ninja: $(ninja --version 2>/dev/null || echo missing)"
  echo "g++:      $(g++ --version 2>/dev/null | head -n 1)"
  echo "caddy:    $(caddy version 2>/dev/null | head -n 1)"
  echo "ufw:      $(ufw version 2>/dev/null | head -n 1)"
  echo "systemd:  $(systemctl --version 2>/dev/null | head -n 1)"
  echo "packages: $(dpkg-query -W -f='${Package}=${Version} ' gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix mingw-w64-x86-64-dev wine64 'wine32:i386' nodejs caddy 2>/dev/null || true)"
}
if [ "$DRY_RUN" = 1 ]; then
  log "[dry-run] would print the tool versions, write $ETC/bootstrap-versions.txt and hold the compiler and wine packages"
else
  { echo "# written by bootstrap-vps.sh at $STAMP"; print_versions; } | tee "$ETC/bootstrap-versions.txt"
  if [ "$HOLD" = 1 ]; then
    # Reproducible builds: unattended upgrades must not swap the compiler or wine under a release. Undo with apt-mark unhold.
    apt-mark hold gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix-runtime mingw-w64-x86-64-dev wine wine64 'wine32:i386' >/dev/null
    log "held: mingw-w64 -posix compilers and wine (apt-mark unhold <pkg> to update them on purpose)"
  fi
fi

log "done. Next (docs/OPS.md, 'First-time setup'): copy the signing key to $GAGE_KEY_DIR, unpack the source, tools/deploy/build-client.sh --dry-run."
banner_cloud_firewall
