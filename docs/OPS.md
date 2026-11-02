# Solar RP: VPS operations runbook

For GI and for the Claude session that runs on the VPS. Everything the VPS does is driven by the scripts in `tools/deploy/`; this file is the order to run them in, and the reasons.

Status: written 2026-11-01 on the PC. **Nothing in this kit has been run on the VPS yet.** Section 13 lists what was checked locally and what only the first real run can prove. Treat every first run as a rehearsal: `--dry-run` first, read it, then run.

## 0. Names, layout, scripts

Player-facing name is **Solar RP**: installer `SolarRP-Setup-<ver>.exe`, launcher `SolarLauncher.exe`, servers **LUNA**, **TERRA**, **SOL** (this VPS runs SOL). Internal names stay GAGE: `gage-server`, `gage_mp.dll`, user `gage`, `/opt/gage`, `gage-server.service`, registry `HKCU\Software\GAGE-MP`.

| Path on the VPS | What |
| --- | --- |
| `/opt/gage/src` | the source tree from the PC (`unpack-source.sh` replaces it; the previous one stays as `src.prev`). Not a git repository. |
| `/opt/gage/build` | persistent build tree; `/opt/gage/src/build` is a symlink to it (`installer/build-installer.sh` writes to `<repo>/build/win/installer`) |
| `/opt/gage/server/{current,releases,data}` and `server.json` | the game server: `current` -> `releases/<UTC stamp>/`; only `data/` is writable by the service |
| `/opt/gage/{wine,inno,logs,tmp,releases}` | wine prefix, Inno Setup copy, build and deploy logs, scratch, archived installers |
| `/var/www/gage/update/` | what Caddy serves: `version.json` (signed) + installers; `/var/www/gage/.staging/` is where manifests are signed |
| `/etc/gage/` | `deploy.env` (settings, no secrets; template `tools/deploy/deploy.env.example`), `server.env` (`GAGE_ADMIN_TOKEN`; and `GAGE_LATEST_CLIENT_VERSION`, set by `release.sh`), `bootstrap-versions.txt` |
| `/root/.gage-keys/` | `update-signing-key.pem` (private, 0600) and `update-signing-pub.txt` |

| Script (in `tools/deploy/`) | Runs on | Purpose |
| --- | --- | --- |
| `pack-source.sh` | PC (Git Bash) | tar the repo for the VPS; prints the scp/ssh commands |
| `unpack-source.sh` | VPS | check, extract and swap in the tarball; installs `/opt/gage/CLAUDE.md` (from `VPS-CLAUDE.md`) |
| `bootstrap-vps.sh` | VPS, once | ufw, packages, wine, Node 22, Caddy, user, directories, unit, Caddyfile |
| `build-client.sh` | VPS | cross-compile the launcher + DLLs, selftests under wine, build the installer |
| `publish-client.sh` | VPS | sign and publish the update manifest |
| `release.sh` | VPS | **client + server together**: build both, publish the client mandatory, in-game countdown, activate the server, verify the live protocol (section "Release (client + server together)") |
| `deploy-server.sh` | VPS | build, test, canary, roll out and roll back `gage-server` |
| `deploy-site.sh` | VPS | build `apps/website` (`npm ci`, tests, build), release to `/var/www/gage/site-releases/<stamp>`, switch the live symlink, HTTP check; `--rollback` |
| `pack-vps-kit.sh` | PC (Git Bash) | ONE self-extracting `solar-rp-vps-kit-<stamp>.run`: the source (via `pack-source.sh`) + GI's Claude setup (skills, rules, agents, memory, sanitised settings, plugin list) + `vps-kit/install.sh`; refuses keys, `.env`, `.pem`, credentials; prints the scp/ssh lines |
| `solar-rp-vps-kit-<stamp>.run` (runs `vps-kit/install.sh`) | VPS, as root | checks its SHA-256, unpacks to `/root/solar-rp-kit/<stamp>/`, installs `/root/.claude` (backups, settings merge), the plugins, and the source via `unpack-source.sh`; `--dry-run` first; never bootstraps or deploys; run it again with a newer kit to refresh |

All scripts take `--dry-run`, are idempotent, read `/etc/gage/deploy.env`, and exit non-zero on failure. Scripts' own `--help` prints their header.

## 1. First-time setup, in this order

**Step 0. Firewall (GI, before anything else).**
Inbound **tcp 22, tcp 80, tcp 443, udp 24800** must be open. The current VPS is not on Hetzner and has no provider firewall in front of it: `ufw` is the firewall, and GI opened these ports (bootstrap also sets the ufw rules). On a host that does put a firewall in front of the VPS, `ufw` cannot see it and nothing in this kit can check it; open the same ports there. Without 80/443 Caddy cannot obtain the Let's Encrypt certificate for `solar-rp.com` and `bootstrap-vps.sh` stops at its step 8 with this hint.

**Step 1. DNS (done by GI).** A records `@` and `www` -> 5.175.253.187. `bootstrap-vps.sh` refuses to continue if either does not resolve to this VPS (`--skip-dns-check` overrides).

**Step 2. Pack and upload (GI, PC, Git Bash).**
```
cd "H:/Users/Administrator/Downloads/gage new/gage-mp-keep/gage-mp-new/gage-mp"
bash tools/deploy/pack-source.sh --dry-run     # file count, size, checks; writes nothing
bash tools/deploy/pack-source.sh               # writes ../solar-rp-src-<stamp>.tar.gz and prints the next commands
```
It prints the exact `cd`, `scp` and `ssh` lines for the next three steps (copy them from its output; the sha256 in them is the tarball's). `pack-source.sh` refuses to pack `*.pem`, `.env*`, `.gage-keys`, any private-key block in a text file, `enhanced EXE/` (GTA game binaries), `build/`, `node_modules`, `logs/`, and every `.toolchain` folder except `innosetup-6.7.3`.

**Step 3. Bootstrap (GI, over ssh, using the lines pack-source printed).**
1. extract into `/opt/gage/src` and run `bootstrap-vps.sh --dry-run`; read it;
2. run `bootstrap-vps.sh` (installs ufw rules, packages incl. i386 + wine, NodeSource Node 22, the Caddy repo, user `gage`, directories, the unit (enabled, not started: no binary yet), the Caddyfile; then waits up to 150 s for `https://solar-rp.com/update/` to answer with a valid certificate);
3. run `unpack-source.sh <tarball> --sha256 <sha>` once more: it installs `/opt/gage/CLAUDE.md`, links `/opt/gage/src/build` and keeps the layout identical to every later update.

**Step 4. Signing key (GI, PC -> VPS).** The private key never goes into the tarball, chat or the repo.
```
cd <the folder that holds update-signing-key.pem and update-signing-pub.txt>
scp ./update-signing-key.pem ./update-signing-pub.txt root@5.175.253.187:/root/.gage-keys/
ssh root@5.175.253.187 'chmod 600 /root/.gage-keys/update-signing-key.pem && chmod 644 /root/.gage-keys/update-signing-pub.txt && stat -c "%a %U %n" /root/.gage-keys/*'
```
Expected: `600 root .../update-signing-key.pem`, `644 root .../update-signing-pub.txt`. (If you have not generated a key yet: `node tools/update-sign/keygen.mjs --out-dir <dir>` on the PC, then keep a second copy of the private key in your password manager; losing it means every installed launcher has to be reinstalled by hand.)

**Step 5. Start the Claude session on the VPS (GI).** Claude Code is not installed by this kit. Install and log in as you normally do, then `cd /opt/gage && claude`. It reads `/opt/gage/CLAUDE.md` (the VPS rules) and, from `/opt/gage/src`, the repository's own CLAUDE.md, which describes the PC workflow; the VPS file says which wins. First instruction to give it: "Read /opt/gage/src/docs/OPS.md sections 1 to 6, then run `build-client.sh --dry-run` and tell me what it reports."

**Step 6. First launcher build** (the Claude session): section 2, with `--tests soft` for the first runs.

**Step 7. First server cutover** (the Claude session, only with GI's yes, because players are on the tmux server): section 3.

## Release (client + server together)

**This is how a normal release goes out.** The client and the game server must always move together: a client built for protocol N cannot play on a server speaking N-1 or N+1, and the server refuses it. `tools/deploy/release.sh` does both and forces the players over:

```
cd /opt/gage/src                                    # kClientVersion in apps/launcher/gage_gui.cpp is the version you give
bash tools/deploy/release.sh <ver> --notes "what changed" --dry-run     # every sub-script's own --dry-run + the plan
bash tools/deploy/release.sh <ver> --notes "what changed"               # real run; --countdown 60 (default), --jobs N
```

It refuses when `<ver>` is not `kClientVersion` (the installer would carry another number), runs under a `release` lock, stops at the first failure, and in order:

1. `build-client.sh` (default test gate);
2. `deploy-server.sh --build-only`: server build, `netcode_test` and `server_test`, canary, a staged release. The staged stamp is read from the line `RELEASE_STAMP=<stamp>` that script prints, then checked against `/opt/gage/server/releases` (and, if the line is ever missing, taken from the one new release directory). **Everything that can fail on a build, a test or a canary comes before the first live change**, so a broken server never leaves players forced onto a client whose server does not exist;
3. `publish-client.sh <ver> --mandatory --notes TEXT`: **live change 1**. Open launchers see the release within 2 minutes (they poll every 2 minutes, and check when brought forward, on PLAY and when the game exits), download it at once and install it as soon as no game has Solar RP loaded;
3b. the **update ping** (not a stop): `release.sh` sets the line `GAGE_LATEST_CLIENT_VERSION=<ver>` in `/etc/gage/server.env` (`set_latest_client_env`; see "The update ping" below). The server reads it when it restarts in step 5 and reports it in its UDP query, so open launchers learn of the release within seconds instead of at the next 2-minute manifest poll. It is written **after** the publish on purpose: a server must never advertise a client that is not published yet. A failure here is a warning, not a stop;
4. the countdown: the players on the server are told through the admin channel (`say`, the mechanism `deploy-server.sh` uses for its own warning: `adminPort` in `server.json` plus `GAGE_ADMIN_TOKEN` in `/etc/gage/server.env`) at the start, at 30 s and at 10 s (the marks that fit `--countdown`), and the script waits (wall clock). No players online: no countdown. Without an admin channel it warns that players get no in-game notice and still waits;
5. `deploy-server.sh --activate <stamp> --allow-players --no-announce`: **live change 2**, the restart with the usual health check and automatic rollback (the countdown was the warning, so `--allow-players` is deliberate; run `release.sh` only with GI's yes);
6. `deploy-server.sh --status`, then the UDP query must report the protocol compiled from `libs/protocol/protocol.h`; the last line of a good run is `RELEASE_RESULT=ok version=<v> stamp=<stamp> protocol=<n>`.

Logs: `/opt/gage/logs/release/<UTC stamp>-<step>.log`, whole. Exit codes: 0 ok, 1 failure (the message says what is live and what is not), 2 usage, 4 the server release failed its health check and was rolled back (**the client IS published**), 5 rollback failed too (stop, tell GI), 6 something else holds the game port, 75 busy. After a failure at step 5 or 6 the script prints the exact `deploy-server.sh --activate <stamp> --allow-players` to run once the cause is fixed; until then new launchers say "This server has not been updated yet" and refuse PLAY.

What the players see: a launcher that has not updated yet and talks to the new server is refused by the launcher itself before the game starts ("Solar RP was updated. Updating the launcher...", and it starts the update check at once); a game that does start and gets `Refuse(VersionMismatch)` shows the server's sentence on the loading screen and in chat ("Solar RP was updated. Close the game; Solar Launcher installs the update."). A new launcher against a server not yet restarted is told "This server has not been updated yet. Try again in a minute." (the window between step 3 and step 5). Details: `docs/UPDATES.md`, "Lockstep releases".

### The update ping (`GAGE_LATEST_CLIENT_VERSION`)

What it is: UDP query format v2 (`docs/api/protocol.md`, "The UDP query") lets a launcher ask the game server, in the query it already sends every 5 s, which client version the server was released with. A launcher older than that refuses PLAY (code 14) and checks for the update at once; before this, only a *protocol* change was noticed that way and a same-protocol release (0.4.3) waited for the 2-minute manifest poll (`docs/UPDATES.md`, "Lockstep releases"). Launchers that predate it (0.4.2 / 0.4.3) are unaffected: they send the v1 query and get the exact v1 reply.

How the server gets the version, and why it is not a `server.json` key: the server reads `latestClientVersion` from `server.json` if the file has it, else the environment variable `GAGE_LATEST_CLIENT_VERSION`. `release.sh` uses the environment: `gage-server.service` loads `/etc/gage/server.env`, and the script changes exactly one line there (replaces `GAGE_LATEST_CLIENT_VERSION=...` in place, or appends it after a missing final newline; every other line, including the admin token, stays byte for byte; the file is rewritten in place so owner and mode stay; nothing from the file is printed). The reasons: (1) `server.json` is one file shared by every release and the old server binary refuses a key it does not know, so writing a key there before the first activation would make the automatic rollback (`deploy-server.sh`, exit 4/5) fail; an old binary ignores an environment variable it does not read; (2) the unit's `ExecStart` is fixed, so there is no per-release command line; (3) the operator's `server.json` is never rewritten. The key is deliberately absent from `apps/server/server.json.example`. If `server.json` does hold a `latestClientVersion`, it wins over the environment: `release.sh` warns (and `--dry-run` notes it) so you remove it.

By hand: `grep -c GAGE_LATEST_CLIENT_VERSION /etc/gage/server.env` (never `cat` the file: it holds the admin token); to switch the ping off, delete that line and restart the server (`unknown`, `0.0.0`, never blocks anyone). The server logs `update ping: the UDP query tells launchers the latest client is x.y.z` (or `no latest client configured`) at start, and the same query shows it: a v2 reply carries it in bytes 20..22. Never set it to a version that is not published: every launcher older than it would be refused PLAY and find no update (docs/UPDATES.md, "Known limits").

Tested without a VPS: the shell function was run against temporary copies of an env file (key absent, key present, no final newline, missing and empty file, malformed versions) and the other lines compared byte for byte; the codec, the server's replies and the launcher's decisions are unit-tested. Not run: a whole `release.sh` on the VPS.

The 2-minute figures are for launchers that carry this feature (the release after 0.4.2). A 0.4.2 launcher in the field polls every 10 minutes and has no protocol gate, no activation / PLAY / game-exit check and does not treat a non-mandatory update as mandatory, which is why `release.sh` always publishes `--mandatory`: for the first lockstep release the slowest players are the ones still on 0.4.2 and older, who hit the server's refusal ("Refuse VersionMismatch", shown only in their log) until their launcher's next check or restart installs the update.

Not tried on the VPS yet (like everything in this kit): the orchestration was run locally against stub scripts and a fake game server speaking the real UDP query and admin-channel protocols (80 checks; the three timed scenarios included), not against `systemctl`, the real builds or real players. First real use: `--dry-run`, then a release with `--countdown 0` while nobody is on.

The single-purpose scripts below stay available for a client-only or server-only emergency; a client-only publish without the matching server breaks the lockstep (new launchers refuse PLAY until the server catches up).

## 2. Release a client update

Prerequisites: the source on the VPS is the tree you want to ship (`unpack-source.sh` from a fresh `pack-source.sh`), and `kClientVersion` in `apps/launcher/gage_gui.cpp` is the new version. The launcher accepts only a strictly newer version than the one it runs.

```
cd /opt/gage/src
bash tools/deploy/build-client.sh --dry-run            # reads preflight problems before anything is built
bash tools/deploy/build-client.sh --tests soft         # first runs; later: no flag (= gate)
bash tools/deploy/publish-client.sh <ver> --dry-run
bash tools/deploy/publish-client.sh <ver> --notes "what changed, one or two sentences"
```

What `build-client.sh` does (8 steps, logs in `/opt/gage/logs/<UTC stamp>-<step>.log`, the first error lines are echoed on failure):

1. preflight: the markers the build needs are still in `CMakeLists.txt` (`launcher-ui/dist`, `crypt32`, `launcher-assets`, `GAGE_TEST_SERVER`, `GAGE_API_BASE`, `GAGE_UPDATE_URL`, `GAGE_UPDATE_PUBKEY`; an older copy once rolled them back silently), no CRLF in the scripts, Node >= 22.12, the `-posix` mingw compiler, the Inno Setup source, the signing public key (88 characters);
2. wine prefix (`/opt/gage/wine`), Inno Setup copied from `.toolchain/innosetup-6.7.3` to `/opt/gage/inno/6.7.3`, `ISCC.exe` smoke test;
3. launcher UI: `npm ci` + `npm run build` in `apps/launcher-ui`;
4. configure with `cmake/toolchain-mingw-linux.cmake`, `-DGAGE_UPDATE_URL=https://solar-rp.com/update/version.json`, `-DGAGE_UPDATE_PUBKEY=<the VPS key>`; the cache is read back; a build dir from another toolchain is refused unless `--clean`;
5. compile everything;
6. prove it: the embedded UI length equals `dist/index.html`; `SolarLauncher.exe` exists; the baked URL and public key are found inside the exe; **PE import allow-list** on the launcher, `gageinjector.dll`, `gage_loader.dll`, `gage_mp.dll`, `gage_launch_shim.dll` (no `libstdc++-6.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`, no `ucrtbase`: GTA must not need a new DLL); `ui/` and `launcher-assets/` present;
7. selftests under wine, **by name** from `tools/deploy/wine-selftests.list` (never a glob: `selftest/` contains `gage_v.exe`, which launches GTA and rewrites the registry handoff). 22 entries: 8 `gate` (pure logic, must pass; `version_gate_test` is the launcher's lockstep policy), 11 `soft` (touch Windows facilities wine only approximates; reported, not blocking), 1 `driver` (`update_check_test` through its node driver), 2 `skip` (`gage_v`, `brand_apparel_test` needs D3D11 WARP). An unlisted test, a stale entry or a wrong count fails the build. `--tests strict` makes soft tests blocking; `--tests off` skips the step. After a few runs, promote the soft tests that pass reliably under wine to `gate` in the list (the list is data, edit it on the PC and repack);
8. installer: `installer/build-installer.sh` unchanged, with `cygpath` and `iscc` shims (`tools/deploy/shim/`) that route to `winepath` and `wine ISCC.exe`. Output: `/opt/gage/build/win/installer/SolarRP-Setup-<ver>.exe` (checked: MZ header, >= 1 MB).

**Before publishing the first cross-built release, test it on a real Windows machine** (copy the installer from the VPS with scp; install; start the launcher; launch GTA with the injected chain). The VPS can prove the build is internally consistent; it cannot prove the Ubuntu-built binaries behave like the WinLibs-built ones in GTA (section 9).

What `publish-client.sh` does, in order (a failure at any step leaves the previous `version.json` serving): refuses unless `<ver>` equals `kClientVersion` and the installer exists, and the build was made with the same public key the VPS signs with; copies the installer to `/var/www/gage/update/` (atomic rename); signs a manifest into `/var/www/gage/.staging/`; verifies it with `tools/update-sign/verify.mjs` against the public key AND the installer's bytes; atomically moves it to `version.json`; fetches both over `http://127.0.0.1` and `https://solar-rp.com` and compares bytes; keeps the newest 3 installers and archives older ones to `/opt/gage/releases/installers`. It never prints the key. Check from outside afterwards:
```
curl -fsS https://solar-rp.com/update/version.json
curl -fsSI https://solar-rp.com/update/SolarRP-Setup-<ver>.exe     # 200 and a Content-Length equal to the installer's size
```
`--mandatory` is what `release.sh` always passes. In every launcher that carries `apps/launcher/version_gate.h` (the release after 0.4.2) every available update is mandatory anyway (the signed flag no longer decides anything for a new launcher; older launchers still read it). It is refused together with `--rollback-from`. A client published alone, without the matching server release, makes new launchers refuse PLAY ("This server has not been updated yet") until the server catches up: publish through `release.sh`.

## 3. Update the game server

Normal update (the tree on the VPS has the new server code):
```
bash tools/deploy/deploy-server.sh --dry-run
bash tools/deploy/deploy-server.sh                   # build, test, canary, install, then activate
bash tools/deploy/deploy-server.sh --status
```
Flow: native Linux build of `apps/server` into `/opt/gage/build/server-<stamp>` -> `netcode_test` and `server_test` run by name -> canary (the new binary starts with the production `server.json`, admin channel, master announce and bans switched off, on a spare port, and answers a UDP query) -> installed to `/opt/gage/server/releases/<stamp>` -> activation gate -> `say` warning through the admin channel -> atomic `current` swap -> `systemctl restart gage-server` -> health check (unit active AND UDP query answers). A failed health check rolls back by itself.

Exit codes: 0 ok; 1 failure; 2 usage; **3 players are online** (nothing live was touched; do not add `--allow-players` without GI's yes); 4 new release failed its health check and was rolled back; 5 failed and the rollback failed too (stop, tell GI); **6 something else already answers on the game port** (the by-hand tmux server); 75 another deploy is running. `--build-only` stages a release without activating it (`--activate <stamp>` later). The in-game warning needs `"adminPort": 24900` in `server.json` and `GAGE_ADMIN_TOKEN` in `/etc/gage/server.env` (bootstrap generated the token; never print it); without them the script says so and restarts without a warning.

### First cutover from the tmux server

The game server currently runs by hand in tmux (as root, from `~/gage-v3`). The unit and that process cannot share UDP 24800, so the first deploy is a short, planned downtime. With GI's explicit yes, in this order:

1. `bash tools/deploy/deploy-server.sh --build-only --jobs 1` while the tmux server keeps running (builds, tests and canaries on a spare port; nothing live is touched; `--jobs 1` keeps two cores free for the running game). It seeds `/opt/gage/server/server.json` from `apps/server/server.json.example` with `"name": "SOL"` (`SERVER_NAME` in `deploy.env` changes that) and `"bansFile": "data/bans.json"`.
2. Move what the old server has: attach to its tmux session (`tmux ls`, `tmux attach -t <name>`; read `pwd` there to find its folder). Compare its `server.json` with the seeded one and carry over every setting that is deliberate (maxPlayers, spawnPoints, `gamemode`, masterUrl, ...); keep `"name": "SOL"`, `"bansFile": "data/bans.json"`, and add `"adminPort": 24900` for the in-game warning. Copy its `bans.json` to `/opt/gage/server/data/bans.json` (`chown gage:gage`, mode 0640) and its gamemode folder `resources/` to `/opt/gage/server/resources/` (root-owned, read-only to the service; scripts that write use `data/<resource>/`; see `docs/api/gamemode.md`).
3. Stop the old server cleanly from inside its tmux session with the console command `quit` (it flushes the ban list and sends clients a shutdown disconnect). Do not kill it by PID or by a search; if it will not stop, tell GI.
4. `bash tools/deploy/deploy-server.sh --activate <stamp from step 1>`; then `--status` and `journalctl -u gage-server -n 50`.
5. Tell GI the result and the UDP query line the script printed.

`server.json` is `root:gage` mode 0640 and the unit's `ProtectSystem=strict` makes everything but `/opt/gage/server/data` read-only: a bans file anywhere else is refused by the script before it can crash-loop the service. A config edit is `edit server.json` then `systemctl restart gage-server` (with players online: ask GI first).

## 4. The domain and HTTPS

HTTPS is live from the first bootstrap: `solar-rp.com` and `www.solar-rp.com` (permanent redirect to the apex) get automatic Let's Encrypt certificates from Caddy; plain HTTP on the bare IP `http://5.175.253.187/update/` keeps serving the same files as a fallback and is what the publish script checks through (`http://127.0.0.1`). Launcher 0.4.0 bakes `https://solar-rp.com/update/version.json`.

Check the domain first, from anywhere, whenever something looks wrong:
```
curl -fsS https://solar-rp.com/update/version.json
```
Expected: the signed manifest (JSON with `version`, `url`, `notes`, `signed` and `sig`; 0.4.0+ launchers trust only `signed`). Diagnose in this order: (1) inbound tcp 80/443 reachable from outside (ufw, and any provider firewall); (2) `dig +short solar-rp.com www.solar-rp.com` both answer 5.175.253.187; (3) `ufw status` shows 80/443 allowed; (4) `journalctl -u caddy -n 100` (ACME errors name the cause); (5) `caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile`. Let's Encrypt has rate limits; do not loop restarts when issuance fails, fix the cause first.

The signed `updateUrl` (`publish-client.sh --update-url <url>`) is **optional** and normally unused: 0.3.x launchers have no updater at all, and 0.4.0 already bakes the https URL. It is only for a straggler build that baked the bare-IP URL and has to be moved to the domain: publish once with `--update-url https://solar-rp.com/update/version.json`, and keep the IP fallback serving.

Another host name (or IP-only operation): set `DOMAIN=` in `/etc/gage/deploy.env` for the scripts and `Environment=DOMAIN=<name>` through `systemctl edit caddy` for Caddy (an EMPTY `DOMAIN` breaks the Caddyfile, because the value is substituted before parsing; for IP-only delete the two `{$DOMAIN}` blocks instead). The master server list (`apps/master`, Node on 127.0.0.1:8080) has a commented `/servers` proxy block in the Caddyfile, ready to enable once it runs here.

## 5. Key handling

- The private key lives only in `/root/.gage-keys/update-signing-key.pem` (0600, root) on the VPS and in GI's own backup. It is in no repo, no tarball (pack-source refuses it), no log, no chat. The Claude session never reads or prints it; it checks existence and mode with `stat -c %a`.
- The public key is `update-signing-pub.txt` (88 characters, base64 of the raw 64-byte X||Y point). `build-client.sh` bakes it into every launcher (`-DGAGE_UPDATE_PUBKEY`), `publish-client.sh` refuses to sign a release whose installer was built with another key, and refuses to replace a manifest signed by another key (`--force-key-mismatch` exists for a deliberate key change; there is no such thing as a safe one: launchers in the field trust exactly one key).
- Leak: assume anyone can push code to every player. Generate a new pair, build and distribute a new launcher by hand (installer download from the website), publish nothing signed with the old key.
- Loss: the same recovery. This is why GI keeps a second copy.

## 6. Rollback

**Server:** `bash tools/deploy/deploy-server.sh --rollback` (same as `--activate previous`): swaps `current` back, restarts, health-checks. The last releases stay in `/opt/gage/server/releases`.

**Launcher:** a launcher accepts only a manifest whose version is strictly newer than the one it runs (anti-downgrade: otherwise a stolen manifest or a mistaken publish could force old, vulnerable code onto everyone). So a bad release `0.4.1` is not undone by publishing `0.4.0` again; the old installer goes out under a **new, higher number**:
```
bash tools/deploy/publish-client.sh 0.4.2 --rollback-from 0.4.0 --notes "Rollback of 0.4.1" --dry-run
bash tools/deploy/publish-client.sh 0.4.2 --rollback-from 0.4.0 --notes "Rollback of 0.4.1"
```
`--rollback-from` takes a version (looked up in the web root and in `/opt/gage/releases/installers`, both name families) or a file. Caveat to tell GI: the installer carries the old launcher, whose own `kClientVersion` is still `0.4.0`, so after installing it that launcher sees manifest `0.4.2` and offers an update again. Players who accept get the same old bits again (a harmless loop unless the update is mandatory, which is why `--mandatory` with `--rollback-from` is refused). The real fix is a new release `0.4.3` built from corrected source; the rollback only stops the bleeding for people who have not updated yet and gives updated players the way back.

## 7. Logs and state

| What | Where |
| --- | --- |
| build and test logs | `/opt/gage/logs/<UTC stamp>-<step>.log` (ui-build, configure, build, tests, installer, pe-imports) |
| game server | `journalctl -u gage-server -f`; release metadata `/opt/gage/server/releases/<stamp>/BUILD-INFO` |
| Caddy | `journalctl -u caddy`; access log to the journal (`log` in the Caddyfile) |
| what is published | `/var/www/gage/update/` and `curl https://solar-rp.com/update/version.json` |
| bootstrap versions | `/etc/gage/bootstrap-versions.txt` (compiler, wine, node, caddy as installed) |
| unpack history | `/opt/gage/src.info` (UTC time, sha256, tarball name, kClientVersion per line) |
| locks | `${TMPDIR:-/tmp}/gage-{build,publish,deploy-server}.lock` (a stale one is removed automatically when its pid is gone) |

## 8. Pending CMake changes

**None are required by the kit.** The root `CMakeLists.txt` was read in full (and `cmake/gage_overlay.cmake`, `cmake/gage_server.cmake`, `tools/embed_ui.cmake`, `tools/embed_bytes.cmake`, `tools/integrity.cmake`). It already carries everything `build-client.sh` greps for (the Solar RP `OUTPUT_NAME SolarLauncher`, `GAGE_UPDATE_URL` / `GAGE_UPDATE_PUBKEY` with validation, `crypt32`, `launcher-assets` copy, `GAGE_API_BASE`, `GAGE_TEST_SERVER`), and the installer scripts already say `SolarLauncher.exe` / `SolarRP-Setup-<ver>.exe` (checked 2026-11-01). If any of those regress, `build-client.sh` stops in preflight and names the missing marker.

Every Windows-host assumption looked for, and what a Linux cross build does with it:

| Construct | Host-specific? | Result |
| --- | --- | --- |
| `project(GAGE_MP C CXX)`; compiler choice | yes, picks the host compiler | solved by `cmake/toolchain-mingw-linux.cmake` (`x86_64-w64-mingw32-gcc/g++-posix`, `windres`, `CMAKE_SYSTEM_NAME Windows`); a configure without it would build for Linux and fail on the first `#include <windows.h>` |
| `if(WIN32) add_link_options(-static -static-libgcc -static-libstdc++)` | `WIN32` must be true | true under the toolchain file (system name Windows). This is what keeps `libstdc++-6.dll` and friends out of the imports; the PE gate proves it |
| `embed_ui.cmake`, `embed_bytes.cmake`, `integrity.cmake` run with `cmake -P` | no | pure CMake (`file(READ ... HEX)`, `file(SHA256)`, regex); same on both hosts. The three chain-DLL hashes baked into the launcher are those of the cross-built DLLs, so they differ from a WinLibs build's: ship the exe and its three DLLs from one build, never mixed |
| `add_library(gage_gui_res OBJECT apps/launcher/gage_gui.rc)` | needs `windres` | `CMAKE_RC_COMPILER` is set by the toolchain file. The `.rc` says `1 ICON "res/gage-mp.ico"`, a path relative to the .rc; windres resolves it the same way on both hosts, but this is untested on Ubuntu until the first VPS build. The comment about spaces in the source path does not apply to `/opt/gage/src` |
| `-municode`, `-Wl,--subsystem,windows` | mingw-gcc flags | identical |
| POST_BUILD `cmake -E rm -rf` and `copy_directory` for `launcher-assets` | needs CMake >= 3.17 for `-E rm` | Ubuntu 24.04 ships CMake 3.28 (`cmake_minimum_required(VERSION 3.16)` is a pre-existing understatement, harmless) |
| `file(COPY apps/ingame-ui/ DESTINATION ...)` at configure time | no | relative to the source dir |
| `-l` libraries (advapi32 bcrypt comctl32 comdlg32 crypt32 d3d11 d3d12 d3dcompiler dwmapi dxgi dxguid gdi32 imm32 kernel32 ole32 oleaut32 psapi shell32 shlwapi user32 uuid version windowscodecs winhttp winmm ws2_32) | needs the import libraries | all 28 names checked present as `lib<name>.a` in the WinLibs mingw-w64 11.0.1 tree; Ubuntu's `mingw-w64-x86-64-dev` is the same mingw-w64 version (11.0.1), its package file list was not diffed against these 28 |
| File-name case (NTFS ignores it, ext4 does not) | **yes** | checked: 940 `#include`s in apps/, libs/, tests/, tools/ (305 quoted, 635 angle) and 100 repo paths named in the CMake files all match the real on-disk case (a wrong-case control was rejected by the same check); the six brand assets of `gage_overlay.cmake` match too |
| Paths with a space (`gage new`) | yes, on the PC | `/opt/gage/src` has none |
| `installer/build-installer.sh` calling `cygpath`, `ISCC.exe` | yes | `tools/deploy/shim/cygpath` (via `winepath`) and `shim/iscc` (via `wine`), no change to the script |

Optional, not needed for the kit, listed so nothing is hidden: `cmake_minimum_required(VERSION 3.17)` would state the real floor (`-E rm`); that is a one-line change in the root file, to be made by whoever owns it.

## 9. ABI differences: Ubuntu mingw-w64 vs the WinLibs build

| | WinLibs (PC, `.toolchain/winlibs-gcc13.3`) | Ubuntu 24.04 (`g++-mingw-w64-x86-64-posix`) |
| --- | --- | --- |
| gcc | 13.3.0 (measured: `g++ -v`) | 13.2.0-6ubuntu1+26.1 (package page for noble) |
| mingw-w64 | 11.0.1 (measured: `_mingw_mac.h`) | 11.0.1 (package page) |
| C runtime | msvcrt (`x86_64-msvcrt-posix-seh`) | expected msvcrt (Debian/Ubuntu packaging; not observed on a real Ubuntu host yet). The toolchain file checks the compiler's default at configure time and refuses a UCRT default unless `GAGE_ALLOW_NON_MSVCRT` is set; the PE gate rejects `ucrtbase`/`api-ms-win-crt-*` imports either way |
| threads / unwinding | posix / SEH | posix (the `-posix` variant is selected explicitly) / SEH |
| libstdc++, libgcc, winpthread | linked statically (`-static -static-libgcc -static-libstdc++`) | the same flags, same root CMake |

So the only difference is the gcc patch level (13.2 vs 13.3), with the same C runtime and ABI. Baseline measured on the current WinLibs build (`tools/deploy/lib/pe-imports.mjs`, 2026-11-01): every PE is `machine=x64`, `DllCharacteristics=0x160` (ASLR, high-entropy VA, DEP); imports are only system DLLs (`SolarLauncher.exe`: ADVAPI32 bcrypt comdlg32 CRYPT32 GDI32 KERNEL32 msvcrt ole32 SHELL32 SHLWAPI USER32 WINHTTP WS2_32; `gage_mp.dll` adds d3d12, D3DCOMPILER_47, dwmapi, WINMM). `tools/deploy/pe-allow.txt` encodes this plus a few stock Windows 10 DLLs; the build fails on anything else. After the first cross build compare `/opt/gage/logs/<stamp>-pe-imports.txt` with the numbers above: `dllchars` should read 0x160 again. If it does not, the linker flags differ (Ubuntu's binutils defaults) and ASLR/DEP would be silently off; report it to GI instead of shipping.

## 10. Server names and more than one server

- This VPS runs **SOL**: `"name": "SOL"`, `"port": 24800`, `"adminPort": 24900` in `/opt/gage/server/server.json`. The launcher's list shows `name`.
- **LUNA** and **TERRA** are not built here; this is how they would be added (documented only). A second instance needs its own UDP port (a ufw rule, plus a provider firewall rule if the host has one), its own config and its own writable `data/`. With the unit as it is, the simplest shape is a templated unit `gage-server@.service`: a copy of `gage-server.service` with `WorkingDirectory=/opt/gage/instances/%i`, `ExecStart=/opt/gage/server/current/gage-server --config /opt/gage/instances/%i/server.json`, `ReadWritePaths=/opt/gage/instances/%i/data`, and per instance `server.json` with its own `name` (`LUNA`), `port` (e.g. 24801), `adminPort` (e.g. 24901) and `"bansFile": "data/bans.json"`. `deploy-server.sh` knows exactly one unit (`gage-server`) and one directory; to roll the binary out to instances it needs a loop over `systemctl restart gage-server@<name>` plus a health check per port. That extension is not written; do not improvise it on a live server.
- The separate repository folder `deploy/` (top level, not `tools/deploy/`) is an older, larger plan for the whole backend (PostgreSQL, master list, platform API, numbered instances `gage-server@1`, `@test`). It is not what runs on this VPS. If that backend is ever deployed here, reconcile the two instance schemes first and decide which kit owns Caddy and the unit; the two kits must not both install a Caddyfile.
- Gamemode scripts: the server loads `resources/` from its working directory (`/opt/gage/server/resources/`) and persists to `data/`.

## 11. Recommendation (needs GI's OK): `git init` the source on the VPS

The tree on the VPS is a plain copy of the PC, replaced on every unpack. Making `/opt/gage/src` a git repository would let the Claude session see what changed between two tarballs. It also conflicts with the replace-the-whole-tree design (`unpack-source.sh` would delete `.git`), so the cheaper alternative is already in place: `/opt/gage/src.prev` and `diff -rq /opt/gage/src.prev /opt/gage/src`. The real fix is git on the PC. Nothing in this kit runs `git init`; do it only if GI says so.

## 12. Troubleshooting

- **Inno Setup does not run / "ISCC.exe smoke test failed"**: ISCC.exe and its DLLs are 32-bit; wine needs the 32-bit part (`wine32:i386`) and `dpkg --add-architecture i386`. `dpkg --print-foreign-architectures` must list `i386`; `dpkg -l wine32:i386` must show `ii`. Re-run `bootstrap-vps.sh`. The first run of wine can take a minute (prefix creation) and prints `fixme:` lines; those are not errors (`WINEDEBUG=-all` hides them in the scripts).
- **`apt update` fails after enabling i386** ("Failed to fetch ... binary-i386"): the host's apt mirror may not carry i386 index files. `bootstrap-vps.sh` restricts the existing Ubuntu sources to `amd64` and adds a separate i386 source from the Ubuntu archive (`/etc/apt/sources.list.d/gage-i386.sources`); if that still fails, send GI the exact `apt update` output.
- **HTTPS self-check fails at the end of bootstrap**: section 4 diagnosis list, starting with whether tcp 80/443 are reachable from outside. The script prints it too.
- **`build-client.sh` stops in preflight**: it names the marker or tool; the most common cause is a tarball packed before a fix. Re-pack, unpack, retry.
- **`build-client.sh` step 4 refuses the build dir**: another toolchain or compiler configured it. `--clean` removes `/opt/gage/build/win` and reconfigures.
- **A `soft` selftest fails under wine**: read its log; if it is wine (registry, GUI, D3D), leave it `soft` and write the reason in the list; if it is a real regression, fix on the PC. Do not mark a failing test `skip` to make the build green.
- **`deploy-server.sh` exit 6**: something else answers on the game port: the tmux server. Section 3.
- **`unpack-source.sh` says "already the current tree"**: the same sha256 is already installed; `--force` unpacks it again.

## 13. What has been verified, and what only the VPS can prove

Verified on the PC (Windows, Git Bash, no Linux available): `bash -n` on every script; each script's `--dry-run`; scratch harnesses with fake `cmake`/`wine`/ISCC for the orchestration of `build-client.sh` (66 checks), `publish-client.sh` (55, including a real signature round trip with `tools/update-sign`), `deploy-server.sh` (18, including a real `gage-server.exe` as the "stranger on the port"), the server-name seeding (12); the PE import gate against the real PE files in `build/win/bin` (and a negative control); the toolchain file with a real CMake configure against WinLibs plus a negative control; the include and CMake-path case scans; `pack-source.sh` (the real 997-file / 55 MiB tarball listed and asserted: none of node_modules, build, logs, game files, other toolchains, keys or .env files in it, while each of those exists on disk, so the zero counts are not vacuous) and `unpack-source.sh` (53 checks on the real tarball and on synthetic and altered ones: checksum, dry run, first install, idempotence, `--force`, one previous copy kept, stale staging, and refusals for a `.pem`, an `.env`, `node_modules`, game files, too few entries, a `..` path, entries without `./`, a symlink, a non-tar, missing kit files, CRLF and a syntax error, each leaving the installed tree untouched). Docs fetched while writing: Caddy (Caddyfile concepts, matchers, `file_server`, `header`, install, running), systemd (`systemd.exec`, `systemd.service`, `systemd.unit`), NodeSource (distributions README and manual repository install), Ubuntu noble package pages (`wine`, `wine64`, `wine32:i386`, the `-posix` mingw packages, `mingw-w64-x86-64-dev`) and the wine man page. From memory: ufw behaviour, Let's Encrypt rate limits, provider-firewall semantics, Inno Setup behaviour under wine.

Only the first real run can prove: the mingw cross build itself (compiles, links, windres, the 28 import libraries on Ubuntu), `ISCC.exe` under wine32, the selftests under wine (the `gate`/`soft` classification is a reasoned guess), `systemd-analyze verify`/`security` on the unit, the Caddyfile through the `caddy` binary (a download-and-run was refused by the permission system on the PC, so it was validated against the docs only), ufw and the apt repositories (NodeSource, Caddy, the i386 fallback), real ACME issuance, the seeding and activation branches of `deploy-server.sh` on a real host, and that cross-built binaries behave in GTA like the WinLibs ones.
