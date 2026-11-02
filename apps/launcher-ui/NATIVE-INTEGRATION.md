# Native integration: wiring apps/launcher-ui into SolarLauncher.exe

This is everything the WebView2 host (`apps/launcher/gage_gui.cpp`) needs to change to run the new launcher UI. It is written so the work can be done in one pass. `apps/launcher/BRIDGE.md` is still the contract for the transport, the origin rules and every message marked *legacy* below; this file adds the rest and supersedes BRIDGE.md where they disagree:
- "no network requests": the SPA now calls the platform API;
- "system font stack": the fonts are inlined;
- "only index.html": `/assets/` is now served.

Source of truth for the shapes: `src/lib/bridge.ts`. Incoming messages are zod-validated there, and unknown `type`s are ignored. Its browser twin is `src/lib/bridge-mock.ts`, which answers every message with the shapes below, and `tests/bridge.test.ts` pins both.

---

## 1. Build outputs

`npm run build` (in `apps/launcher-ui`, Node 22.12+) runs `asset-manifest → tsc --noEmit → vite build` and writes:

| Output | What | Size (last build) |
|---|---|---|
| `dist/index.html` | The whole SPA in one file: JS, CSS and the 4 woff2 fonts inlined. No external URL is fetched at runtime besides `./assets/…` and the platform API. | 1,538,345 B |
| `dist/assets/**` | GTA V footage and stills: 5 muted H.264 MP4 loops, each with a WebP poster and a WebP card thumbnail, 7 WebP stills and 5 WebP product thumbnails (ASSETS.md). Copied verbatim from `public/assets/`. | 12,484,333 B, 28 files (27 plus `manifest.json`) |
| `dist/assets/manifest.json` | `{version:1,totalBytes,files:[{path,bytes,sha256,type}]}`: every asset with its SHA-256 and MIME type, sorted. | ≈3 KB |

**Embedding.** Point CMake's `GAGE_UI_HTML_IN` at `${CMAKE_SOURCE_DIR}/apps/launcher-ui/dist/index.html`. Today it points at `docs/archive/web-legacy/dist/index.html`. `tools/embed_ui.cmake` needs no change, because the file is plain bytes like before. `CMakeLists.txt` is owned by the platform agent, so this one-line change goes through them.

**Installer.** `dist/assets/**` ships next to the exe as `{app}\launcher-assets\`. Remove the old tree first, so a renamed or deleted asset never lingers:

```iss
[InstallDelete]
Type: filesandordirs; Name: "{app}\launcher-assets"

[Files]
Source: "..\apps\launcher-ui\dist\assets\*"; DestDir: "{app}\launcher-assets"; Flags: recursesubdirs ignoreversion
```

---

## 2. URL → file mapping under `https://app.gage/`

The page loads from `https://app.gage/index.html` and requests its assets by relative URL (`./assets/video/city.mp4` resolves to `https://app.gage/assets/video/city.mp4`). The existing `WebResourceRequested` filter `https://app.gage/*` already sees every request. Extend `ResHandler::Invoke` as follows:

| Request (query and fragment stripped) | Response |
|---|---|
| `https://app.gage/` or `https://app.gage/index.html` | `kUiHtml`, `200`, `Content-Type: text/html; charset=utf-8`, `Cache-Control: no-store` (unchanged) |
| `https://app.gage/assets/<rel>` where `<rel>` is listed in `manifest.json` (including `manifest.json` itself) | The bytes of `{exeDir}\launcher-assets\<rel>` (`/` → `\`), `200`, `Content-Type` from the table below, `Cache-Control: no-store`, `X-Content-Type-Options: nosniff` |
| anything else under the origin | `404` (unchanged) |

Rules for `<rel>`:
- Serve only paths that appear in `manifest.json`. That allow-list is the whole path validation: no `..`, no `\`, no `%`-escapes, no drive letters ever reach the file system.
- The manifest is read once at startup from `{exeDir}\launcher-assets\manifest.json`. If the manifest is missing or unreadable, log it and serve 404 for every asset. The UI then falls back to its black ground with the emblem and the interface intact (the boot cover lifts after 5 s without a picture); it never hangs.
- Optional, recommended: check each file's size against the manifest on first serve (cheap). A full SHA-256 check of all 12.4 MB costs about 35 ms if you want it at startup. On a mismatch, log and 404.
- Serve each file whole from an `SHCreateStreamOnFileEx` stream (read-only, `STGM_SHARE_DENY_WRITE`). **Range requests are not needed, video included.** A `<video>` over a whole-file 200 is not seekable and cannot loop reliably, so the UI fetches each MP4 once with `fetch()` into a Blob and plays it from a `blob:` URL (`src/media/Backdrop.tsx`). Verified in Chrome for Testing 156 against a server that sends no `Accept-Ranges`: seekable to the full duration and wrapping at the loop point. If you ever add Range support (`206`, `Accept-Ranges: bytes`), nothing changes on the UI side.
- Send `Content-Length` with every asset; the MP4s are 0.9-3.3 MB each.
- No CORS headers: every asset request is same-origin.

| Extension | Content-Type |
|---|---|
| `.mp4` | `video/mp4` |
| `.webp` | `image/webp` |
| `.json` | `application/json` |
| `.webm` / `.avif` | `video/webm` / `image/avif` (none ship today; mapped so a VP9 fallback or AVIF stills can be added without a host change) |
| `.png` / `.jpg` / `.glb` / `.hdr` / `.ktx2` / `.woff2` | `image/png` / `image/jpeg` / `model/gltf-binary` / `image/vnd.radiance` / `image/ktx2` / `font/woff2` (none ship today; `asset-manifest.mjs` refuses an unknown extension) |

The manifest carries each file's `type`; the host may take `Content-Type` from it instead of keeping its own table.

**Page background.** Change `kBgColor` from `#0b0e15` to the new ground `#07080a`, i.e. `{ 255, 7, 8, 10 }`. It shows during resize and before first paint.

**H.264.** The loops are H.264 High (`avc1.640029`). WebView2 decodes it through Windows Media Foundation. On Windows N/KN without the Media Feature Pack, `canPlayType` returns `''` and the UI shows each loop's poster instead (a still, by design). No host change is needed for that.

---

## 3. Bridge messages

The transport is unchanged (BRIDGE.md): JS → native `chrome.webview.postMessage(obj)`; native → JS `PostWebMessageAsJson(json)`. Every message is a flat object with a `type`, and only `https://app.gage/` may use the bridge.

Status key:
- **legacy**: unchanged from BRIDGE.md.
- **changed**: extra fields; an old host ignores them.
- **NEW**: native must add it.

### 3.1 JS → native

| Message | Status | Native does / replies |
|---|---|---|
| `{"type":"ready"}` | legacy, **must** | Reply `settings` (3.2). The UI waits for `settings` before it restores the session, because `apiBase` comes from it, so it must arrive. Its `gamePath` must already be the auto-detected game when none is saved: `DetectGamePathIfUnset()` in `apps/launcher/gage_gui.cpp` does this today, before `PostSettings()`, from the Rockstar, Steam and Epic records. Settings shows the path as detected, and the Play screen offers PLAY with no setup step. An empty `gamePath` means nothing was found: Settings and Play then say the GTA V folder was not found and point to Browse. Also send the current `window` state. Send any queued `deepLink` after `settings`. |
| `{"type":"getSettings"}` | legacy | Reply `settings`. |
| `{"type":"saveSetting","key":K,"value":V}` | changed | Also accept `key:"launchOptions"`, a string of at most 512 chars, trimmed. Persist it and reply `settings`. The other keys are as in BRIDGE.md. |
| `{"type":"browseGamePath"}` | legacy | As in BRIDGE.md; reply `settings`. |
| `{"type":"checkUpdate"}` | legacy | Reply `update`. The SPA sends it once after `ready` (the host's first check may have reported before the page listened), on `launchResult` code 14 and from the update popup's Try again; `docs/UPDATE-POPUP-REVIEW.md` has the whole mapping. |
| `{"type":"copyText","text":S}` | legacy | Reply `copied`. |
| `{"type":"minimize"}` / `{"type":"close"}` / `{"type":"quit"}` | legacy | As in BRIDGE.md (close = hide to tray). |
| `{"type":"toggleMaximize"}` | **NEW** | `ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE)`, then send `window`. A double-click on the drag region already does this natively; this message is for the rail's maximize button. |
| `{"type":"launch","server":<serverObj>,"gamePath":S,"playerName":S,"serverId":S,"launchOptions":S}` | changed | As in BRIDGE.md, plus the following. `serverId` is the platform's server id; keep it for `joinTokenNeeded`. `launchOptions` is appended to the game's arguments where the store passes them on. Send `launchStage` at each stage (3.2). Right before injection, send `joinTokenNeeded` and wait for `joinToken` (3.3). Send `game` when the game process starts and when it exits. |
| `{"type":"joinToken","serverId":S,"token":S\|null,"expiresAt":S\|null,"error":S}` | **NEW** | The reply to `joinTokenNeeded`; see 3.3. |
| `{"type":"tokenGet"}` | **NEW** | Reply `{"type":"token","token":S\|null}` within 1.5 s; the UI treats silence as "no token". |
| `{"type":"tokenSet","token":S}` | **NEW** | Store the session token with DPAPI (`CryptProtectData`, user scope, `CRYPTPROTECT_UI_FORBIDDEN`) in `%LOCALAPPDATA%\GAGE-MP\session.bin`. Write atomically: temp file, then `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING`. Reply `tokenSaved`. Refuse an empty token or one over 512 chars with `ok:false`. Never log the token. |
| `{"type":"tokenClear"}` | **NEW** | Delete `session.bin` and reply `tokenSaved` (`ok:true` when the file is gone, including when it never existed). |
| `{"type":"openExternal","url":S}` | **NEW** | Only `https://` URLs of at most 2048 chars: `ShellExecuteW(nullptr, L"open", url, …)`, then reply `opened`. Refuse anything else with `ok:false`. Used for checkout pages and the Discord authorize page. |
| `fetch`, `refresh`, `ping`, `addFavorite`, `removeFavorite`, `clearFavorites`, `addRecent`, `clearRecents` | legacy, **unused** | The new UI never sends these: the server list comes from the platform API (`GET /v1/servers`). Keep them if the legacy SPA must still run. |

### 3.2 native → JS

| Message | Status | When |
|---|---|---|
| `{"type":"settings","gamePath":S,"playerName":S,"language":S,"autoUpdate":B,"startMinimized":B,"closeGame":B,"version":S,"launchOptions":S,"apiBase":S}` | changed | After `ready`, `getSettings`, `saveSetting` and `browseGamePath`. **`apiBase`** is the platform API origin, e.g. `https://api.gage-mp.net`, with no trailing path. Add `kDefaultApiBase` to `tools/build_config.h.in`, overridable by an `api_base=` line in `gage_mp.cfg` the way `list_url` is. Accept only `https://…`, or `http://127.0.0.1[:port]` / `http://localhost[:port]` for development. Without it every API call fails with "not configured". |
| `{"type":"status","text":S}` | legacy | During launch, as before. |
| `{"type":"launchStage","stage":"preflight"\|"starting_store"\|"waiting_for_game"\|"injecting"\|"injected"}` | **NEW** | At each transition, in this order: integrity check (code 11 is decided here) → store asked to start the game → polling for the game process → injecting → injected. Skip stages that do not apply; for example, attaching to a running game goes `preflight` → `injecting`. |
| `{"type":"launchResult","ok":B,"code":N,"error":S}` | legacy, + code 12 | As before. **New code 12**: no join token, because the reply to `joinTokenNeeded` had `token:null` or did not come within 15 s. Put the `joinToken.error` text in `error`. The game is left running, and pressing PLAY again attaches to it. |
| `{"type":"joinTokenNeeded","serverId":S}` | **NEW** | See 3.3. |
| `{"type":"game","running":B}` | **NEW** | `true` once the game process this launcher started or injected into is alive after `injected`; `false` when it exits. Wait on the process handle, e.g. `RegisterWaitForSingleObject`, and post to the UI thread. The UI pauses the footage and the emblem while `running` is true. |
| `{"type":"window","maximized":B,"focused":B,"visible":B}` | **NEW** | After `ready`, and on every change: `WM_SIZE` (maximize, restore, minimize), `WM_ACTIVATE` (focus), hide to tray and restore. `visible` is false when the window is minimized or hidden. This message drives the rail's maximize/restore icon and the render pause. |
| `{"type":"token","token":S\|null}` | **NEW** | Reply to `tokenGet`. |
| `{"type":"tokenSaved","ok":B}` | **NEW** | Reply to `tokenSet` / `tokenClear`. |
| `{"type":"opened","ok":B,"url":S}` | **NEW** | Reply to `openExternal`. |
| `{"type":"deepLink","url":S}` | **NEW** | A `gage-mp://…` URL Windows handed to the exe. See 3.4. At most 2048 chars. |
| `{"type":"discordCallback","flowId":S}` | NEW, optional | Send it if a `gage-mp://discord/…` link is ever registered. It makes the UI poll the Discord flow at once (`flowId` may be omitted). Without it the UI polls every 2 s on its own. |
| `{"type":"update",…}`, `{"type":"copied",…}` | legacy | Unchanged. |
| `servers`, `ping`, `pings`, `favorites`, `recents` | legacy, **ignored** | The new UI drops them as unknown types. Sending them is harmless. |

### 3.3 Join token at injection (why and how)

A join token lives `JOIN_TOKEN_TTL_SECONDS`, 60 s by default, and a cold GTA V start takes minutes. So:
1. At PLAY, the UI calls `POST /v1/game/join` once as a **gate**. Unverified email, ban, maintenance or an unknown server are refused before the game starts.
2. Then it sends `launch` (no token in it).
3. Once the game process is up and native is about to inject, native sends `{"type":"joinTokenNeeded","serverId":"<launch.serverId>"}`. The UI mints a fresh token and replies with one of:
   - `{"type":"joinToken","serverId":"…","token":"gmj_…","expiresAt":"<ISO>","error":""}`
   - `{"type":"joinToken","serverId":"…","token":null,"expiresAt":null,"error":"<human reason>"}`
   Minting again voids the gate token, because the platform keeps one live token per account.
4. Native ignores a `joinToken` whose `serverId` differs from the pending launch. It writes the token where the engine reads the join target: `HKCU\Software\GAGE-MP`, value `join_token` (REG_SZ), next to `server_host`, `server_port` and `player_name`. It writes `server_id` (REG_SZ) too. Then it injects.
5. The engine sends `join_token` in its connect packet, and the game server redeems it with `POST /internal/game/verify`. See `apps/platform/docs/game-server-contract.md`.
6. Native deletes `join_token` from the registry once the engine has read it, or at the latest when the game exits. The value is single-use and worthless after 60 s, but it should not sit in the registry.

If the reply is `token:null`, or nothing arrives within 15 s, native sends `launchResult` `{ok:false, code:12, error:<joinToken.error or "No answer from the launcher UI.">}`.

### 3.4 Deep links (`gage-mp://`)

The platform mails `gage-mp://auth/verify-email?token=…` and `gage-mp://auth/reset-password?token=…` (`EMAIL_LINK_BASE_URL`, default `gage-mp://auth`). It never consumes a token by GET.
- **Installer:** register the protocol per user:
  ```iss
  [Registry]
  Root: HKCU; Subkey: "Software\Classes\gage-mp"; ValueType: string; ValueData: "URL:Solar RP"; Flags: uninsdeletekey
  Root: HKCU; Subkey: "Software\Classes\gage-mp"; ValueName: "URL Protocol"; ValueType: string; ValueData: ""
  Root: HKCU; Subkey: "Software\Classes\gage-mp\shell\open\command"; ValueType: string; ValueData: """{app}\SolarLauncher.exe"" --url ""%1"""
  ```
- **First instance:** take `--url <u>` from the command line. Keep it only if it starts with `gage-mp://` (case-insensitive) and is at most 2048 chars. Queue it, and send `deepLink` after `ready` + `settings`.
- **Second instance** (single-instance mutex taken): before `ActivateRunningInstance()` / `ExitProcess`, forward the URL to the running window with `WM_COPYDATA` (`dwData = 'GGLK'`, UTF-16, at most 2048 chars). The running instance validates it the same way, then sends `deepLink` and restores its window.
- The UI handles `verify-email` (POST `/v1/auth/verify-email`, then a confirmation line) and `reset-password` (prefills the reset frame). If the mail was opened on another device, the link can also be pasted into the launcher (profile → "Paste the link", or the reset frame).

---

## 4. Window controls and drag region (frameless)

| Element | CSS | Behaviour |
|---|---|---|
| The top rail, `.rail`: 60 px tall, full width | `app-region: drag` | Drags the window; Win11 snap; double-click maximizes or restores; right-click opens the system menu. |
| Every button and link in the rail: section tabs, account chip, window buttons | `app-region: no-drag` | Clickable. |
| Rail window buttons: Minimize, Maximize/Restore, Close | — | They send `minimize`, `toggleMaximize` and `close` (hide to tray). Settings → "Quit Solar Launcher" sends `quit`. |
| Top 8 px of the page | nothing clickable there | WebView2's top resize band. Rail controls start at y = 8; the capture gate asserts it on every frame. |

The host must keep `ICoreWebView2Settings9::put_IsNonClientRegionSupportEnabled(TRUE)`; it does today. Left, right and bottom resizing stay native as today.

**Window size.** The layout is designed at 1240×800 and is fluid from 1024×700 up to a maximized 1440p window. It is captured and verified at 1024×700, 1240×800 and 1440×900. The UI does not zoom itself; keep the host's existing minimum and opening size rules.

---

## 5. Render pause

The background loop plays, and the emblem's small 3D canvas renders continuously (`frameloop="always"`), only while `renderActive()` (`src/ui/hooks.ts`) is true. Otherwise the video is paused on its current frame and the canvas renders nothing (`never`). Under `prefers-reduced-motion` the loop's poster stands in for the video and the emblem renders on demand (a still).

| Pauses when | Signal the UI uses |
|---|---|
| Window minimized or hidden to tray | `window.visible=false`; also `document.visibilitychange`, which fires because the host calls `put_IsVisible(FALSE)` (it already does, `SetWebVisible`) |
| Unfocused **and** idle for 10 s | `window.focused=false` (or the page `blur` event) plus no pointer/key input for 10 s |
| A launch owns the window (the launch overlay is up) | Internal launch state. The footage keeps playing under `joining` and `launching` (the launch is set over the same shot) and stops at `launched`; the emblem now does the same (it stands beside the server name on the launch). |
| The game is running | `game.running=true` |

The server-list polling (every 15 s) stops on the same visibility, game and launch conditions.

---

## 6. Platform API

- The UI calls `settings.apiBase` + `/v1/...` with `Authorization: Bearer <session>` and `credentials: 'omit'`. Every route is in `src/lib/api.ts` (`ROUTES`) and documented in `apps/platform/docs/api.md`.
- **CORS** is already in place on the platform: `CORS_ORIGINS` defaults to `https://app.gage`. It allows the `Authorization`, `Content-Type`, `Idempotency-Key` and `X-Request-Id` headers and exposes `Retry-After`. Keep `https://app.gage` in `CORS_ORIGINS` on every deployment.
- The session token lives only in DPAPI storage through the bridge (`tokenGet`/`tokenSet`/`tokenClear`), never in `localStorage`.

### Known API gaps (the UI works around them; listed for the platform owner)

| Gap | UI today |
|---|---|
| No route to change the account email | Settings shows the email read-only. |
| No VIP expiry field | The UI derives VIP time from the owned `vip` entitlement grants (`vipWindow()`, documented as the client's reading of the ledger). |
| No per-server art field | Footage and card art by keywords in the server name and tags (harbor/port/beach → harbor; vinewood/hills/boulevard → vinewood; race/drift/street/night/police → night; freeroam/sandbox/county → wild; downtown/city → city), otherwise rotated by list position (`serverArt()` in `src/media/catalog.ts`). An optional `art: "city"\|"harbor"\|"wild"\|"night"\|"vinewood"` field on the server would let the operator choose. |
| No per-product art field | Still by first item-grant key prefix: `vehicle.*` → vehicle, `outfit.*` → outfit, other items → watch; VIP → vip; otherwise coins (`productStill()` over `showroomModelFor()`). The product schema's `imageUrl` is not used: a remote image would need a CSP and host decision. |
| No eligibility check without minting a join token | The PLAY gate mints a token that the injection-time token voids (3.3). |

---

## 7. Checklist for the native pass

1. `GAGE_UI_HTML_IN` → `apps/launcher-ui/dist/index.html` (through the CMake owner).
2. `ResHandler`: serve `/assets/<rel>` from `{exeDir}\launcher-assets\` against `manifest.json`, with `video/mp4` for `.mp4` (section 2); set `kBgColor` to `#07080a`.
3. `settings` gains `apiBase` (`kDefaultApiBase` + `api_base=` override) and `launchOptions`; `saveSetting` accepts `launchOptions`.
4. Add `toggleMaximize`, `tokenGet`/`tokenSet`/`tokenClear` (DPAPI), `openExternal` (https only) and `joinToken`.
5. Send `window`, `game`, `launchStage`, `joinTokenNeeded` and `deepLink`; add launch result code 12.
6. Registry: `join_token` and `server_id` for the engine, deleted after use; the engine sends `join_token` in its connect packet.
7. Installer: the `{app}\launcher-assets` tree (delete, then copy) and the `gage-mp://` protocol keys; second-instance URL forwarding over `WM_COPYDATA`.
8. Verify inside the exe:
   - the boot cover, then the city loop playing and looping past its 11.5 s mark;
   - drag, snap and double-click on the rail;
   - minimize → the video pauses and the GPU drops to idle (render pause);
   - log in → the token survives a restart;
   - PLAY → stages → `joinTokenNeeded` → launched;
   - the game exits → the footage and the emblem resume;
   - a `gage-mp://auth/verify-email?token=…` link opens the running launcher and verifies.
