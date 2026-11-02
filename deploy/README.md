# GAGE-MP production deploy kit (one Hetzner VPS)

Runbook for the whole backend on a single machine: game servers (`gage-server`, C++), the master
server list (`apps/master`, Node), the platform API (`apps/platform`, Hono + PostgreSQL), Caddy for TLS.

**Assumed:** Ubuntu 24.04 LTS (noble), x86_64, a Hetzner Cloud VPS with at least 4 GB RAM (the C++ build and
`npm ci` need headroom; add swap if you go smaller), one static public IPv4, a domain whose DNS you control.
Nothing in this kit was run against a real host while it was written; read "Verification status" at the end
and do the on-box checks of steps 4 and 9 before trusting it.

Every placeholder is loud: `gage.example` (domain), `203.0.113.10` (the VPS's public IPv4, a documentation
address), `ops@gage.example`, `hooks.example.invalid`. Nothing here contains a secret; secrets are generated
on the box and live only in `/etc/gage/*.env`.

## 0. Layout

```
Internet
  80/443 tcp ─► Caddy ─┬─ api.gage.example    ─► platform  127.0.0.1:8787   (public API; /internal/* refused)
                       ├─ master.gage.example ─► master    127.0.0.1:8080   (GET /servers, /healthz only)
  8081 tcp (allow-list)└─ http://:8081        ─► master    127.0.0.1:8080   (POST /announce only, game servers' IPs)
  24800-24809 udp ────────────────────────────► gage-server@1, @2, @test ...   (game + UDP query on the same port)

loopback only: platform internal API :8788, PostgreSQL :5432, gage-server admin channel :24900+
```

| Port | Proto | Service | Exposure |
|---|---|---|---|
| 22 (or your port) | tcp | sshd | internet, keys only, fail2ban |
| 80, 443 | tcp | Caddy | internet |
| 8081 | tcp | Caddy plain-HTTP announce listener | only `GAGE_ANNOUNCE_ALLOW` addresses (ufw and Caddy) |
| 24800-24809 | udp | gage-server instances | internet |
| 8080 / 8787 / 8788 / 5432 / 24900+ | tcp | master / platform / platform internal / PostgreSQL / game-server admin | loopback only, never opened |

| Path | Owner | What |
|---|---|---|
| `/opt/gage/src` | deploy | repository checkout (build input) |
| `/opt/gage/deploy` | root | this kit, installed copy (units, scripts) |
| `/opt/gage/<app>/releases/<id>` , `current`, `previous` | deploy | versioned releases, symlinks |
| `/etc/gage/` | root | `*.env` (mode 600, owned by the service user), `server-<n>.json`, `network.env` |
| `/var/lib/gage/<app>`, `/var/lib/gage/server-<n>` | service user | data (`bans.json` per game server) |
| `/var/backups/gage` | postgres | nightly dumps |
| journald | | all logs (`journalctl -u <unit>`) |

System users (no shell, no home): `gage-server`, `gage-master`, `gage-platform`. Operator: `deploy` (sudo).

## 1. Provision the machine

Do this as root on first login, **keep that session open** until the last verification of this step passes.

```bash
adduser deploy                      # prompts for a password: used only for sudo, SSH logins are key-only
usermod -aG sudo deploy
install -d -m 700 -o deploy -g deploy /home/deploy/.ssh
echo 'ssh-ed25519 AAAA...your-public-key... you@laptop' > /home/deploy/.ssh/authorized_keys
chown deploy:deploy /home/deploy/.ssh/authorized_keys && chmod 600 /home/deploy/.ssh/authorized_keys
```

SSH hardening as a drop-in. The `10-` prefix matters: sshd keeps the FIRST value it reads, and Hetzner's
cloud-init image ships `50-cloud-init.conf` with `PasswordAuthentication yes`.

```bash
cat > /etc/ssh/sshd_config.d/10-gage-hardening.conf <<'EOF'
PermitRootLogin no
PasswordAuthentication no
KbdInteractiveAuthentication no
PubkeyAuthentication yes
AuthenticationMethods publickey
AllowUsers deploy
MaxAuthTries 3
LoginGraceTime 20
X11Forwarding no
AllowAgentForwarding no
AllowTcpForwarding local
ClientAliveInterval 300
ClientAliveCountMax 2
EOF
sshd -t && systemctl reload ssh
sshd -T | grep -E '^(passwordauthentication|permitrootlogin|allowusers|authenticationmethods) '
```

From a **second terminal**: `ssh deploy@<ip>` must work with your key, and `ssh root@<ip>` must be refused. Only then close the
root session. (`AllowTcpForwarding local` stays on so you can tunnel to the loopback admin ports, section 10.)

A custom SSH port (optional): on Ubuntu 24.04 sshd is socket-activated, so `Port` in sshd_config alone does nothing.
Use `sudo systemctl edit ssh.socket` with `[Socket]` / `ListenStream=` / `ListenStream=2222`, then
`sudo systemctl daemon-reload && sudo systemctl restart ssh.socket`, check `ss -ltn | grep 2222` and log in on the
new port from a second terminal BEFORE the firewall step. Put the same number in `GAGE_SSH_PORT` (step 5).

Everything from here runs as `deploy` with `sudo`.

### Updates, clock, journal

```bash
sudo apt-get update && sudo apt-get -y full-upgrade
sudo apt-get install -y unattended-upgrades apt-listchanges
sudo dpkg-reconfigure -f noninteractive unattended-upgrades          # writes /etc/apt/apt.conf.d/20auto-upgrades
sudo unattended-upgrade --dry-run --debug 2>&1 | tail -n 5           # no errors
sudo timedatectl set-timezone UTC
timedatectl | grep -E 'Time zone|synchronized|NTP service'           # synchronized: yes, NTP service: active (systemd-timesyncd)
```

Security updates install themselves; **reboots do not** (a reboot kicks every player). After a kernel update
`/var/run/reboot-required` appears: reboot in a window you announce. Third-party repos (NodeSource, Caddy) are not part
of unattended-upgrades: run `sudo apt-get update && sudo apt-get upgrade` monthly and `sudo systemctl reload caddy` afterwards.

journald must be persistent (Ubuntu's default `auto` loses logs at reboot when `/var/log/journal` does not exist) and capped:

```bash
sudo install -d /etc/systemd/journald.conf.d
sudo tee /etc/systemd/journald.conf.d/10-gage.conf >/dev/null <<'EOF'
[Journal]
Storage=persistent
SystemMaxUse=1G
MaxRetentionSec=30day
EOF
sudo systemctl restart systemd-journald && journalctl --disk-usage
```

## 2. Install the runtime

```bash
sudo apt-get install -y ufw fail2ban build-essential cmake ninja-build git rsync curl ca-certificates gnupg openssl \
  netcat-openbsd shellcheck postgresql-16 postgresql-client-16 debian-keyring debian-archive-keyring apt-transport-https
cmake --version | head -n1      # 3.28 on noble; the project needs >= 3.16
psql --version                  # 16.x
```

**Node 22** from NodeSource (noble ships Node 18). Download the setup script, read it, then run it (NodeSource's documented
non-piped form, fetched 2026-10-29):

```bash
curl -fsSL https://deb.nodesource.com/setup_22.x -o /tmp/nodesource_setup.sh
less /tmp/nodesource_setup.sh
sudo -E bash /tmp/nodesource_setup.sh && sudo apt-get install -y nodejs
node --version                  # v22.x, must be >= 22.12 (apps/*/package.json engines)
```

Node 22 is in maintenance LTS in late 2026 (security fixes until about April 2027, from the Node release schedule, not
re-checked): plan the move to Node 24 and change nothing else but this step.

**Caddy** (Caddy's documented apt repository, fetched 2026-10-29):

```bash
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' | sudo tee /etc/apt/sources.list.d/caddy-stable.list
sudo chmod o+r /usr/share/keyrings/caddy-stable-archive-keyring.gpg /etc/apt/sources.list.d/caddy-stable.list
sudo apt-get update && sudo apt-get install -y caddy
caddy version
```

## 3. System users and directories

```bash
for app in server master platform; do
  sudo useradd --system --user-group --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin gage-$app
done

sudo install -d -m 0755 -o root   -g root   /opt/gage /opt/gage/deploy /etc/gage /var/lib/gage
sudo install -d -m 0755 -o deploy -g deploy /opt/gage/src
for app in master platform server; do
  sudo install -d -m 0755 -o deploy -g deploy /opt/gage/$app /opt/gage/$app/releases
done
sudo install -d -m 0750 -o gage-master   -g gage-master   /var/lib/gage/master
sudo install -d -m 0750 -o gage-platform -g gage-platform /var/lib/gage/platform
sudo install -d -m 0750 -o gage-server   -g gage-server   /var/lib/gage/server-1 /var/lib/gage/server-test
sudo install -d -m 0755 -o postgres -g postgres /var/backups/gage
```

Code under `/opt/gage/<app>/releases` is owned by `deploy` and read-only for the service users: a compromised service
cannot rewrite its own code. The `deploy` user can (it is a sudoer anyway).

## 4. Get the code and install the kit

Pick one. Git is preferred (release ids carry the commit; `-dirty` is added for uncommitted changes).

```bash
# (a) git with a read-only deploy key
git clone git@github.com:YOUR-ORG/gage-mp.git /opt/gage/src
# (b) rsync from the dev machine, run from the repository root there
rsync -a --delete --exclude='.git' --exclude='.toolchain' --exclude='build' --exclude='build-*' \
  --exclude='node_modules' --exclude='.env*' ./ deploy@YOUR-VPS:/opt/gage/src/
```

Install the kit (root-owned: the timers run these scripts as other users, so `deploy` must not be able to edit them
between reviews). Repeat after every pull that touches `deploy/`.

```bash
sudo rsync -a --delete --chown=root:root /opt/gage/src/deploy/ /opt/gage/deploy/
sudo find /opt/gage/deploy -type f \( -name '*.sh' -o -name '*.service' -o -name '*.timer' -o -name Caddyfile -o -name '*.json' \) -print0 | xargs -0 grep -l $'\r' || echo "no CRLF"
```

On-box checks of the kit itself (none of these tools exist on the dev machine, see "Verification status"):

```bash
shellcheck /opt/gage/deploy/scripts/*.sh /opt/gage/deploy/ufw.sh      # fix or consciously accept every finding
sudo cp /opt/gage/deploy/systemd/*.service /opt/gage/deploy/systemd/*.timer /etc/systemd/system/
sudo systemctl daemon-reload
systemd-analyze verify /etc/systemd/system/gage-*.service /etc/systemd/system/gage-*.timer
# 'AssertPathExists' warnings before the first release are expected; anything else is not.
```

The third check, `caddy validate`, comes in step 9.

## 5. Firewall and fail2ban

Settings shared by the firewall and Caddy go in one file. Create it once (mode 644, no secrets):

```bash
sudo tee /etc/gage/network.env >/dev/null <<'EOF'
GAGE_SSH_PORT=22
GAGE_GAME_UDP_RANGE=24800:24809
GAGE_ANNOUNCE_PORT=8081
GAGE_ANNOUNCE_ALLOW="203.0.113.10"
EOF
```

`GAGE_ANNOUNCE_ALLOW` is a space-separated list of IPv4 addresses (or `/16`..`/32` ranges) that may POST announces.
It must contain **every address a game server announces from**. For game servers on this machine that is the machine's
own public IPv4 (a connection to a local address keeps that address as its source: `ip route get 203.0.113.10` shows
`local 203.0.113.10 dev lo src 203.0.113.10`). ufw lets loopback traffic through, so the firewall needs no rule for
the local case; Caddy checks the same list.

```bash
sudo bash /opt/gage/deploy/ufw.sh --dry-run      # read the plan; the guards tell you if --apply would refuse
sudo bash /opt/gage/deploy/ufw.sh --apply        # refuses to run without --apply or --dry-run
sudo ufw status verbose
```

Result: deny incoming, allow outgoing; allow SSH, 80/443 tcp, the game UDP range, and 8081/tcp from the listed addresses only.
The script refuses when nothing listens on the SSH port, when an SSH session arrived through another port, when the announce
list contains `0.0.0.0/0` or anything broader than `/16`, or when a range touches a loopback-only port (5432, 8080, 8787,
8788, 24900-24999). The gage-server admin port is never opened: the server binds `127.0.0.1` only and ufw denies the rest.
Re-running adds nothing that exists and deletes announce rules for addresses you removed from the list; SSH and game-range
rules are additive, so after changing those review `sudo ufw status numbered`.

Optional second layer: a Hetzner Cloud Firewall with the same table (it filters before the packet reaches the VM).

fail2ban (after ufw, because it bans through ufw):

```bash
sudo tee /etc/fail2ban/jail.d/10-gage.local >/dev/null <<'EOF'
[DEFAULT]
bantime = 1h
findtime = 10m
maxretry = 5
banaction = ufw
ignoreip = 127.0.0.1/8 ::1

[sshd]
enabled = true
backend = systemd
port = ssh
EOF
sudo systemctl enable --now fail2ban && sudo systemctl restart fail2ban
sudo fail2ban-client status sshd
```

(`port = ssh` means 22; with a custom port write the number.)

## 6. PostgreSQL

PostgreSQL 16 listens on localhost only by default and Ubuntu's `pg_hba.conf` uses `scram-sha-256` for TCP. Create the role and
database; the password goes from a shell variable into psql's stdin, never onto a command line:

```bash
sudo ss -ltnp | grep 5432                           # must show 127.0.0.1:5432 (and ::1) only
sudo -u postgres psql -v ON_ERROR_STOP=1 <<'SQL'
CREATE ROLE gage_platform LOGIN;
CREATE DATABASE gage_platform OWNER gage_platform ENCODING 'UTF8';
REVOKE CONNECT ON DATABASE gage_platform FROM PUBLIC;
SQL
PGPW=$(openssl rand -hex 24)
printf "ALTER ROLE gage_platform PASSWORD '%s';\n" "$PGPW" | sudo -u postgres psql -v ON_ERROR_STOP=1
# keep $PGPW in this shell for step 7, then: unset PGPW
```

One role owns the schema and runs the app. Splitting an owner role (migrations) from a DML-only runtime role is a
worthwhile hardening once the schema settles; the platform reads a single `DATABASE_URL`.

## 7. Environment files

Rules: `/etc/gage/*.env` are mode 600 and owned by the **service user** (systemd reads them as root, the app
itself can read its own). Keep values plain `KEY=value` (quotes only where a value has spaces): systemd's parser, Node's
`--env-file` and this kit's scripts all read the same file, and they disagree on `$`, `#` and escapes. Generated secrets
are hex for that reason.

Create each file empty with the right owner first, then fill it:

```bash
sudo install -m 600 -o gage-master   -g gage-master   /dev/null /etc/gage/master.env
sudo install -m 600 -o gage-platform -g gage-platform /dev/null /etc/gage/platform.env
sudo install -m 600 -o gage-server   -g gage-server   /dev/null /etc/gage/server-1.env
sudo install -m 600 -o gage-server   -g gage-server   /dev/null /etc/gage/server-test.env
sudo install -m 600 -o root          -g root          /dev/null /etc/gage/healthcheck.env
sudo install -m 644 -o root          -g root          /dev/null /etc/gage/backup.env
sudoedit /etc/gage/master.env          # paste the block below, repeat for the other files; then check the modes:
sudo ls -l /etc/gage                   # *.env must read -rw------- and belong to their service user
```

### `/etc/gage/master.env`

Every key is documented in `apps/master/README.md`. `TRUST_PROXY=1` is right only because the service listens on loopback
and Caddy overwrites `X-Forwarded-For`.

```dotenv
HOST=127.0.0.1
PORT=8080
TTL_SECONDS=90
TRUST_PROXY=1
ALLOW_PRIVATE=0
VERIFY_UDP=1
PROBE_TIMEOUT_MS=2000
PROBE_ATTEMPTS=3
MAX_INFLIGHT_PROBES=64
MAX_PER_IP=8
MAX_ENTRIES=2000
RATE_LIMIT_BURST=20
RATE_LIMIT_PER_MINUTE=60
SWEEP_INTERVAL_SECONDS=10
SHUTDOWN_GRACE_SECONDS=10
LOG_LEVEL=info
```

`VERIFY_UDP=1`: the master sends the 24-byte GAGE query to every announcing address and lists the server only if it answers
(`docs/api/server.md` section 8). All game servers on this VPS announce from one address, so `MAX_PER_IP` (8) caps the instances
that can be listed at once.

### `/etc/gage/platform.env`

The authoritative list is `apps/platform/src/config.ts` (and `docs/api.md` there); this is what a production start needs today.
The platform refuses to start in production on a bad value and names every one.

```dotenv
NODE_ENV=production
HOST=127.0.0.1
PORT=8787
INTERNAL_HOST=127.0.0.1
INTERNAL_PORT=8788
INTERNAL_ON_PUBLIC_LISTENER=0
DATABASE_URL=postgres://gage_platform:PASTE-PGPW-HERE@127.0.0.1:5432/gage_platform
DATABASE_POOL_MAX=10
PUBLIC_API_URL=https://api.gage.example
CORS_ORIGINS=https://app.gage
TRUST_PROXY=1
LOG_LEVEL=info
BODY_LIMIT_BYTES=65536
SHUTDOWN_GRACE_SECONDS=10
REQUIRE_VERIFIED_EMAIL=1
EMAIL_DRIVER=outbox
EMAIL_FROM="GAGE-MP <no-reply@gage.example>"
EMAIL_LINK_BASE_URL=gage-mp://auth
DISCORD_CLIENT_ID=
DISCORD_CLIENT_SECRET=
DISCORD_REDIRECT_URI=https://api.gage.example/v1/auth/discord/callback
PAYMENT_PROVIDER=fake
ALLOW_FAKE_PAYMENTS_IN_PRODUCTION=1
FAKE_WEBHOOK_SECRET=PASTE-openssl-rand-hex-32-HERE
```

Fill the two secrets without echoing them:

```bash
printf 'DATABASE_URL=postgres://gage_platform:%s@127.0.0.1:5432/gage_platform\n' "$PGPW"   # copy the line into the file; then: unset PGPW
openssl rand -hex 32                                                                        # FAKE_WEBHOOK_SECRET
```

Notes that decide whether you can go live (details in CHECKLIST.md and "Open decisions"):

* `CORS_ORIGINS=https://app.gage` is the WebView2 virtual host the launcher serves its UI from (`apps/launcher/gage_gui.cpp`).
  It is not a website. Keep it exactly.
* `EMAIL_DRIVER` has only `outbox` (rows in a table, nothing is sent) and `console` (logs the message). **No real email sender exists
  in the platform yet**; a real one is a code change in `buildDeps()`, not a setting. With `REQUIRE_VERIFIED_EMAIL=1` an
  email/password account cannot verify, and `src/game/join.ts` and the shop refuse unverified accounts.
* `PAYMENT_PROVIDER` has only `fake`. In production it is refused unless `ALLOW_FAKE_PAYMENTS_IN_PRODUCTION=1`, which is what a
  **staging** box sets. Real payments need a real provider implementation first.
* `DISCORD_*`: both set or both empty (empty means off). The redirect URI must equal what you register in the Discord developer portal.

### `/etc/gage/server-<n>.env`

One admin token per instance (16 to 128 printable characters, hex here so the file is also safe to `source`):

```bash
printf 'GAGE_ADMIN_TOKEN=%s\n' "$(openssl rand -hex 24)" | sudo tee /etc/gage/server-1.env >/dev/null
printf 'GAGE_ADMIN_TOKEN=%s\n' "$(openssl rand -hex 24)" | sudo tee /etc/gage/server-test.env >/dev/null
sudo ls -l /etc/gage/server-*.env      # -rw------- gage-server gage-server
```

(`tee` keeps the mode and owner `install` gave the file.) The JSON files carry no secret, only `adminPort`; the server takes
the token from `GAGE_ADMIN_TOKEN` when the file sets none (`docs/api/server.md` section 2).
All instances run as the same user, so they can read each other's env file: instances are not isolated from each other.

### `/etc/gage/healthcheck.env` (root, mode 600) and `/etc/gage/backup.env`

```dotenv
GAGE_ALERT_WEBHOOK_URL=https://hooks.example.invalid/REPLACE-WITH-YOUR-WEBHOOK
HEALTH_MIN_SERVERS=2
HEALTH_TLS_HOSTS="api.gage.example master.gage.example"
```

```dotenv
BACKUP_DB=gage_platform
BACKUP_KEEP_DAYS=14
BACKUP_MIN_KEEP=3
```

## 8. Build and release

systemd units are already in `/etc/systemd/system` (step 4). First release of each app, as `deploy`, **in this order**
(master first: the server release and the health check use the master's compiled UDP probe):

```bash
sudo -v                                                    # cache sudo; release.sh uses it for systemctl and DB access
bash /opt/gage/deploy/scripts/release.sh master
bash /opt/gage/deploy/scripts/release.sh platform          # backs up the DB, migrates, smoke-tests, switches
bash /opt/gage/deploy/scripts/release.sh server            # builds from source, runs both test programs, smoke-tests
```

What `release.sh` does and why is in its header. In short: build into `releases/.staging-<id>`, rename to `releases/<id>`
when complete, run the **new** release as a transient unit on spare ports (`gage-smoke-<app>`) and require a fresh-boot
health answer, atomically swap `current`/`previous`, restart, require a fresh health answer again, and swap back by
itself if that fails. Any failure before the swap leaves the live release untouched.

### gage-server on its own

`release.sh server` calls `build-server.sh`; run it alone to try a build without releasing anything:

```bash
bash /opt/gage/deploy/scripts/build-server.sh --src /opt/gage/src
# = cmake -S . -B build/server -DGAGE_SERVER_ONLY=ON -G Ninja -DCMAKE_BUILD_TYPE=Release, build, then
#   build/server/selftest/netcode_test and build/server/selftest/server_test (both must exit 0)
```

`docs/api/server.md` states the Linux build "has not been run": treat this first build as untested until both test
programs print passes here. `server_test` reports one admin-channel check as SKIPPED on a host without a non-loopback
address; a VPS has one, so a SKIPPED line there is worth reading. On a 2 GB machine add `--jobs 1`.

### Platform migrations

Applied by the repo's own script, compiled: `node dist/scripts/migrate.js` (drizzle-orm's migrator over `apps/platform/drizzle/*.sql`),
run by `release.sh` as `gage-platform` with the service's env file, after a `pg_dump` labelled `pre-<release id>`.
`drizzle-kit` is the **generator** (`npm run db:generate` on the dev machine writes the SQL into `drizzle/`, which is committed);
it is a devDependency that is pruned from releases, and `drizzle.config.ts` carries no database credentials, so
`drizzle-kit migrate` could not run on the box. Rules that follow:

* Migrations run **before** the switch, so the old code runs against the new schema for a moment: make them additive
  (add columns/tables/indexes) and remove things one release later.
* A code rollback does not roll the schema back. If a migration must be undone, stop the platform and restore the
  `pre-<id>` dump (section 13).
* The platform's `npm start` script points at `dist/index.js`, but `tsconfig.build.json` has `rootDir: "."` so the real entry is
  `dist/src/index.js` (the unit uses that). `release.sh` checks the unit and the build agree and refuses to switch otherwise.

### Bootstrap the platform (once)

Compiled copies of the admin scripts are in the release (`dist/scripts/`); `npm run admin:*` is not usable on the box
because it needs `tsx`, a devDependency. Register an account in the launcher first, then:

```bash
cd /opt/gage/platform/current
sudo -u gage-platform env NODE_ENV=production /usr/bin/node --env-file=/etc/gage/platform.env \
  dist/scripts/grant-role.js --user YOUR-USERNAME --role admin
sudo -u gage-platform env NODE_ENV=production /usr/bin/node --env-file=/etc/gage/platform.env \
  dist/scripts/create-server.js --id eu-1 --name "GAGE EU 1" --region eu --host 203.0.113.10 --port 24800 --allowed-ip 127.0.0.1
```

`create-server` prints the server secret **once**; store it as the game server's credential when the C++ server can
authenticate (see Open decisions). `--allowed-ip` is the TCP peer the platform sees on its internal listener: `127.0.0.1` for servers on this box.

## 9. Caddy

```bash
sudo cp /etc/caddy/Caddyfile /etc/caddy/Caddyfile.dist
sudo cp /opt/gage/deploy/caddy/Caddyfile /etc/caddy/Caddyfile
sudo sed -i 's/gage\.example/YOUR.DOMAIN/g' /etc/caddy/Caddyfile
sudo systemctl edit caddy            # add the two lines below, save
```

```ini
[Service]
EnvironmentFile=/etc/gage/network.env
```

DNS first: `A api.YOUR.DOMAIN` and `A master.YOUR.DOMAIN` to the VPS (add `AAAA` only after you have tested IPv6 end to end;
announces use the IPv4 address on purpose, see section 10). Ports 80 and 443 must be reachable for Let's Encrypt.

```bash
sudo bash -c 'set -a; . /etc/gage/network.env; set +a; caddy validate --config /etc/caddy/Caddyfile'
caddy fmt --diff /etc/caddy/Caddyfile
sudo systemctl daemon-reload && sudo systemctl reload caddy      # reload, never restart
sudo journalctl -u caddy -n 30 --no-pager                        # "certificate obtained successfully" for both names
```

What the Caddyfile does: TLS for `api.` and `master.`; `/internal/*` and `/v1/dev/*` answer 404 on the API name;
the master name serves only `GET|HEAD /servers` and `/healthz`; a plain-HTTP listener on `:8081` that accepts only
`POST /announce` from `GAGE_ANNOUNCE_ALLOW` and 403s the rest. Why plain HTTP: `gage-server` has no TLS client and its
`masterUrl` rejects `https://`; the announce body is public listing data, so the risk is forged listings, not secrecy,
and that is bounded by the IP allow-list in Caddy and ufw plus the master's own caps and UDP probe.
Both proxied apps run with `TRUST_PROXY=1` and are bound to loopback; Caddy overwrites `X-Forwarded-For` with the real peer.
Security headers: HSTS, nosniff, DENY framing, no-referrer, CSP, COOP; access logs go to journald with the OAuth `code`/`state`
query values redacted (Caddy already redacts `Authorization` and cookies).

**Rate limiting.** Stock Caddy has no request rate limiter (it is the third-party `caddy-ratelimit` module, which needs an
`xcaddy` build; confirmed in current Caddy community and module docs). This kit therefore applies what Caddy does natively
(body-size caps, header/body/idle timeouts, header-size cap, no HTTP/3) and leaves commented `rate_limit` blocks in the
Caddyfile. The limits that exist today live in the apps: master `POST /announce` token bucket and per-address caps;
platform Postgres-backed login and account throttles. `GET /servers` and the platform's other routes have no per-IP
limiter in front. Decision for the owner: accept that, or build Caddy with the module (adds a non-distro binary to patch).

## 10. Game server instances

Per instance `N` (here `1` and `test`; ports 24800/24900 and 24801/24901, inside the firewall's 24800-24809 range):

```bash
sudo install -m 644 -o root -g root /opt/gage/deploy/server-instances/server-1.json    /etc/gage/server-1.json
sudo install -m 644 -o root -g root /opt/gage/deploy/server-instances/server-test.json /etc/gage/server-test.json
sudo sed -i 's#203\.0\.113\.10#YOUR.VPS.IPV4#' /etc/gage/server-1.json                 # masterUrl
sudoedit /etc/gage/server-1.json                                                          # name, maxPlayers, gamemode, lang, url
sudo systemctl enable --now gage-server@test      # canary first
sudo systemctl enable --now gage-server@1
```

Config rules worth knowing (`docs/api/server.md` section 2): unknown keys, wrong types and trailing data are fatal at
start, so a typo cannot leave a default in place; `masterUrl` is `http://` only; `adminPort` needs the token from
`/etc/gage/server-N.env`; `bansFile` is relative to the unit's `WorkingDirectory` (`/var/lib/gage/server-N`).

`masterUrl` uses the VPS's **IPv4 literal** on purpose: the master lists the TCP source address plus the game port, and the
server is IPv4-only. A host name with an AAAA record could make the server announce from an IPv6 address players cannot
reach. `server-test.json` has an empty `masterUrl` (not listed); set it to the same URL to test the announce path.

Operate an instance through its loopback admin channel (`list`, `say`, `kick`, `ban`, `banip`, `unban`, `bans`, `quit`):

```bash
sudo bash -c 'set -a; . /etc/gage/server-1.env; set +a; printf "auth %s\nlist\n" "$GAGE_ADMIN_TOKEN" | nc -q1 127.0.0.1 24900'
# from your laptop, through SSH (AllowTcpForwarding local is on):  ssh -L 24900:127.0.0.1:24900 deploy@YOUR-VPS
```

Add an instance: copy a JSON (new `port` and `adminPort`, both free; the UDP port inside 24800-24809, otherwise widen
`GAGE_GAME_UDP_RANGE` and re-run `ufw.sh`), create `/etc/gage/server-N.env` (token), `/var/lib/gage/server-N` (0750, gage-server), then
`sudo systemctl enable --now gage-server@N`. Remember the master's `MAX_PER_IP` (8) and `maxConnectionsPerIp` per server.
Rotate a token: write a new value into the env file, `sudo systemctl restart gage-server@N` (kicks the players).

## 11. Start, enable, verify

```bash
sudo systemctl enable --now gage-master gage-platform
sudo systemctl enable --now gage-backup.timer gage-healthcheck.timer
systemctl list-timers 'gage-*'
systemctl --failed
```

Locally on the box:

```bash
curl -fsS http://127.0.0.1:8080/healthz            # {"status":"ok","servers":N,"uptimeSeconds":...}
curl -fsS http://127.0.0.1:8787/health             # {"status":"ok","uptimeSeconds":...,"version":...}   (503 = database down)
node /opt/gage/deploy/scripts/udp-probe.mjs 127.0.0.1 24800    # {"ok":true,"protocol":1,"players":0,"maxPlayers":64,...} (the master's own probe)
sudo journalctl -u gage-server@1 -n 30 --no-pager  # listening on UDP 24800 ... announcing to ... master: listed as <ip>:24800 (new entry)
sudo journalctl -u gage-master -n 20 --no-pager    # JSON lines: "server listed"
systemd-analyze security --no-pager gage-platform.service gage-master.service gage-server@1.service | tail -n 8   # exposure scores; read the lines marked UNSAFE
```

Through Caddy, from **another machine** (this is what the launcher sees):

```bash
curl -fsS https://api.YOUR.DOMAIN/health
curl -fsS https://master.YOUR.DOMAIN/servers                      # the listed servers, keyed ip:port
curl -sS -o /dev/null -w '%{http_code}\n' https://master.YOUR.DOMAIN/announce -X POST      # 404: not offered over HTTPS
curl -sS -o /dev/null -w '%{http_code}\n' https://api.YOUR.DOMAIN/internal/game/wallet     # 404
curl -sS -m 5 -o /dev/null -w '%{http_code}\n' http://YOUR.VPS.IPV4:8081/announce -X POST  # timeout from a non-allowed address (ufw drops it)
nmap -Pn -p 22,80,443,5432,8080,8081,8787,8788,24900 YOUR.VPS.IPV4          # only 22, 80, 443 open
# UDP is the one that lies (nc/nmap say open|filtered): ask the game server itself, with the repo's compiled master probe
node deploy/scripts/udp-probe.mjs YOUR.VPS.IPV4 24800 --module apps/master/dist/probe.js     # {"ok":true,...}
```

The launcher then needs `kDefaultListUrl` (or the `server_endpoint` registry value) set to `https://master.YOUR.DOMAIN/servers`
and the platform base URL to `https://api.YOUR.DOMAIN` (client side, not part of this kit).

## 12. Update and rollback

Everything below is run as `deploy`. Release directories are `releases/<UTC timestamp>-<git sha>`; `current` and `previous` are
symlinks; the newest `--keep` (default 5) plus `current` and `previous` are kept.

```bash
cd /opt/gage/src && git pull --ff-only
sudo rsync -a --delete --chown=root:root /opt/gage/src/deploy/ /opt/gage/deploy/     # if deploy/ changed; then daemon-reload for unit changes
bash /opt/gage/deploy/scripts/release.sh master   --dry-run                           # print the plan
bash /opt/gage/deploy/scripts/release.sh master
bash /opt/gage/deploy/scripts/release.sh platform                                     # --test to run npm test first
bash /opt/gage/deploy/scripts/release.sh server --instances test                      # canary: build, test, smoke, restart only @test
bash /opt/gage/deploy/scripts/release.sh server --instances 1 --yes                   # then the rest (--yes: players online)
```

* A failed health check after a restart swaps the links back and restarts the old release by itself (exit 1,
  `ROLLED BACK to <id>`); the failed release stays in `releases/` for inspection.
* `release.sh server` refuses to restart an instance that has players unless `--yes`, and restarting a game server
  drops its players: announce first (`say` on the admin channel). `--no-restart` switches the binary and leaves running
  servers on the old one until their next restart.
* Manual rollback: `release.sh <app> --list`, `release.sh <app> --rollback` (swaps with `previous`), or `--rollback --to <id>`.
* Platform rollback = code only. If the new release's migration cannot be undone by running the old code, stop the platform and restore
  the `pre-<id>` dump (section 13) before rolling back.
* First release of an app that fails leaves the unit stopped and no `current`: fix the cause, release again.

## 13. Backups

`gage-backup.timer` runs `backup-db.sh` nightly at 23:30 UTC (03:30 Tbilisi) as the `postgres` OS user:
`pg_dump --format=custom`, written to `.partial-*`, read back with `pg_restore --list`, checksummed (`.sha256`), then renamed.
Retention: older than `BACKUP_KEEP_DAYS` (14) are deleted, but the newest `BACKUP_MIN_KEEP` (3) never are. A dump before each platform
release is labelled `pre-<id>` and counts toward retention like any other.

```bash
sudo systemctl start gage-backup.service && sudo journalctl -u gage-backup -n 20 --no-pager
sudo ls -lh /var/backups/gage
sudo bash /opt/gage/deploy/scripts/backup-db.sh --dry-run      # shows what retention would delete
```

Dumps on the same disk do not survive losing the VPS. Add an off-site copy: `BACKUP_POST_HOOK=/etc/gage/backup-offsite.sh` in
`/etc/gage/backup.env` (root-owned executable, run as `postgres` with the dump path as `$1`, a failure fails the backup run), for
example to a Hetzner Storage Box (rsync over SSH on port 23, a key readable only by `postgres`) or an encrypted bucket via `rclone`.
Hetzner snapshots are a useful second layer for the machine, not a substitute for dumps.

**Restore drill: do it before go-live and monthly.** It restores the newest dump into a scratch database next to the live one,
checks it has tables and applied migrations, and drops it:

```bash
sudo bash /opt/gage/deploy/scripts/restore-db.sh --file latest --target-db gage_restore_drill --drop-after
sudo bash /opt/gage/deploy/scripts/restore-db.sh --file latest --target-db gage_restore_drill --dry-run     # plan only
```

Compare `applied migrations` with `ls /opt/gage/platform/current/drizzle/*.sql | wc -l`. A drill is complete only when the
log ends with `done`; record the date in CHECKLIST.md.

Restore over the live database (disaster): `sudo systemctl stop gage-platform`, then
`sudo bash restore-db.sh --file /var/backups/gage/<dump> --force`, then `sudo systemctl start gage-platform`. The script refuses a
non-empty target without `--force`, refuses the live database while `gage-platform` runs even with `--force`, checks the
`.sha256`, and restores in one transaction (a failure leaves the dropped-and-recreated empty database, not a half-restored one).
A restore onto a fresh machine needs the `gage_platform` role to exist first (step 6).

## 14. Logs and retention

All services log to journald (JSON lines from the Node apps, timestamped text from `gage-server`, Caddy's JSON access log):

```bash
sudo journalctl -u gage-platform -f
sudo journalctl -u 'gage-server@*' --since '1 hour ago' -g 'refuse|ban|ERROR'
sudo journalctl -u caddy -o cat | tail -n 20
sudo journalctl -p warning --since today
```

Retention is set by `/etc/systemd/journald.conf.d/10-gage.conf`: 1 GB or 30 days, whichever comes first. Logs hold player IP
addresses (game server `connect`/`join` lines, Caddy access log, platform access log): 30 days is a deliberate cap, keep it
consistent with your privacy policy. PostgreSQL writes its own file log under `/var/log/postgresql/`, rotated by the packaged
logrotate rule. No file logs are written by the GAGE services, so no logrotate rule is needed for them.

## 15. Monitoring

* systemd: `Restart=on-failure` with `StartLimitBurst=5` per 300 s on every service; `gage-server` does not restart on exit codes 1/2
  (config, ban list, bind failure: restarting cannot fix them). `systemctl --failed` lists what gave up.
* `gage-healthcheck.timer` (every minute): master `/healthz`, platform `/health` (needs the database), PostgreSQL, the units
  `gage-master`, `gage-platform`, `caddy`, `postgresql`, every **enabled** `gage-server@` instance (active and answering the UDP query),
  at least `HEALTH_MIN_SERVERS` of them, root disk under 90 %, newest dump younger than 36 h, TLS certificates (`HEALTH_TLS_HOSTS`) valid for
  14 more days. A check that cannot run counts as failed.
* Alerts go to `GAGE_ALERT_WEBHOOK_URL` (a placeholder; the JSON carries both `content` for Discord and `text` for Slack, adapt it to
  your provider in `send_alert`): after 2 consecutive failing runs, again when the set of failures changes, every hour while broken, and
  once on recovery. Test it: `sudo systemd-run --pipe --wait -p EnvironmentFile=/etc/gage/healthcheck.env -p DynamicUser=yes bash /opt/gage/deploy/scripts/healthcheck.sh --test-alert`.
* Add one external uptime monitor from outside the datacentre on `https://api.YOUR.DOMAIN/health` and `https://master.YOUR.DOMAIN/healthz`: the
  on-box check cannot see an outage of the whole machine.
* Periodically: `systemd-analyze security <unit>`, `sudo ufw status`, `sudo fail2ban-client status sshd`, `df -h /`, `journalctl --disk-usage`.

## 16. Open decisions

| Decision | Why it is open |
|---|---|
| Domain, DNS, ACME contact | placeholders everywhere; also the Discord and payment URLs derive from it |
| Email provider | the platform has no real sender (`outbox`/`console` only): with `REQUIRE_VERIFIED_EMAIL=1` email accounts cannot join or buy. Needs code, then settings |
| Payment provider | only `fake` exists; production refuses it unless `ALLOW_FAKE_PAYMENTS_IN_PRODUCTION=1` (staging) |
| Number of game servers | sizes the UDP range, `MAX_PER_IP` (8 per announcing address), RAM, `HEALTH_MIN_SERVERS` |
| Community master list | the announce allow-list makes this an official-servers-only master; open it deliberately (the master's caps and UDP probe are built for that) or keep it closed |
| Request rate limiting in front | stock Caddy has none: accept app-level limits only, or build Caddy with `caddy-ratelimit` |
| Game servers authenticating to the platform | `apps/server/httpclient` cannot send the `X-Gage-Server-*` headers yet (`platform/docs/game-server-contract.md`): join tokens are not verified by the C++ server until that exists |
| Off-site backup target | hook is a placeholder |
| Node 22 maintenance LTS | schedule the move to Node 24 |
| Auto-reboot window | disabled on purpose; owners pick a window |

## Verification status

Validated on the Windows dev machine: `bash -n` on every script; the pure helpers (IP and CIDR validation, env-file parsing, JSON field
extraction, retention selection, ufw rule parsing, SSH-port guard parsing) and each script's argument handling with unit tests that fail
against a mutated copy; `release.sh` end to end for `master` with stubbed `systemctl`/`systemd-run`/`curl`/`npm` (first release, second,
live-failure auto-rollback, smoke failure, failing tests, manual rollback, prune, dry-run, list) and `healthcheck.sh`'s alert state machine
with a stubbed webhook, both failing against mutants; `udp-probe.mjs` and both example server configs against the real `gage-server`
(Windows build of the same sources): UDP query answered, admin channel authenticates, wrong token rejected; JSON validity and key names.

**Not** validated, because the tools or the target are absent here: `shellcheck`, `systemd-analyze verify`, `caddy validate`/`caddy fmt`, `ufw`,
`systemd-run` behaviour, PostgreSQL (`backup-db.sh` and `restore-db.sh` were never run against a database), a Linux build of `gage-server`,
`release.sh` for `platform` and `server` beyond the shared code paths, the sandboxing options on the real binaries (`SystemCallFilter`
returns EPERM instead of killing, so a too-tight filter degrades rather than crash-loops; check `systemd-analyze security` and the journal on the
first start). Caddy syntax was checked against the current Caddy documentation, not by Caddy.
