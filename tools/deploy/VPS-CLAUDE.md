# Solar RP VPS: working agreement

This file is `tools/deploy/VPS-CLAUDE.md` in the repository. `tools/deploy/unpack-source.sh` installs it as `/opt/gage/CLAUDE.md` on every unpack, so edit it on the PC, not here. Full procedures are in `/opt/gage/src/docs/OPS.md`; read the section you are about to run before running it.

## Who and how

- Address the user as "GI" at the start of every response. Times are GMT+4 (Tbilisi).
- Terse reports. A claim is verified only when you observed the thing itself: the file bytes, the HTTP body, the log line, the exit code. HTTP 200 proves nothing by itself; a systemd "active" proves nothing without the UDP query answering.
- Numbers from earlier sessions, reviewers or logs are hypotheses until you re-measure them.
- If a step fails, read the FIRST error line of the log (`/opt/gage/logs/<UTC stamp>-<step>.log`), never `| tail` it away, and tell GI the exact line before trying a fix.

## What this machine is

Ubuntu 24.04 (not Hetzner: no cloud firewall in front, ufw is the firewall and GI opened the ports), public IP 5.175.253.187, domain solar-rp.com (A records for `@` and `www`). It does three jobs: builds the Windows launcher and installer by cross-compiling (mingw-w64 + wine + Inno Setup), signs and publishes launcher updates, and runs the game server `gage-server` (UDP 24800) for the Solar RP servers. Player-facing name: Solar RP (installer `SolarRP-Setup-<ver>.exe`, launcher `SolarLauncher.exe`; the live server is named SOL, later LUNA and TERRA). Internal names stay GAGE: `gage-server`, `gage_mp.dll`, user `gage`, `/opt/gage`, `gage-server.service`.

| Path | What |
| --- | --- |
| `/opt/gage/src` | the source tree from the PC. REPLACED on every unpack (previous copy: `/opt/gage/src.prev`). Never edit it here: changes made here are lost and never reach the PC. |
| `/opt/gage/src/build` | symlink to `/opt/gage/build`, the persistent build tree |
| `/opt/gage/server/{current,releases,data}` | the running game server: `current` is a symlink to a release; `data/` is the only writable part (bans, gamemode data) |
| `/opt/gage/{logs,tmp,wine,inno,releases}` | build logs, scratch, the wine prefix, the Inno Setup copy, archived installers |
| `/var/www/gage/update/` | what Caddy serves: `version.json` (signed) and the installers |
| `/etc/gage/` | `deploy.env` (settings, no secrets), `server.env` (admin token: never print it), `bootstrap-versions.txt` |
| `/root/.gage-keys/` | the update signing key (`update-signing-key.pem`, mode 600) and `update-signing-pub.txt` (public) |

## Hard rules

1. Never print, `cat`, copy, log, quote or put in a command line the private key `/root/.gage-keys/update-signing-key.pem`, or the value of `GAGE_ADMIN_TOKEN`. `update-signing-pub.txt` is public and may be shown. Check that a secret exists with `test -s` or `stat -c %a`, never by reading it. Never ask GI to paste either into chat.
2. Never kill processes by search result. No `pkill`, `killall`, `kill $(pgrep ...)`, `kill $(ps ... | grep ...)`. End only a child you started yourself, through its own PID handle. Services go through `systemctl` (and the game server through `deploy-server.sh`). If something unexpected holds a port or runs, report the PID and its command line to GI instead of killing it.
3. Do not restart or replace the game server while players are on it without GI saying yes in this conversation. `deploy-server.sh` stops at exit 3 when players are online; do not add `--allow-players` on your own judgment. A server started by hand in tmux (from the days before the unit) counts as live: deploy-server.sh exits 6 while it still answers on 24800; ask GI before stopping it.
4. Run the selftests only by the names in `tools/deploy/wine-selftests.list` (build-client.sh does that). Never loop over `*.exe` in the selftest folder: it contains `gage_v.exe`, which launches GTA and rewrites the registry handoff.
5. Never `rm -rf` anything you did not create in this session, never touch `/opt/gage/build` or `/opt/gage/wine` while a build runs (the `build` lock is held; `ls /tmp/gage-build.lock` to see), never delete `/opt/gage/server/data`, `/opt/gage/server/releases` or `/var/www/gage/update` contents by hand (publish-client.sh does the retention).
6. No `git init`, commits, pushes or new dependencies on this machine. The tree is not a git repository; the PC is the source of truth. No package installs beyond what `bootstrap-vps.sh` does, without GI's yes.
7. No inline python/node through shell heredocs, `python -c` or `node -e` for anything with quotes, backslashes or non-ASCII. Write a script file under `/opt/gage/tmp/<task>/` and run it by path.
8. Do not change ufw, the Caddyfile or the systemd unit except through the repository files (edit on the PC, repack, unpack, then `bootstrap-vps.sh` is idempotent). Never claim "ports are open" from a loopback or self-address test (the host or its network can still filter what ufw allows); a valid Let's Encrypt certificate and a request that comes from outside prove it.
9. The repository's own `CLAUDE.md` (in `/opt/gage/src`) describes the PC workflow: WinLibs, `build/win` on Windows, `.toolchain` paths, Windows registry. Where it disagrees with this file, this file wins on this machine.

## The kit (all under `/opt/gage/src/tools/deploy/`, details in docs/OPS.md)

| Script | Use |
| --- | --- |
| `unpack-source.sh TARBALL --sha256 HEX [--dry-run]` | swap in a new source tree from the PC |
| `bootstrap-vps.sh [--dry-run]` | one-time (idempotent) machine setup: ufw, packages, wine, Node, Caddy, users, directories, the unit |
| `build-client.sh [--dry-run] [--tests soft]` | cross-compile the launcher, run the selftests under wine, build `SolarRP-Setup-<ver>.exe` |
| `publish-client.sh <ver> [--notes TEXT] [--dry-run]` | sign and publish the update manifest (atomic), verify it over HTTP(S) |
| `deploy-server.sh [--dry-run] [--build-only]` | build, test, canary and roll out `gage-server`; `--status`, `--rollback` |
| `release.sh <ver> --notes TEXT [--countdown S] [--jobs N] [--dry-run]` | client + server together: build both, publish the client `--mandatory`, announce in game, activate the server with `--allow-players`, check the live protocol. It restarts the server with players on it (after the countdown): run it only with GI's yes in this conversation, `--dry-run` first |
| `deploy-site.sh [--dry-run] [--rollback] [--skip-build] [--keep N] [--no-http-check]` | build `apps/website`, release it to `/var/www/gage/site-releases/<stamp>`, switch the live symlink, check through Caddy |

Every script takes `--dry-run`: run it first and read what it would do. Order of a normal launcher release: `unpack-source.sh` (new tarball) then `build-client.sh --dry-run`, `build-client.sh`, `publish-client.sh <ver> --dry-run`, `publish-client.sh <ver> --notes "..."`, then check from outside what players get: `curl -fsS https://solar-rp.com/update/version.json`. The version must be the `kClientVersion` in `apps/launcher/gage_gui.cpp`, and strictly newer than what is published (launchers refuse anything else).

`build-client.sh` refuses a launcher that has neither `GAGE_API_BASE` nor `GAGE_TEST_SERVER` (it would refuse every PLAY with code 13: demo mode, no platform API yet). `bootstrap-vps.sh` never overwrites an existing `/etc/gage/deploy.env`, so if that file predates this kit, add `GAGE_TEST_SERVER=5.175.253.187:24800` to it (by hand, with GI's yes) before `build-client.sh`.

## Website (solar-rp.com)

The site is `apps/website`, built with `npm ci && npm test && npm run build` (Node >= 22.12). Deploy it only with `deploy-site.sh`; never copy files into `/var/www/gage` by hand.

```
bash tools/deploy/deploy-site.sh --dry-run     # every check, every change printed, nothing changed
bash tools/deploy/deploy-site.sh               # npm ci + tests + build, check dist, new release, switch the symlink, HTTP check
bash tools/deploy/deploy-site.sh --rollback    # back to the newest release older than the live one (no build)
```

`--skip-build` uses `apps/website/dist` as it is, `--keep N` keeps N releases (default 3, minimum 2), `--no-http-check` skips the check through Caddy. Releases are `/var/www/gage/site-releases/<UTC stamp>/`; the live site is the symlink `/var/www/gage/site`, which Caddy serves for every path except `/update/*` and `/healthz` (a site deploy never touches the update feed). A failed HTTP check switches back by itself. Exit codes: 0 ok, 1 failure, 2 usage, 75 another deploy-site is running. Site changes are made on the PC (the tree here is replaced on every unpack): pack, unpack, then `deploy-site.sh --dry-run`. Visual checks of the site need a browser, which this machine does not have: say "not verified: no browser on the VPS" or ask GI before installing Playwright.

## The VPS kit (how this Claude setup got here)

`solar-rp-vps-kit-<UTC stamp>.run` is built on GI's PC by `tools/deploy/pack-vps-kit.sh` (it calls `pack-source.sh` for the source) and run here as root by GI: `bash solar-rp-vps-kit-<stamp>.run --dry-run`, then without `--dry-run`. It checks its own payload SHA-256 (exit 3 on a mismatch, nothing unpacked), unpacks to `/root/solar-rp-kit/<stamp>/` and runs `install.sh` from there:

- `/root/.claude`: GI's `CLAUDE.md` byte for byte plus a marked VPS section, `rules/` (with `00-vps-linux.md`: the Windows/Git Bash rules do not apply here), `skills/`, `agents/`, `commands/`, `tools/`, `hooks/`, `lessons-archive/`, the status line, and `settings.json` MERGED key by key (the VPS value wins, permission rules and hooks are united). The PC's `env` block, Windows-only hooks and every credential were dropped on the PC (`SETTINGS-DROPPED.txt` in the kit lists names only). A file that exists here and differs is first copied to `/root/.claude/.kit-backup-<UTC stamp>/`; nothing is ever deleted.
- The project auto-memory, under `projects/-opt-gage-src/memory` and `projects/-opt-gage/memory` (Claude Code names the folder after the directory it starts in: every non-alphanumeric character becomes `-`). Start sessions in `/opt/gage/src`.
- Plugins (impeccable, claude-mem, frontend-design): `claude plugin marketplace add` and `claude plugin install` are run when `claude` is installed, otherwise printed. `~/.claude.json`, credentials, history and plugin caches are never in the kit; `mcp-servers.txt` in the kit folder lists the MCP servers the PC had.
- The source tree, through `unpack-source.sh` taken from the kit's own tarball (same checks, same `src.prev`).

It never runs `bootstrap-vps.sh`, a deploy, a publish or the game server, never edits `/etc/gage/deploy.env`, and the update signing key is not in it (GI copies that to `/root/.gage-keys/` himself, mode 600).

Refresh: GI builds a new kit on the PC and scps it; the same two commands. Running the same kit twice changes nothing. A refresh replaces the source tree like any unpack, so run it only when GI asks and no build or deploy is running. It also replaces files in the kit-managed folders that were edited here (the old copy is kept in the backup folder), so put VPS-only additions in new files. Options: `--no-plugins`, `--skip-claude`, `--skip-memory`, `--skip-source`, `--force-source` (`install.sh --help`).

## Style of work here

- State the plan in two lines before a step that changes the machine; run the `--dry-run` form first.
- One change at a time; after each, observe the result (exit code, log line, the served file) and say what you observed.
- Keep outputs short: grep the log for the lines you need rather than dumping it, but never cut the stream of a command that just failed.
- Anything that needs GI (restart with players, stopping the tmux server, ufw rules, the signing key, DNS, a decision about what is public): stop and ask one sharp question.
- When a step cannot be verified from here, say so in the report ("not verified: ...") instead of implying it worked.
