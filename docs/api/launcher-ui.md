# Solar Launcher UI reference

The reference for `apps/launcher-ui`, the React SPA that SolarLauncher.exe hosts in WebView2. It describes what the shipped code does.
- Wiring it into the exe (URL mapping, every bridge message with its JSON, window and drag contract, render pause, installer) is in [`apps/launcher-ui/NATIVE-INTEGRATION.md`](../../apps/launcher-ui/NATIVE-INTEGRATION.md).
- The platform routes it calls are in `apps/platform/docs/api.md`.

Contents

1. [Build, run, test](#1-build-run-test)
2. [Architecture](#2-architecture)
3. [Bridge (summary)](#3-bridge-summary)
4. [Platform API client](#4-platform-api-client)
5. [State machines](#5-state-machines)
6. [Screens and states](#6-screens-and-states)
7. [Components](#7-components)
8. [Background media and the emblem](#8-background-media-and-the-emblem)
9. [Demo mode (mock bridge and mock API)](#9-demo-mode)
10. [Assets](#10-assets)

## 1. Build, run, test

Run these in `apps/launcher-ui`, with Node 22.12+:

| Command | Does |
|---|---|
| `npm run dev` | Vite on `http://127.0.0.1:5317` (strict port), with the mock bridge and mock API. |
| `npm run typecheck` | `tsc --noEmit`. Strict, with `noUncheckedIndexedAccess`; covers `src`, `tests` and `vite.config.ts`. |
| `npm test` | Vitest: 6 files, 90 tests (API client, bridge and mock, email links, auth/launch/shop state, render pause, media mapping and files on disk). |
| `npm run build` | `scripts/asset-manifest.mjs` → `tsc --noEmit` → `vite build`, producing `dist/index.html` (single file) and `dist/assets/**`. |
| `npm run manifest` | Rewrites `public/assets/manifest.json` (path, bytes, sha256 and MIME type of every asset). |

The dependencies are pinned exactly in `package.json`:
- runtime: React 18.3.1, three 0.173.0, @react-three/fiber 8.18.0, drei 9.122.0, gsap 3.15.0, lenis 1.3.26, zod 4.6.7. `@react-three/postprocessing` 2.19.1 and `postprocessing` 6.36.7 are still listed but no longer imported (the retired street scene was their only user; `docs/archive/launcher-3d-street/`);
- build: Vite 6.4.4, vite-plugin-singlefile 2.2.1, TypeScript 5.9.3, Vitest 3.2.7.

## 2. Architecture

```
main.tsx ─ App.tsx ─┬─ Backdrop (media/)       full-bleed GTA V loop or still, crossfades, paused by renderActive()
                    ├─ Rail (ui/)              40 px drag rail: section tabs, account, window buttons
                    ├─ AuthScreen | PlayScreen | ShopScreen | ProfileScreen | SettingsScreen
                    ├─ EmblemLayer (three/)    the one R3F canvas: the chrome G, laid over the mounted <EmblemSlot>
                    └─ LaunchOverlay           PLAY push → the launch, over the same loop still playing
state/AppState.tsx  the single state owner: bridge, API client, auth, launch, settings, window, servers, email links
lib/                bridge.ts (+ bridge-mock.ts), api.ts (+ mock-api.ts), schemas.ts (zod), errors.ts, links.ts
media/catalog.ts    loop and still names, URLs (clipThumbUrl for server cards), serverArt(), productStill(), canPlayLoops()
```

Every side effect lives in `AppState.tsx`. Screens read it through `useApp()`, and the reducers in `state/` are pure and unit-tested.

## 3. Bridge (summary)

`src/lib/bridge.ts` defines `OutMsg` (JS → native) and `inMsgSchema` (native → JS, zod; unknown types are dropped). `createNativeBridge()` wraps `chrome.webview`. `request()` sends a message and resolves with the first matching reply, or `null` after a timeout, so an old host never hangs the UI.

| Direction | Messages |
|---|---|
| JS → native | `ready`, `getSettings`, `saveSetting`, `browseGamePath`, `checkUpdate`, `copyText`, `minimize`, `close`, `quit`, `launch`; **NEW** `toggleMaximize`, `tokenGet`, `tokenSet`, `tokenClear`, `openExternal`, `joinToken` |
| native → JS | `settings` (+ `apiBase`, `launchOptions`), `status`, `launchResult` (+ code 12), `update`, `copied`; **NEW** `launchStage`, `joinTokenNeeded`, `game`, `window`, `token`, `tokenSaved`, `opened`, `deepLink`, `discordCallback` |

The JSON shapes and the native behaviour for each message are in NATIVE-INTEGRATION.md §3. Launch error copy for every code (1–12) is `launchErrorCopy()`.

## 4. Platform API client

`createApi({ baseUrl, getToken, fetchImpl?, timeoutMs? })` in `src/lib/api.ts`. Every route string lives in `ROUTES`, every response is zod-validated (`schemas.ts`), and every failure becomes an `ApiFailure` with a `kind`:

| kind | Meaning |
|---|---|
| `not_configured` | No `apiBase` from the host. |
| `offline` | Network, DNS, TLS, CORS or connection refused. |
| `timeout` | No answer in 12 s. |
| `unauthorized` | 401 `unauthorized`: the session is dead. The UI clears the token and returns to login. |
| `rate_limited` | 429; `retryAfterSeconds` comes from `Retry-After` and drives the login lockout countdown. |
| `server` | 5xx (except a coded 503), or a non-API error body such as a proxy page. |
| `refused` | Any other coded refusal (`invalid_credentials`, `email_not_verified`, `server_maintenance`, …). |
| `invalid_response` | 2xx with a body that does not match the schema. |

Calls used:
- auth: `register`, `login`, `logout`, `verifyEmail`, `resendVerification`, `forgotPassword`, `resetPassword`, `discordStart`/`Poll`/`Complete`, `discordLinkStart`, `discordUnlink`;
- account: `me`, `changePassword`, `sessions`, `revokeSession`, `revokeOtherSessions`, `getSettings`/`putSettings`, `wallet`, `items`;
- play: `servers`, `join`;
- shop: `products`, `checkout`, `orders`, `order`.

`checkout` sends an `Idempotency-Key` from `newIdempotencyKey()`: 18 random bytes, base64url. Requests use `credentials: 'omit'` and `cache: 'no-store'`, and the bearer token comes from `getToken()` on every call.

**Email links** (`src/lib/links.ts`):
- `parseAuthLink(raw, schemes?)` reads `…/verify-email?token=` and `…/reset-password?token=` (token: base64url, 16–200 chars).
- `tokenFromPaste(raw, kind)` accepts a whole link or a bare token.
- A native `deepLink` only counts with the `gage-mp:` scheme.

## 5. State machines

| Reducer | States / events |
|---|---|
| `authReducer` (`state/auth.ts`) | `booting` → `signedOut{view, busy, error, lockedUntil, email, discord, reason}` ↔ `signedIn{account, token}`. Views: login, register, verifyNotice, forgot, forgotSent, reset, resetDone, discord, discordUsername. `NOTICE` returns to login with a one-line reason and keeps the lockout. |
| `launchReducer` (`state/launch.ts`) | `idle` → `joining` (gate `POST /v1/game/join`) → `launching{stage, statusText}` → `launched{gameRunning}` / `failed{title, body, action?}`; `game.running=false` after launch → `idle`. `isBusy()` = joining or launching. |
| `renderActive()` (`ui/hooks.ts`) | The background loop and the emblem run only while the window is visible, not (unfocused and idle for 10 s), no launch in progress, no launched overlay and no game. `App` asks it twice: the emblem with the launch counted (and stopped while the launch covers the screen), the loop without it, so the footage keeps playing under a joining or launching overlay and pauses once the launch hands off to the game. |
| Shop helpers (`state/shop.ts`) | `stampFor` (order → PAID / DELIVERED / AWAITING PAYMENT / EXPIRED / …, with a `stamp`/`pending`/`void` tone shown as a chip), `formatMoney` (ISO 4217 minor units), `vipWindow` (VIP time from entitlement grants), `showroomModelFor` (product → subject kind; `productStill` maps it to a still), `kindLabel`, `spentByCurrency`. |

## 6. Screens and states

Every screen is full-window over the footage: content on the left, one opaque column on the right (`--col`, 288–344 px), the emblem in the top-left of the content. All states are captured at 1024×700, 1240×800 and 1440×900.

| Screen | States built |
|---|---|
| Auth | Over the `city` loop: the emblem, the lockup and one line of copy on the left; an opaque chamfered panel on the right. login, register, verify notice, forgot, forgot sent, reset (paste a link or prefilled from a deep link), reset done, Discord wait, Discord username, wrong password, locked (429 countdown), offline, session-ended notice, email-verified notice. In demo mode "Enter with the demo account" is the primary button. |
| Play | The chosen server's loop full-bleed. Left: the server name (caps, 56–96 px), the online count at display size (48–64 px) with "Online now", "of N slots" and a capacity bar, then region, tags, ping, a "Demo counts" chip in demo mode, and the PLAY plate (64 px). The count shows only for an online server outside maintenance. Right: the server list, one card per server with a frame of its own loop as the thumbnail (`clipThumbUrl`), name, region and tags, count or status, and a fill bar. Pick by click or arrow keys (the loop crossfades and the emblem pulses), Enter plays. While a launch runs the plate stays blue, busy, and reads the current step. A line under PLAY appears only when PLAY is closed: maintenance (banner, PLAY disabled with a reason), offline/full (card dimmed, reason under PLAY), every server offline (one line saying so), GTA V folder not found (PLAY becomes "Set GTA V folder", which opens Settings). Also: loading, no servers (no list column), API down, stale list. |
| Launch | PLAY: the emblem charges, the footage pushes in 5% over 1.4 s and keeps playing (it is never darkened), the screen fades out from 0.32 s to 0.62 s. Then, below the rail (section tabs locked, window controls live), the launch over the same shot with one gradient from the left: the emblem and the server name in the hero's row (the emblem keeps rendering until the launch hands off to the game), a phase chip and a clock counted from PLAY (it freezes when the launch ends), a five-segment rail named JOIN, FILES, STORE, GAME, LOAD (join + 4 native stages; the full step text is read to screen readers) and the current step at display size, plus the host's `statusText` when it adds something. Launched (the loop pauses); failed (the footage greys) with copy per code and its action (Set GTA V folder, Go to profile). Back eases the push-in back to 1. |
| Shop | Showroom over the product's still: the product name, price, kind, a "Demo price · not for sale" chip, BUY (the price sits beside it, not in the label), checkout wait (opens the browser, waits on the order) and the last order; the product list on the right (same cards as the server list, with the price). Wallet in the subnav. Orders and Owned are full-height pages over a near-solid ground: the summary (wallet, VIP window, paid total per currency) as a tall left column, then the ledger (date, order id, item, total, state chip; refunded orders get a plain chip) or the owned shelf (large tiles: name, kind, state, date; the next slot links back to the showroom). The wallet chip in the sub-bar shows on the showroom only. Empty catalogue; unverified (BUY disabled with a reason). |
| Profile | Over the `downtown` still: username, role chips, "Since", a "Demo account" chip, banned banner; an opaque panel with email state (Resend and Paste-the-link verification), Discord link/unlink, wallet balances, owned items (with any still queued) and the VIP bar (one tick per day). |
| Settings | Over the `settings` still: the title and a section index on the left (follows the scroll, jumps on click); an opaque right panel with GTA V folder (shown as detected; "not found" with Browse when empty), launch options, local toggles, password change, devices (revoke one or all others), update check, sign out / quit, attributions and the not-affiliated line. |

## 7. Components

Primitives in `src/ui/primitives.tsx`:

| Component | Props | Notes |
|---|---|---|
| `Plate` | button props + `tone: primary\|ghost\|quiet\|danger`, `size: sm\|md\|lg\|xl`, `busy` | Button with one chamfered corner (top right). `primary` is brand blue and appears once per screen. |
| `Chip` | `tone: done\|pending\|void\|demo\|warn\|plain` | Small state label. `done` carries a check, `demo` a dashed outline. |
| `Code` | `tone: dim\|ink` | Condensed caps for codes, counts and clocks. |
| `StatusDot` | `state: online\|offline\|unknown\|maintenance` | |
| `Field` | input props + `label`, `error`, `hint` | `error` sets `aria-invalid` and a `role=alert` line with an alert mark. |
| `Toggle` | `label`, `checked`, `onChange`, `hint?`, `disabled?` | `role=switch`; ink when on (blue is reserved for PLAY, selection and focus). |
| `GageMark` / `Lockup` / `Icon` | size props | The brand mark from `src/brand/` (copied from `brand/`, not imported across folders). |

`EmblemSlot({className})` (`src/ui/emblemSlot.tsx`) marks where the emblem sits on a screen; `EmblemLayer` follows the mounted slot. At most one slot is mounted at a time.

Screen components: `PlayScreen({onPlay})`, `ServerList({servers, selectedId, onSelect, demo})`, `ShopScreen({onStill})` (reports the still to show behind the chosen product), `ProfileScreen()`, `VipBar({vip})`, `SettingsScreen()`, `AuthScreen()`, `LaunchOverlay({onBack, startedAt})` (`startedAt` is the epoch ms of the PLAY press, set by `App`), `Rail({locked?})`. Exported pure helpers: `blockedReason(server)`, `fmtDate(iso)` (numeric parts, `29 OCT 2026`), `spentByCurrency(orders)` (`src/state/shop.ts`: the total of paid and fulfilled orders per currency; refunded and open orders are not counted).

## 8. Background media and the emblem

**Backdrop** (`src/media/Backdrop.tsx`): `Backdrop({scene, active, reducedMotion, onReady})`, where `scene` is `{clip}` (a loop) or `{still, drift?}` (a still, optionally with a slow drift). A new scene fades in over the old one (900 ms).
- Auth plays `city`; Play plays the chosen server's loop; Shop shows the chosen product's still; Profile and Settings show their stills.
- A loop plays from a blob URL: `loopBlob` fetches the whole file once, so the video seeks and loops without HTTP Range support from the host. The poster (the loop's first frame) shows until the video plays.
- `active` false pauses the video and the drift. Reduced motion, or a runtime where `canPlayLoops()` is false (Windows N without the Media Feature Pack), shows the poster instead of the video.

**Art mapping** (`src/media/catalog.ts`). The platform has no art fields yet, so these are the contract until it does:
- `serverArt(server, index)` → `{clip}`: keyword rules over the name and tags, most specific first (harbour/port/docks/marina/beach/coast → `harbor`; vinewood/hills/boulevard → `vinewood` (a penthouse over the city at sunset); race/racing/drift/drag/street/night/police/cops → `night` (tuned cars past a neon club); free roam/sandbox/county/desert/wild → `wild`; downtown/city/los santos → `city`), otherwise a rotation by list position (`city`, `harbor`, `wild`, `night`, `vinewood`). The server card shows `clipThumbUrl(clip)`, frame 0 of the same loop.
- `productStill(product)` → still: vehicle item → `vehicle`, outfit item → `outfit`, any other item → `item` (light-strip trim on bikes and suits), VIP → `vip`, coins and anything else → `coins`.
- Suggested API addition: an optional `art` field on servers (a clip or still name, or an asset path) and use of the existing product `imageUrl`; the maps above stay as the fallback.

**Emblem** (`src/three/EmblemLayer.tsx`, `Emblem.tsx`, `env.ts`): the only real-time 3D. One small transparent canvas, positioned over the mounted `EmblemSlot` (it re-measures on any resize of the slot or its ancestors), never takes pointer events.
- The mark's oblique plate is extruded from the brand polygons (`src/brand/mark.ts`) with a 45° chamfer in polished chrome, the G cut clean through and a blue enamel back plate showing in the cut. It is turned 0.36 rad toward the copy beside it, sways a little and leans toward the pointer.
- Lighting is an environment of studio strips (drei `<Lightformer>`s rendered into a side scene, assigned explicitly through `env.ts`): a cool softbox above a dark horizon and a dim warm one below it at the face's mirror direction, thin strips along the chamfers, a warm low bounce. No HDRI, no lights.
- `charge` (0..1, driven by GSAP) brightens the enamel: a short pulse when a new server is picked, a full charge at PLAY.
- `frameloop` is `always` while a slot is shown and `active`, `demand` under reduced motion (a still emblem), otherwise `never`.

## 9. Demo mode

Outside SolarLauncher.exe (`chrome.webview` absent) the UI runs on `createMockBridge()` and `createMockFetch()`. It is always labelled: "Demo data · not live" in the rail, "Demo" on counts, "Demo price · not for sale" on prices. Query parameters:

| Param | Values |
|---|---|
| `signedIn=1` | Start with a stored session. |
| `screen` | `shop`, `profile`, `settings` |
| `servers` | `ok`, `empty`, `maintenance`, `down` |
| `shop` | `ok`, `empty` |
| `account` | `ok`, `unverified`, `banned` |
| `net` | `ok`, `offline`, `slow` |
| `launch` | A launch result code to fail with (default: success). |
| `stageMs` | Milliseconds between mock launch stages (default 900). |
| `game` | `none`: the mock bridge reports an empty `gamePath`, as SolarLauncher.exe does when no GTA V install was found. The default is a detected Rockstar Games path. |
| `deepLink` | A URL-encoded `gage-mp://auth/…` link delivered once after `settings`. |

Mock login: any username with a password of at least 8 characters. `wrong` fails the password; a `locked*` username gets a 429 with `Retry-After: 540`.

## 10. Assets

- Heavy assets are never inlined. They live in `public/assets/` (in dev, `/assets/…`) and in the exe at `https://app.gage/assets/…` (from `{app}\launcher-assets\`).
- `public/assets/video/`: 5 muted H.264 loops (`city`, `harbor`, `wild`, `night`, `vinewood`; each one continuous trailer shot held slow, 1600×900, 3–5.4 s, seamless, `+faststart`), each with a `.webp` poster and a 640×360 card thumbnail in `video/thumb/`. `public/assets/stills/`: 7 stills (1600×900 webp: `downtown` and `settings` backgrounds, five product stills `vehicle`, `vip`, `outfit`, `item`, `coins`) and the product card thumbnails in `stills/thumb/` (640×360). `src/media/catalog.ts` is the single list; `tests/media.test.ts` checks every listed file exists and that nothing unlisted ships.
- The imagery is official GTA V trailer footage and screenshots, © Rockstar Games, used at the owner's accepted risk. No Rockstar or GTA V logo and no Pricedown type appear on screen. Every file, its source URL and how it was cut are in `apps/launcher-ui/ASSETS.md`.
