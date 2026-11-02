# update-server

This server hosts the launcher's signed update manifest and setup downloads on this machine, for tests and rehearsals. It listens on 127.0.0.1 only (default port 18600) and has no dependencies. The format, the trust model and the production publish steps are in `docs/UPDATES.md`; this page is about the local server.

It serves the manifest bytes untouched. It never signs anything: `tools/update-sign/sign.mjs` makes the signature, and the launcher refuses a manifest that does not verify against the public key baked into it.

| Request | Served from |
|---|---|
| `GET /version.json` or `GET /update/version.json` | `version.json` in this folder (or `--version-file`), `Cache-Control: no-store` |
| `GET /download/<setup>` or `GET /update/<setup>` | `build/win/installer/<setup>` (or `--installer-dir`); only names like `SolarRP-Setup-1.2.3.exe` (or the pre-rename `GAGE-MP-Setup-1.2.3.exe`) |
| anything else | 404 |

The `/update/...` aliases mirror the VPS layout (`https://solar-rp.com/update/version.json`; the bare-IP `http://5.175.253.187/update/version.json` is the legacy path for launchers built before the domain).

Run it with `node tools/update-server/serve.mjs`. Options: `--port N` (0 picks a free port; the real one is in the "listening" line), `--version-file PATH`, `--installer-dir DIR`. It exits non-zero if the port is taken or the version file is missing or invalid, and it logs one line per request. On start it warns on stderr when the version file is the old unsigned format: launchers with the signed updater ignore such a file.

**The `version.json` in this folder is an unsigned sample of the old format.** It is kept only as a fixture for that warning. A real manifest comes from `sign.mjs`.

**Rehearse an update on this machine** (never against a launcher you are using; it runs the real installer):
1. Make a throwaway key pair: `node tools/update-sign/keygen.mjs --out-dir %TEMP%\upd\k`. Never use the production private key for this.
2. Configure a scratch build with that public key and this server's manifest URL: `cmake -DGAGE_UPDATE_PUBKEY=<contents of update-signing-pub.txt> -DGAGE_UPDATE_URL=http://127.0.0.1:18600/version.json build/scratch`.
3. Build a setup with a higher `kClientVersion` than that build (`installer/build-installer.sh`), then sign it: `node tools/update-sign/sign.mjs --key %TEMP%\upd\k\update-signing-key.pem --installer <setup.exe> --version <x.y.z> --url http://127.0.0.1:18600/download/<setup.exe> --out tools/update-server/version.json`.
4. Run the server and start the scratch launcher. gage.log prints one `update:` line per step.

**Point a real build at a real host:** the baked default is `https://solar-rp.com/update/version.json`; for another host run `cmake -DGAGE_UPDATE_URL=https://<host>/update/version.json build/win` (CMake keeps the old cache value unless you pass the option again, so pass it even for the default after a reconfigure of an old build dir), then rebuild. The URL is compiled into the launcher through `build/win/gen/build_config.h`. A launcher that is already shipped keeps its old URL until a signed manifest moves it with `updateUrl` (docs/UPDATES.md, "URL migration").

**Test:** `node tests/update_check_test.mjs --exe build\win\selftest\update_check_test.exe` (build the exe first with `cmake --build build/win --target update_check_test`). It starts this server itself for its own scenarios, with throwaway keys. The tests never run an installer.
