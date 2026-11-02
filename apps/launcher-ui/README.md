# Solar Launcher UI

The desktop client's interface: a React 18 single-page app that SolarLauncher.exe hosts in a WebView2 window (1240×800 default, resizable). Art direction "Window on Los Santos": GTA V's own trailer footage and screenshots run full-bleed behind a sharp, minimal Solar RP interface. The only real-time 3D is the chrome G emblem (React Three Fiber), drawn on one small transparent canvas over the footage.

- Contract: `.impeccable/surfaces/apps-launcher-ui-src-app-tsx.md` at the repo root.
- Design system: `DESIGN.md`.
- Direction, references and shot list: `docs/DIRECTION.md`, `docs/SHOTS.md`.
- Footage and stills (every file, its source and the owner-accepted risk): `ASSETS.md`.

## Commands (Node 22)

```bash
npm install
npm run dev         # http://127.0.0.1:5317/ in demo mode (mock bridge + mock API)
npm run typecheck   # tsc --noEmit
npm test            # vitest: tests/**/*.test.ts
npm run build       # asset manifest -> typecheck -> vite build
npm run preview     # serves dist/ at http://127.0.0.1:5318/
```

`npm run build` produces:

- `dist/index.html`: one self-contained file with JS, CSS and fonts inlined. The native build embeds it.
- `dist/assets/**`: the background loops (`video/*.mp4`, each with a `.webp` poster and a card thumbnail in `video/thumb/`), the stills (`stills/*.webp`, product card thumbnails in `stills/thumb/`) and `manifest.json`, copied from `public/assets/`. They are never inlined; the exe serves them from `https://app.gage/assets/`.

## Background media

- `src/media/catalog.ts` maps each server to a loop (`serverArt`: by name or tag, otherwise a stable rotation; its card shows `clipThumbUrl`, a frame of the same shot) and each product to a still (`productStill`). The API has no art fields yet; when it gets them, they take over from these maps.
- A loop plays from a blob URL (`loopBlob` fetches the whole file once), so it seeks and loops without HTTP Range support from the host.
- The loop pauses whenever `renderActive` is false (hidden, unfocused and idle, in game) and keeps playing during a launch, which is set over the same shot. It shows its poster under reduced motion or when the WebView2 runtime cannot decode H.264 (Windows N without the Media Feature Pack).

## Run it in the exe

Wiring the exe is the native side's job. `NATIVE-INTEGRATION.md` is the complete contract:

- URL to file mapping under `https://app.gage/` (including the `.mp4` MIME type);
- every bridge message in each direction, with JSON shapes;
- window controls and the drag region;
- render pause;
- join token at injection;
- `gage-mp://` deep links;
- installer entries.

## Demo mode

In a plain browser, where `chrome.webview` is absent, the UI runs on a mock bridge and mock API. Everything mock is labelled "Demo" on screen. Query parameters such as `?signedIn=1&screen=shop`, `servers=maintenance` and `launch=1` are listed in `docs/api/launcher-ui.md` §9. `update=pending|downloading|downloading-unknown|verifying|installing|deferred|error|run|noupdate|server-behind` (with `progress=0.42`) previews the forced-update popup: see `docs/UPDATE-POPUP-REVIEW.md`.

## Layout

```
src/
  App.tsx                shell: backdrop loop, scrim, rail, screen, emblem layer, launch overlay, PLAY move
  media/                 catalog (loop and still names, URLs, server and product mapping), Backdrop (loop/still layers, blob loader)
  audio/                 the PLAY launch sound
  screens/               Auth, Play (+ server list), Shop (showroom, orders, owned), Profile, Settings, LaunchOverlay, UpdatePopup (the forced-update modal)
  state/                 AppState (context), auth / launch / shop / updatePopup reducers and helpers
  lib/                   bridge (+ mock), platform API client (+ mock), zod schemas, errors, deep links
  three/                 EmblemLayer (the one canvas), Emblem, env
  ui/                    primitives (Plate, Chip, Field, Toggle, Icon), Rail, emblem slot, hooks, Lenis
  styles/                tokens, base, ui, screens
  brand/, fonts/         brand mark and lockup (copied from brand/; still the old G art), Sofia Sans woff2 subsets
public/assets/           video/ and stills/ (sources: ASSETS.md)
scripts/asset-manifest.mjs
tests/                   api, bridge, links, media, mock-api, state, update-popup
```

API reference for developers: `docs/api/launcher-ui.md` at the repo root.
