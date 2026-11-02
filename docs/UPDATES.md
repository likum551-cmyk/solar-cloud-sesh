# Launcher updates (signed, silent)

SolarLauncher.exe (GAGE-MP.exe up to 0.3.x) finds new versions by itself, downloads the setup, proves it is the one the publisher signed, runs it silently, and starts the new version. Nobody hands out an .exe.

Code: `apps/launcher/updater.{h,cpp}` (checks, crypto, download, install), `apps/launcher/gage_gui.cpp` (the state machine and the SPA messages), `installer/gage-mp.iss` (the relaunch), `tools/update-sign/` (publisher side). The SPA-facing messages are in `apps/launcher/BRIDGE.md` (`update`, `launchResult` code 14).

## Trust model

The only thing the launcher believes is a signature. Everything else (HTTP, DNS, the web server, the CDN, the download host) is untrusted transport.

1. The public key is baked into SolarLauncher.exe at configure time (`-DGAGE_UPDATE_PUBKEY=<base64 of the raw 64-byte X||Y point>`, default in `CMakeLists.txt`). The private key signs manifests and nothing else.
2. A manifest is accepted only if its ECDSA P-256 / SHA-256 signature verifies over the exact decoded bytes of its `signed` member. Only after that is the payload parsed. The unsigned top-level `version`, `url` and `notes` fields are never read by the launcher; they exist for humans and for older launchers that only show "update available".
3. The payload names the installer URL, its SHA-256 and its size. The launcher downloads at most that many bytes (the signed `size` is a hard cap, 512 MiB maximum) and refuses the file unless size and SHA-256 equal the signed values.
4. The hash is computed on a handle opened with `FILE_SHARE_READ` only, and that handle stays open while setup starts. Until setup has loaded, no other process can write to, truncate, rename or delete the file, so what was verified is what runs. (Verified by experiment in `tests/update_check_test.cpp`: the write, delete and rename attempts are refused while the handle is open.)
5. The version must be a strict `x.y.z` and strictly newer than the running launcher. Equal, older and malformed versions are refused, so a replayed old manifest cannot downgrade anyone.

### What an attacker can and cannot do

| Attacker | Can | Cannot |
|---|---|---|
| Network (the baked URL is HTTPS since the rename; launchers built before it still fetch the bare-IP URL over plain HTTP), no private key | Withhold updates (drop, delay or block the manifest or the download); serve garbage that is then refused and logged | Install anything, move the manifest URL, change the version, swap the installer for a different file, make the launcher downgrade |
| Replays an old signed manifest | Nothing: the version is not newer, so nothing happens. Its `updateUrl` is also ignored (see URL migration) | |
| Owns the download host but not the key | Serve a different file | Pass the SHA-256 / size check |
| Has the private key | Push arbitrary code to every launcher that has the matching public key | |
| Local user who can write the install folder | The same as running code as that user already allows | Replace the installer between the check and its start (the handle is held) |

So the private key is the whole security of the system. Losing it means a launcher with a new baked key must be distributed by hand once. Leaking it means anyone can run code on every player's PC.

## Manifest format

Written only by `tools/update-sign/sign.mjs` (read its header and the payload in the script: that is the format; this page does not repeat field lists so they cannot drift). In short, `version.json` is:

```json
{
  "version": "0.4.0",          // unsigned, display only
  "url": "https://solar-rp.com/update/SolarRP-Setup-0.4.0.exe",  // unsigned, display only
  "notes": "...",              // unsigned, display only
  "signed": "<base64 of the payload JSON bytes>",
  "sig": "<base64 of the 64-byte r||s signature over those bytes>"
}
```

The payload is `{v:1, version, url, sha256, size, notes, mandatory, updateUrl, issued}`. The launcher refuses `v != 1`, a malformed version, a non-http(s) or over-long `url` (2048 characters), a `sha256` that is not 64 hex characters, a `size` outside 1..512 MiB, `notes` over 2000 characters and a non-boolean `mandatory`. An invalid `updateUrl` is ignored with a log line; the update itself still proceeds.

Check a signed file the way the launcher does with `tools/update-sign/verify.mjs --pub <key> --manifest <version.json> [--installer <setup.exe>]`.

## Publishing a release

Normally through `tools/deploy/release.sh` (client and server together, docs/OPS.md); the manual steps below are what `publish-client.sh` does.

1. Bump `kClientVersion` in `apps/launcher/gage_gui.cpp`, rebuild everything (the exe bakes the SHA-256 of the three chain DLLs).
2. `BIN="$PWD/build/win/bin" bash installer/build-installer.sh` writes `build/win/installer/SolarRP-Setup-<version>.exe`.
3. Sign it (the key path is wherever the private key lives; never inside the repo):
   `node tools/update-sign/sign.mjs --key <update-signing-key.pem> --installer build/win/installer/SolarRP-Setup-<v>.exe --version <v> --url https://solar-rp.com/update/SolarRP-Setup-<v>.exe --out version.json [--notes "..."] [--mandatory] [--update-url <new manifest URL>]`
4. Upload the installer first, then the manifest. The order matters: a manifest that names a missing file makes every launcher's download fail. `sign.mjs` writes the manifest by temp file and rename; upload it the same way.
5. `node tools/update-sign/verify.mjs --pub <update-signing-pub.txt> --manifest version.json --installer <setup.exe>`, then fetch both URLs from outside and compare. `tools/deploy/publish-client.sh` is the scripted version of steps 3 to 5 on the VPS.

Layout on the VPS (Caddy serving `/var/www/gage`): `/update/version.json` and `/update/SolarRP-Setup-<v>.exe` (older releases keep their `GAGE-MP-Setup-<v>.exe` names). `https://solar-rp.com/update/...` is the address new launchers use; the same files are still served over plain HTTP at `http://5.175.253.187/update/...` for launchers that baked that URL (legacy / straggler path, see "Launchers that still bake the IP" below). Serve the manifest with `Cache-Control: no-store` so a check never sees a stale file (a stale file is harmless, the version is just not newer yet).

`tools/update-server/serve.mjs` serves the same layout on loopback for tests and rehearsals (`/update/...` aliases included); see its README.

## Key handling

- Generate once: `node tools/update-sign/keygen.mjs --out-dir <dir>`. It writes `update-signing-key.pem` (PKCS#8, private) and `update-signing-pub.txt` (the base64 public key to bake).
- The private key never goes into the repo, a backup tarball, a chat, a CI log or an agent's context. Backups and scratch copies exclude it (`tar --exclude='*.pem'`).
- On the VPS keep it in a directory only the publishing user can read, file mode 600 (`chmod 600 update-signing-key.pem`, directory 700). Keep one offline copy somewhere that is not the VPS.
- The public key is not secret. Check what a build contains: gage.log prints `update: ... signing key fingerprint <12 hex>` (SHA-256 of the baked base64 text) at start.
- Tests use throwaway keys from `keygen.mjs --out-dir %TEMP%\upd\k` only.
- Rotating the key means shipping a new exe with `-DGAGE_UPDATE_PUBKEY=` by hand to everyone; there is deliberately no in-band key rotation, because that would be a second way to take over the fleet.

## Baked configuration

| CMake cache option | Default | Meaning |
|---|---|---|
| `GAGE_UPDATE_PUBKEY` | the production public key | base64 of the raw 64-byte public point; configure fails unless it is 88 characters ending `==` |
| `GAGE_UPDATE_URL` | `https://solar-rp.com/update/version.json` | first-install manifest URL; configure fails unless it is http(s) |

CMake keeps old cache values: after changing a default in `CMakeLists.txt`, re-pass `-DGAGE_UPDATE_URL=...` (or delete the cache entry) and check `build/win/gen/build_config.h`.

## URL migration (bare-IP HTTP to the solar-rp.com HTTPS URL)

A launcher already installed keeps checking its baked URL, so every GAGE-MP 0.3.x launcher still polls `http://5.175.253.187/update/version.json`. To move them to `https://solar-rp.com/update/version.json` without a hand-delivered exe:

1. Publish the first Solar RP release signed with `--update-url https://solar-rp.com/update/version.json`.
2. A launcher that verifies that manifest stores the URL in `HKCU\Software\GAGE-MP` `update_url`. From then on it asks that URL first and falls back to the baked one.
3. Only a verified payload whose version is not older than the running launcher may move the URL, so an old manifest replayed to a client cannot redirect it. The URL must be http(s), at most 256 characters, with no userinfo or fragment.
4. Keep both manifests current until the old host stops being needed.

Deleting the registry value returns a client to the baked URL.

### Launchers that still bake the IP

The bare-IP URL is the legacy / straggler path: it is plain HTTP, kept only so launchers built before solar-rp.com existed can still find the Solar RP release that moves them. Serve the same `version.json` at both addresses until the fleet has moved (the signature, not the transport, is the trust). The signed `url` names `SolarRP-Setup-<v>.exe`; a 0.3.x launcher downloads it and keeps its local copy as `GAGE-MP-Setup-<v>.exe` in its `updates` folder (the local file name does not matter). The setup finds the old install by its unchanged AppId, keeps the folder, replaces `GAGE-MP.exe` with `SolarLauncher.exe`, removes the old shortcuts and starts the new exe, because it still honours `/GAGEUPDATE`.

## Lockstep releases

Client and server releases move together (`tools/deploy/release.sh`, docs/OPS.md "Release (client + server together)"), and the players are forced onto the matching client as fast as possible. The launcher's side of that is `apps/launcher/version_gate.h` (pure functions, pinned by `tests/version_gate_test.cpp`), wired in `gage_gui.cpp`:

- **Every available update is mandatory.** A verified manifest that names a newer version blocks PLAY (`launchResult` code 14) and downloads and installs at once, whatever the signed `mandatory` flag or the "Install updates automatically" setting say. The `update` message reports `mandatory: true` for every available update. If GTA V runs with Solar RP loaded, the verified installer waits (`deferred`) and installs when the game exits.
- **When it looks for a release.** At start; every **2 minutes** while the launcher stays open (tray included); and, at most **once a minute** (`kMinCheckGapMs`), when the window is brought forward or restored from the tray, when the game exits, and when a server reports a protocol ahead of this launcher. PLAY itself checks first if the last check is older than a minute: it waits for that check (at most 8 s; the status line says "Checking for updates…"), so a release published a minute ago is never skipped. The SPA's own `checkUpdate` is not rate-limited.
- **The protocol gate.** The server's UDP query reports the protocol it enforces. When the launcher's PLAY reaches `DoLaunch` it asks the target server first (1.5 s at most; a silent server is not gated). Server protocol **ahead** of the launcher's `gage::proto::kVersion`: PLAY is refused with code 14 and "Solar RP was updated. Updating the launcher...", and an update check starts at once (or the pending-update text is shown when an update is already known). Server **behind**: code 15, "This server has not been updated yet. Try again in a minute." Equal or unknown: allowed. In demo mode the status worker also reads the protocol every 5 s, so a server ahead of the launcher starts the update check before anyone presses PLAY.
- **The backstop.** A game that starts anyway and meets a server of another protocol gets `Refuse(VersionMismatch)` with the same two sentences (server: "Solar RP was updated. Close the game; Solar Launcher installs the update." for an older client, "This server has not been updated yet. Try again in a minute." for a newer one). The engine shows the sentence on the loading screen's status line and in chat, repeated every 12 s while the player stays.

## What the launcher does

- Checks once at start (always, because an available update is mandatory) and on every `checkUpdate` from the SPA, then on the schedule in "Lockstep releases" above (2-minute poll plus event-driven checks, at most one a minute).
- An available update downloads to `<install dir>\updates\SolarRP-Setup-<v>.exe.part`, is verified, then installed; `autoUpdate` and the manifest's `mandatory` no longer change that (the setting is still stored and shown). If the install folder is not writable it uses `%LOCALAPPDATA%\GAGE-MP\updates`.
- Old setups and `.part` files are deleted once at start from both places the folder can be (`<install dir>\updates` and the `%LOCALAPPDATA%` fallback); `setup.log`, the last installer's log, is kept. A file still in use stays and nothing fails because of it. A hash or size mismatch deletes the file at once.
- It never replaces its own files under a running game: if GTA V runs with a Solar RP DLL loaded (gage_mp.dll and friends) (or a launch is in progress) the verified installer waits (`deferred`), is retried every 15 s and when the game exits, and the SPA is told.
- Install: `SolarRP-Setup-<v>.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /GAGEUPDATE /LOG=<updates>\setup.log`, started with the launcher's own token (no extra UAC prompt; the setup is per-user, `PrivilegesRequired=lowest`), then the launcher exits about 1.5 s later. `/NOCLOSEAPPLICATIONS` (not `/CLOSEAPPLICATIONS=no`, which is not an Inno switch) keeps Setup from touching the launcher itself.
- `installer/gage-mp.iss` sees `/GAGEUPDATE`, waits up to 30 s for the launcher's single-instance mutex to disappear (without that wait the silent AppMutex check aborts setup with exit code 1), installs, and starts the new SolarLauncher.exe. A silent install run by hand carries no `/GAGEUPDATE` and starts nothing, as before.
- `launch` is refused with code 14 while an update is pending (every available update is mandatory) or setup is starting; code 15 when the server is behind this launcher.
- Every refusal and failure logs one `update:` line to gage.log (signature invalid, not newer, malformed, size or hash mismatch, download failed, deferred and why).

## Tests

`node tests/update_check_test.mjs --exe build\win\selftest\update_check_test.exe` generates two throwaway key pairs and fixtures with `sign.mjs`, serves them from a local test server and from `tools/update-server/serve.mjs`, and runs the exe against them. It covers base64, SHA-256, version compare, URL rules, good and bad signatures (wrong key, flipped payload byte, flipped signature bit, truncated base64, short or long signature), payload rules, fallback and migration, the write-protected handle, the size cap, download failures and an end-to-end download plus verify that stops before executing anything. The tests never run an installer. The last line is `CHECKS <n> FAILED <m>`; the driver exits non-zero unless there are at least 150 checks, no failure and all eight scenarios.

Build the exe with `cmake --build build/win --target update_check_test`.

## Known limits

- The launcher's state machine (timers, window messages, the deferred retry, the exit 1.5 s after setup starts) is only exercised by compile and by the pure functions underneath it; a live run needs a second signed release and a running SolarLauncher.exe.
- Setup was proven with stub binaries from a non-elevated shell. The shipping launcher is `requireAdministrator`; its child setup inherits that token. Run one real update on a normal machine before relying on it.
- If setup fails after the launcher has exited, nothing restarts the old version. The next manual start checks again and retries.
- A launcher left open looks again every 2 minutes (and on the events listed under "Lockstep releases"), so a release reaches it within about that time; one that cannot reach the update host keeps its last known state and retries on the next tick.
- The lockstep behaviour (the 2-minute poll, the activation / restore / game-exit / PLAY checks, the protocol gate, the parked PLAY) is pinned by `tests/version_gate_test.cpp` for the decisions only; the window-message wiring in `gage_gui.cpp` compiles and was not run against a live second release.
- Launchers built before solar-rp.com existed fetch over plain HTTP (the bare-IP URL). The signature carries the trust, not the transport; see the attacker table.
