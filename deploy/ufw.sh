#!/usr/bin/env bash
# Configure the host firewall (ufw) for the GAGE VPS. Refuses to do anything without an explicit
# --apply (changes the firewall) or --dry-run (prints what --apply would run).
#
#   sudo bash ufw.sh --dry-run
#   sudo bash ufw.sh --apply
#   options: [--ssh-port N] [--game-range LO:HI] [--announce-port N] [--announce-ips "IP IP/24 ..."]
#            [--conf FILE]
#
# Result: default deny incoming, allow outgoing, and only
#   TCP <ssh port>            SSH (any source; fail2ban + keys-only sshd protect it)
#   TCP 80, 443               Caddy (ACME + HTTPS)
#   UDP <game range>          game servers (UDP query is answered on the same port)
#   TCP <announce port>       ONLY from the listed announce IPs (the C++ server announces over plain HTTP)
# Never opened: PostgreSQL 5432, master 8080, platform 8787/8788, gage-server admin ports (loopback only),
# anything else. Traffic between processes on this machine uses the loopback interface, which ufw always allows
# (this includes game servers on this machine announcing to the machine's own public address).
#
# Settings (flag, else environment, else /etc/gage/network.env, else default):
#   GAGE_SSH_PORT=22   GAGE_GAME_UDP_RANGE=24800:24809   GAGE_ANNOUNCE_PORT=8081   GAGE_ANNOUNCE_ALLOW=""
# The same GAGE_ANNOUNCE_ALLOW feeds the Caddyfile (caddy/Caddyfile), so the firewall and Caddy agree.
#
# Idempotent: re-running adds nothing that exists. Announce rules for IPs that left the list are deleted.
# SSH and game-range rules are additive: after changing those, review `ufw status numbered` by hand.
#
# Lock-out guards (refuse, never warn): nothing listens on the SSH port; or an SSH session is connected
# through a different port than --ssh-port.
# Exit codes: 0 done, 1 refused or failed, 2 usage.
set -euo pipefail

SCRIPT_NAME=ufw
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=scripts/lib.sh
. "$SCRIPT_DIR/scripts/lib.sh"

UFW=${UFW_BIN:-ufw}
CONF=${GAGE_ETC:-/etc/gage}/network.env
APPLY=0
OPT_SSH=''
OPT_GAME=''
OPT_APORT=''
OPT_AIPS=''
OPT_AIPS_SET=0

usage() {
  sed -n '2,/^set -euo pipefail$/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'
}

# stale_announce_ips PORT "WANTED IPS"   Read `ufw show added` on stdin; print the source of every
# announce rule (allow from X to any port PORT proto tcp) whose source is not wanted.
stale_announce_ips() {
  local port=$1 wanted=" $2 " line ip
  local re="^ufw allow from ([0-9./]+) to any port ${port} proto tcp"
  while IFS= read -r line; do
    if [[ $line =~ $re ]]; then
      ip=${BASH_REMATCH[1]}
      case $wanted in *" $ip "*) ;; *) printf '%s\n' "$ip" ;; esac
    fi
  done
}

# sshd_foreign_ports SSH_PORT  Read `ss -H -tnp state established` on stdin; print the local port of
# every established sshd connection that is NOT on SSH_PORT.
sshd_foreign_ports() {
  local want=$1
  awk -v want="$want" '/"sshd/ { n = split($3, a, ":"); if (a[n] != want) print a[n] }' | sort -u
}

in_range_port() { is_uint "${1-}" && [ "$1" -ge 1 ] && [ "$1" -le 65535 ]; }

main() {
  local dry=0
  while [ $# -gt 0 ]; do
    case $1 in
      --apply) APPLY=1; shift ;;
      --dry-run) dry=1; shift ;;
      --ssh-port) [ $# -ge 2 ] || usage_die "--ssh-port needs a value"; OPT_SSH=$2; shift 2 ;;
      --game-range) [ $# -ge 2 ] || usage_die "--game-range needs a value"; OPT_GAME=$2; shift 2 ;;
      --announce-port) [ $# -ge 2 ] || usage_die "--announce-port needs a value"; OPT_APORT=$2; shift 2 ;;
      --announce-ips) [ $# -ge 2 ] || usage_die "--announce-ips needs a value"; OPT_AIPS=$2; OPT_AIPS_SET=1; shift 2 ;;
      --conf) [ $# -ge 2 ] || usage_die "--conf needs a value"; CONF=$2; shift 2 ;;
      -h | --help) usage; exit 0 ;;
      *) usage_die "unknown argument: $1 (try --help)" ;;
    esac
  done
  if [ "$APPLY" = 1 ] && [ "$dry" = 1 ]; then usage_die "--apply and --dry-run are mutually exclusive"; fi
  if [ "$APPLY" = 0 ] && [ "$dry" = 0 ]; then
    usage >&2
    usage_die "refusing to run without --apply (change the firewall) or --dry-run (print the plan)"
  fi
  if [ "$dry" = 1 ]; then DRY_RUN=1; fi

  local ssh_port game_range game_lo game_hi aport aips
  ssh_port=${OPT_SSH:-$(conf_value GAGE_SSH_PORT 22 "$CONF")}
  game_range=${OPT_GAME:-$(conf_value GAGE_GAME_UDP_RANGE 24800:24809 "$CONF")}
  aport=${OPT_APORT:-$(conf_value GAGE_ANNOUNCE_PORT 8081 "$CONF")}
  if [ "$OPT_AIPS_SET" = 1 ]; then aips=$OPT_AIPS; else aips=$(conf_value GAGE_ANNOUNCE_ALLOW '' "$CONF"); fi

  in_range_port "$ssh_port" || usage_die "bad SSH port '$ssh_port'"
  in_range_port "$aport" || usage_die "bad announce port '$aport'"
  [[ $game_range =~ ^([0-9]+):([0-9]+)$ ]] || usage_die "game range must look like 24800:24809 (got '$game_range')"
  game_lo=${BASH_REMATCH[1]}
  game_hi=${BASH_REMATCH[2]}
  in_range_port "$game_lo" && in_range_port "$game_hi" || usage_die "game range ports must be 1..65535"
  [ "$game_lo" -ge 1024 ] || usage_die "game range must start at 1024 or above"
  [ "$game_lo" -le "$game_hi" ] || usage_die "game range is reversed"
  [ $((game_hi - game_lo)) -lt 100 ] || usage_die "game range is wider than 100 ports: refusing (typo?)"

  # Ports that must never be exposed by this script, whatever the settings say.
  local p
  for p in 5432 8080 8787 8788; do
    if [ "$aport" = "$p" ]; then usage_die "announce port $aport is a loopback-only service port"; fi
    if [ "$game_lo" -le "$p" ] && [ "$p" -le "$game_hi" ]; then usage_die "game range includes loopback-only port $p"; fi
  done
  if [ "$aport" = 80 ] || [ "$aport" = 443 ] || [ "$aport" = "$ssh_port" ]; then
    usage_die "announce port $aport collides with SSH/HTTP/HTTPS"
  fi
  if [ "$aport" -ge 24900 ] && [ "$aport" -le 24999 ]; then usage_die "announce port $aport is in the gage-server admin range 24900-24999"; fi
  if [ "$game_hi" -ge 24900 ]; then usage_die "game range must end below 24900 (24900-24999 is the loopback admin range)"; fi

  local -a ips=()
  local ip
  for ip in $aips; do
    valid_ipv4_or_cidr "$ip" || usage_die "bad announce source '$ip' (IPv4 or IPv4/prefix)"
    case $ip in 0.0.0.0*) usage_die "announce source '$ip' would open the announce port to everyone" ;; esac
    if [[ $ip == */* ]] && [ "${ip#*/}" -lt 16 ]; then usage_die "announce source '$ip' is too broad (prefix must be /16 or longer)"; fi
    ips+=("$ip")
  done

  if [ "$APPLY" = 1 ]; then
    [ "$(id -u)" -eq 0 ] || die "--apply needs root: sudo bash ufw.sh --apply"
    need_cmd "$UFW" ss awk sort
  fi

  # Lock-out guards.
  local guard_failed=0
  if command -v ss >/dev/null 2>&1; then
    if [ -z "$(ss -H -ltn "sport = :$ssh_port" 2>/dev/null)" ]; then
      warn "nothing is listening on TCP $ssh_port: enabling the firewall with that SSH port would lock you out"
      guard_failed=1
    fi
    local foreign
    foreign=$(ss -H -tnp state established 2>/dev/null | sshd_foreign_ports "$ssh_port" || true)
    if [ -n "$foreign" ]; then
      warn "an SSH session is connected through TCP port(s) $(echo "$foreign" | tr '\n' ' ')but --ssh-port is $ssh_port: you would lose it"
      guard_failed=1
    fi
  else
    warn "ss not found: cannot verify the SSH port"
    guard_failed=1
  fi
  if [ "$guard_failed" = 1 ]; then
    if [ "$APPLY" = 1 ]; then die "lock-out guard tripped: not touching the firewall"; fi
    warn "dry run: --apply would REFUSE here"
  fi

  log "plan: SSH tcp/$ssh_port, tcp/80, tcp/443, udp/$game_range, announce tcp/$aport from: ${ips[*]:-(nobody: closed to the network)}"
  [ "${#ips[@]}" -gt 0 ] || log "no announce sources: servers on this machine still reach the announce listener over loopback"

  # Stale announce rules (IPs removed from the list). Needs root to read the rule list.
  local -a stale=()
  if [ "$(id -u)" -eq 0 ] && command -v "$UFW" >/dev/null 2>&1; then
    local s
    while IFS= read -r s; do
      [ -z "$s" ] || stale+=("$s")
    done < <("$UFW" show added 2>/dev/null | stale_announce_ips "$aport" "${ips[*]:-}")
  else
    log "not root or ufw missing: cannot compare with existing rules (stale announce rules would not be detected)"
  fi
  for ip in "${stale[@]}"; do
    log "stale announce rule for $ip will be deleted"
    run "$UFW" delete allow from "$ip" to any port "$aport" proto tcp
  done

  run "$UFW" default deny incoming
  run "$UFW" default allow outgoing
  run "$UFW" default deny routed
  run "$UFW" allow "$ssh_port/tcp" comment gage:ssh
  run "$UFW" allow 80/tcp comment gage:http
  run "$UFW" allow 443/tcp comment gage:https
  run "$UFW" allow "$game_lo:$game_hi/udp" comment gage:game
  for ip in "${ips[@]}"; do
    run "$UFW" allow from "$ip" to any port "$aport" proto tcp comment gage:announce
  done
  run "$UFW" --force enable
  if [ "$APPLY" = 1 ]; then
    "$UFW" status verbose
    log "done. Verify from OUTSIDE the host (CHECKLIST.md, 'Firewall verified from outside')"
  else
    log "dry run only: nothing was changed"
  fi
}

# Run main only when executed, so tests can source this file for the parsing helpers.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
