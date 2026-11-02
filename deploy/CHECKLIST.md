# GAGE-MP go-live checklist

Tick an item only with its evidence (command output, a screenshot, a timestamp). `README.md` has the procedures; section numbers
below point into it. Replace `YOUR.DOMAIN` and `YOUR.VPS.IPV4`. "From outside" means a machine that is not the VPS and not on the
VPS's network (a laptop on mobile data is fine).

## Blockers that need a decision or code first

- [ ] **Email provider.** The platform ships only `EMAIL_DRIVER=outbox|console`; neither delivers mail. With `REQUIRE_VERIFIED_EMAIL=1` an
      email/password account cannot verify, so it cannot join a server or buy (`apps/platform/src/game/join.ts`, `shop/service.ts`).
      Done when: a real `EmailSender` is plugged into `buildDeps()`, its credentials are in `/etc/gage/platform.env`, and a registration
      from outside delivers a verification mail that completes the flow. SPF, DKIM and DMARC records exist for the sending domain, and
      `EMAIL_FROM` uses it. (Until then: Discord-only login, or `REQUIRE_VERIFIED_EMAIL=0` knowingly.)
- [ ] **Payment provider.** Only `PAYMENT_PROVIDER=fake` exists and production refuses it unless `ALLOW_FAKE_PAYMENTS_IN_PRODUCTION=1`
      (a staging switch; it grants items for nothing). Done when: a real provider is implemented, `ALLOW_FAKE_PAYMENTS_IN_PRODUCTION` and
      `FAKE_WEBHOOK_SECRET` are gone from `/etc/gage/platform.env`, and a test payment in the provider's sandbox reaches `applied`
      in `GET /v1/shop/orders/:id`.
- [ ] **Game servers can authenticate to the platform.** `apps/server/httpclient` cannot send `X-Gage-Server-Id` / `-Secret` yet
      (`apps/platform/docs/game-server-contract.md`). Until it can, join tokens are not verified by the C++ server.
- [ ] **Request rate limiting decision** recorded (README section 9): accept app-level limits only, or build Caddy with `caddy-ratelimit` and
      uncomment the `rate_limit` blocks. Either way write the decision here: ____________________

## DNS and TLS

- [ ] `A api.YOUR.DOMAIN` and `A master.YOUR.DOMAIN` point at YOUR.VPS.IPV4 (`dig +short api.YOUR.DOMAIN master.YOUR.DOMAIN`). No `AAAA`
      unless IPv6 was tested end to end.
- [ ] Reverse DNS (PTR) of YOUR.VPS.IPV4 set in the Hetzner console (matters for any mail you send from this IP).
- [ ] Certificates issued: `sudo journalctl -u caddy --no-pager | grep -i 'certificate obtained'` shows both names.
- [ ] From outside: `curl -vI https://api.YOUR.DOMAIN/health 2>&1 | grep -E 'SSL connection|subject|expire|HTTP/'` shows a valid Let's Encrypt
      certificate, TLS 1.2+ and `HTTP/2 200`. Same for `master.YOUR.DOMAIN/healthz`.
- [ ] `http://api.YOUR.DOMAIN/health` redirects to HTTPS (`curl -sI`).
- [ ] Security headers present (`curl -sI https://api.YOUR.DOMAIN/health`): `strict-transport-security`, `x-content-type-options: nosniff`, `x-frame-options: DENY`,
      `referrer-policy`, `content-security-policy`; no `server` header.
- [ ] Renewal path is alive: Caddy's storage `/var/lib/caddy` exists and the `gage-healthcheck` TLS check (`HEALTH_TLS_HOSTS`) passes.

## Firewall verified from outside

- [ ] `sudo ufw status verbose` shows deny (incoming), allow only: SSH, 80/tcp, 443/tcp, `24800:24809/udp`, `8081/tcp` from the announce addresses.
- [ ] From outside: `nmap -Pn -p 22,80,443,5432,8080,8081,8787,8788,24900,24901 YOUR.VPS.IPV4` shows **only** 22, 80, 443 open (8081 closed or filtered from a
      non-allowed address).
- [ ] From outside, UDP (nmap cannot tell open from filtered): `node deploy/scripts/udp-probe.mjs YOUR.VPS.IPV4 24800 --module apps/master/dist/probe.js`
      prints `"ok":true` for every instance port.
- [ ] From outside: `curl -sS -m 5 -X POST http://YOUR.VPS.IPV4:8081/announce` times out; from an allow-listed address with a valid body it returns 200.
      `curl -sS -o /dev/null -w '%{http_code}' -X POST https://master.YOUR.DOMAIN/announce` is 404.
- [ ] `GAGE_ANNOUNCE_ALLOW` in `/etc/gage/network.env` lists exactly the addresses game servers announce from (include the VPS's own address for local servers); `ufw.sh --apply`
      was re-run and `sudo systemctl reload caddy` done after the last edit.
- [ ] `https://api.YOUR.DOMAIN/internal/game/wallet` is 404 and `/v1/dev/fake-checkout/x` is 404.
- [ ] Hetzner Cloud Firewall (optional second layer) mirrors the table, if used.

## Access and hardening

- [ ] `ssh root@YOUR.VPS.IPV4` refused; password login refused (`ssh -o PubkeyAuthentication=no deploy@YOUR.VPS.IPV4`); `sudo sshd -T | grep -E 'passwordauthentication|permitrootlogin'` says `no`.
- [ ] `sudo fail2ban-client status sshd` shows the jail running.
- [ ] `sudo unattended-upgrade --dry-run --debug` is clean; the reboot-window owner is named: ____________________
- [ ] `timedatectl` says `System clock synchronized: yes`, time zone UTC.
- [ ] `ss -ltnp` shows nothing public except sshd and Caddy; PostgreSQL, master, platform, admin channels on 127.0.0.1 only.
- [ ] `systemd-analyze security gage-platform gage-master gage-server@1` reviewed; no unit starts with a SIGSYS or a denied-syscall flood in its journal.
- [ ] `/etc/gage/*.env` are `-rw-------` and owned by their service user (`sudo ls -l /etc/gage`); no secret appears in `systemctl show -p Environment <unit>`, in `ps`, or in the repository.

## Secrets and admin access

- [ ] **Admin tokens rotated** after the first start and after anyone who saw them (setup logs, screen shares) is done: new `GAGE_ADMIN_TOKEN` in each
      `/etc/gage/server-N.env` (`openssl rand -hex 24`), `sudo systemctl restart gage-server@N`, old token refused
      (`printf 'auth OLDTOKEN\nlist\n' | nc -q1 127.0.0.1 24900` answers `bad token`).
- [ ] Admin ports unreachable from outside (nmap line above) and bound to loopback (`ss -ltn | grep 249` shows `127.0.0.1` only).
- [ ] `FAKE_WEBHOOK_SECRET`, the database password and any provider keys were generated on the box (hex) and exist nowhere else; the database
      password in `DATABASE_URL` is the one set in step 6 (`sudo -u postgres psql -c '\du'` shows `gage_platform` with LOGIN only).
- [ ] The first platform admin exists (`grant-role.js`, README section 8); the official servers are registered (`create-server.js`) and their secrets are stored in a password manager,
      not in a file in the repository.
- [ ] Second person (or a printed sheet in a safe place) can reach: the VPS console, the DNS provider, the Hetzner project, the backup target, the webhook/alert channel.

## OAuth, email and payment callbacks

- [ ] **Discord OAuth.** Application in the Discord developer portal with redirect URL exactly `https://api.YOUR.DOMAIN/v1/auth/discord/callback`
      (the platform default for `PUBLIC_API_URL`; if `DISCORD_REDIRECT_URI` is set it must be the same string). `DISCORD_CLIENT_ID` and
      `DISCORD_CLIENT_SECRET` in `/etc/gage/platform.env` (both or neither). Done when a login from the launcher completes and a first-time Discord
      user reaches the username step.
- [ ] **Email.** (see Blockers) `EMAIL_FROM`, SPF/DKIM/DMARC, and `EMAIL_LINK_BASE_URL=gage-mp://auth` matches the launcher's registered protocol; password reset and
      verification mails arrive within a minute and the links open the launcher.
- [ ] **Payment webhook URL.** In the provider's dashboard: `https://api.YOUR.DOMAIN/v1/webhooks/<provider id>` (route `POST /v1/webhooks/:provider`, signature verified over the raw body,
      body limit 256 KiB which Caddy allows up to 512 KB). The provider's signing secret is in the platform env. Done when the provider's "send test event" returns 200 and a sandbox
      payment turns an order into `paid`.
- [ ] `CORS_ORIGINS=https://app.gage` unchanged (the launcher's WebView2 origin) and the launcher can call `https://api.YOUR.DOMAIN` (no CORS error in its dev console).
- [ ] The launcher build points at the production endpoints: master list `https://master.YOUR.DOMAIN/servers`, platform `https://api.YOUR.DOMAIN`, update URL (`GAGE_UPDATE_URL`).

## Services

- [ ] `systemctl --failed` is empty; `systemctl is-enabled gage-master gage-platform caddy postgresql gage-backup.timer gage-healthcheck.timer gage-server@1` all `enabled`.
- [ ] Every enabled game server answers the UDP query from the box (`udp-probe.mjs`) and is **listed**: `curl -fsS https://master.YOUR.DOMAIN/servers` shows its `ip:port`, `players`, `maxplayers`.
- [ ] A real client joins a server through the launcher end to end (login, join token, connect, chat, leave).
- [ ] A restart test per service: `sudo systemctl restart gage-master` (list refills within one heartbeat, 30 s), `gage-platform` (health back, no 5xx burst beyond the restart),
      `gage-server@test` (players drop and rejoin). `sudo kill -9 $(systemctl show -p MainPID --value gage-master)` is restarted by systemd within seconds.
- [ ] `release.sh <app> --dry-run` and one real release plus one `--rollback` rehearsed for each app on the production box (or on a staging copy) and the result recorded here: ____________________
- [ ] Platform migrations applied: `ls /opt/gage/platform/current/drizzle/*.sql | wc -l` equals the `applied migrations` count printed by `restore-db.sh` in the drill.

## Backups

- [ ] `gage-backup.timer` listed in `systemctl list-timers`; at least one nightly dump exists with a `.sha256`; `sudo journalctl -u gage-backup` shows `dump complete`.
- [ ] **Restored once:** `sudo bash /opt/gage/deploy/scripts/restore-db.sh --file latest --target-db gage_restore_drill --drop-after` ended with `done`, table count and migration count
      plausible. Date and operator: ____________________ (repeat monthly).
- [ ] Off-site copy configured (`BACKUP_POST_HOOK`) and a file from it restored on a different machine. Target: ____________________
- [ ] Retention understood and set (`BACKUP_KEEP_DAYS`, `BACKUP_MIN_KEEP`); disk space for 14 dumps plus a restore drill checked (`df -h /var/backups`).
- [ ] The restore-over-live procedure (README section 13) read by the person who would run it at 3 am.

## Rate limits and abuse controls

- [ ] Master: `RATE_LIMIT_BURST`, `RATE_LIMIT_PER_MINUTE`, `MAX_PER_IP` (>= number of local game servers), `MAX_ENTRIES`, `VERIFY_UDP=1` set as intended; a forged announce from an allowed
      address for a closed port is rejected `422 unreachable` (test with a throwaway instance).
- [ ] Game server: `maxConnectionsPerIp`, `chatBurst`, `chatWindowSeconds` set per instance; UDP query replies are capped by the server (20/s per address, 2000/s total).
- [ ] Platform: login throttles (`LOGIN_*`), `BODY_LIMIT_BYTES`, `SESSION_TTL_DAYS` reviewed; Caddy body caps (512 KB api, 8 KB master) and timeouts in place.
- [ ] Request-rate limiting in front of `GET /servers` and the platform: see the Blockers decision.
- [ ] Hetzner abuse/DDoS expectations understood (UDP floods on the game ports are not stopped by this kit): decide on a provider-level filter or an upstream mitigation if the game becomes a target.

## Logs and monitoring

- [ ] **Log retention:** `journalctl --disk-usage` below the 1 GB cap, `/etc/systemd/journald.conf.d/10-gage.conf` in place and journald restarted since (`systemctl show -p ActiveEnterTimestamp systemd-journald`),
      persistent storage (`ls /var/log/journal`). Retention (30 days) matches the privacy policy: ____________________
- [ ] Caddy access log shows requests with `code=REDACTED` on the Discord callback (`sudo journalctl -u caddy -o cat | grep discord/callback | head -n 2`).
- [ ] **Alert path tested:** the `--test-alert` command (README section 15) delivered to the real channel; then `sudo systemctl stop gage-master`, wait 3 minutes: an ALERT arrives; start it: a RECOVERED
      arrives. Who receives it and who answers at night: ____________________
- [ ] External uptime monitor on `https://api.YOUR.DOMAIN/health` and `https://master.YOUR.DOMAIN/healthz`, alerting a different channel than the on-box one.
- [ ] Disk alarm: `HEALTH_DISK_MAX_PCT` appropriate; Hetzner volume/backups plan decided.

## After launch (first week)

- [ ] Day 1, 3, 7: `systemctl --failed`, `journalctl -p warning --since yesterday`, backup list, `ufw status`, `fail2ban-client status sshd`, certificate expiry.
- [ ] First kernel update rebooted in an announced window; all units back, game servers re-listed.
- [ ] First real release done with `release.sh`, rollback target confirmed with `release.sh <app> --list`.
