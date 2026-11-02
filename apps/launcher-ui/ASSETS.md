# Launcher UI assets

Everything the client loads at run time lives in `public/assets/`. Vite copies it to `dist/assets/`; the installer ships it as `{app}\launcher-assets\`; the exe serves it at `https://app.gage/assets/...` (see `NATIVE-INTEGRATION.md`). `scripts/asset-manifest.mjs` writes `public/assets/manifest.json` (path, bytes, SHA-256, MIME type) on every `npm run build`; the exe's allow-list comes from that file. `src/media/catalog.ts` is the one place that names these files, and `tests/media.test.ts` fails if a file it names is missing or if a file ships that it does not name.

**GTA V imagery, owner-accepted risk.** Every loop and still below is cut from official Grand Theft Auto V and GTA Online trailers and Steam store screenshots published by Rockstar Games. The owner accepted the IP risk of shipping them (2026-10-29). Rules kept while cutting them:

- no Rockstar Games or GTA V logo, no Pricedown, no overlay UI, watermark or burned-in caption on any frame (every frame was checked at 1:1; in-world signs such as shop names and a police livery stay);
- each loop is ONE continuous shot, held: muted 1600x900 H.264, 3.7-5.4 s, seamless, under 3.4 MB (largest 3,324,769 bytes);
- per-server and per-product art is picked from these files by name, tags or grant (`serverArt`, `productStill` in `src/media/catalog.ts`) until the platform API has art fields. Demo items stay labelled as demo on screen.

Credits shown in the client (Settings, end of the panel): "Solar RP is not affiliated with Rockstar Games or Take-Two Interactive.", "Background footage and stills: official Grand Theft Auto V trailers and screenshots, © Rockstar Games.", and Sofia Sans (OFL).

## Shipped files

Total under `public/assets/`: 27 files and `manifest.json`, 12,479,136 bytes (from `manifest.json`; budget 60 MB).

### Loops (`video/`, `video/thumb/`)

Each loop has three files: the MP4, its poster (frame 0, 1600x900 WebP, shown until the video plays and under reduced motion) and a 640x360 card thumbnail of the same frame, so a server card shows the shot its server plays.

| Clip | Files and bytes | The shot | Cut (source, in-out, slow-down) |
|---|---|---|---|
| `city` | `city.mp4` 3,324,769<br>`city.webp` 81,826<br>`thumb/city.webp` 22,590 | Downtown Los Santos towers in golden haze above the Rockford Hills houses; the camera drifts. 5.37 s. | CE 2.65-5.45 s, 2.2x |
| `harbor` | `harbor.mp4` 956,452<br>`harbor.webp` 44,604<br>`thumb/harbor.webp` 13,048 | Refinery tanks and the port bridges of Elysian Island at sunset, smoke rising. 3.7 s. | AN 38.5-40.3 s, 2.5x |
| `wild` | `wild.mp4` 1,650,327<br>`wild.webp` 38,362<br>`thumb/wild.webp` 12,250 | A crop-duster biplane low over the Grand Senora desert, into the sun. 3.83 s. | FO 20.55-22.4 s, 2.5x |
| `night` | `night.mp4` 3,268,674<br>`night.webp` 79,570<br>`thumb/night.webp` 24,880 | Tuned cars sliding past a neon-lit club front on a wet street at night. 4.45 s. | TU 11.3-13.4 s, 2.5x |
| `vinewood` | `vinewood.mp4` 2,098,396<br>`vinewood.webp` 48,010<br>`thumb/vinewood.webp` 16,156 | A penthouse over the city at sunset; a man in an orange jacket walks to the window, a helicopter passes. 4.2 s. | VN 60.9-63.4 s, 2.0x |

Source keys:

- AN: Grand Theft Auto V: PS4, Xbox One and PC announcement trailer (Rockstar Games), https://www.youtube.com/watch?v=3DBrG2YjqQA
- CE: GTA Online: The Criminal Enterprises trailer (Steam store, GTA V Enhanced), https://store.steampowered.com/app/3240220/ (trailer 257107461, Criminal Enterprises)
- FO: Grand Theft Auto V and GTA Online: coming to new generation consoles trailer (Rockstar Games), https://www.youtube.com/watch?v=foUaOCzfIRU
- TU: GTA Online: Los Santos Tuners trailer (Steam store, GTA V Enhanced), https://store.steampowered.com/app/3240220/ (trailer 257107463, Los Santos Tuners)
- VN: Grand Theft Auto V: PS5 and Xbox Series X|S trailer (Rockstar Games), https://www.youtube.com/watch?v=VNbONMSObfs

### Stills (`stills/`, `stills/thumb/`)

`downtown` (profile) and `settings` are backgrounds only. The five product stills also have a 640x360 card thumbnail.

| File | Bytes | What it is | Source |
|---|---:|---|---|
| `stills/downtown.webp` | 151,222 | Downtown Los Santos skyline at dusk over the freeway. Profile background. | https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/3240220/d61184a98c1cf2db2b08b2999c04b0519e3615bb/ss_d61184a98c1cf2db2b08b2999c04b0519e3615bb.1920x1080.jpg?t=1781187782 |
| `stills/settings.webp` | 95,688 | The Downtown freeway interchange at dusk, a helicopter beside the towers. Settings background. | https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/271590/ss_4627498ce6ec1516512468eecfb9f90b7fdf438b.1920x1080.jpg?t=1765387725 |
| `stills/vehicle.webp`<br>`stills/thumb/vehicle.webp` | 73,860<br>17,082 | A red supercar at dusk under power lines. | https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/271590/ss_80b965d66b13d6eb5e1468151a371e12fe159663.1920x1080.jpg?t=1765387725 |
| `stills/vip.webp`<br>`stills/thumb/vip.webp` | 63,010<br>20,870 | A members' lounge party in the same penthouse at dusk, arcade cabinets along the window. Frame 57.6 s of VN, cropped to 16:9 (`crop=1536:864:192:108`), 1600x900 WebP q80. | VN (above) |
| `stills/outfit.webp`<br>`stills/thumb/outfit.webp` | 102,142<br>21,116 | A man in a work jacket and white T-shirt in an underground bunker, beside a utility cart. | https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/271590/ss_108d9c5f4244a6186662850a56898028913d28a4.1920x1080.jpg?t=1765387725 |
| `stills/item.webp`<br>`stills/thumb/item.webp` | 92,100<br>27,846 | Riders in light-strip suits beside cyan and orange light-trimmed bikes on the Del Perro Pier at night. Shown for any item that is not a vehicle or an outfit (the demo "Glow Trim Kit"). | https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/271590/ss_3a20a814df620793186348058d9201f1c60b1bc2.1920x1080.jpg?t=1765387725 |
| `stills/coins.webp`<br>`stills/thumb/coins.webp` | 108,822<br>25,464 | An office desk with stacks of cash and ledgers. | https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/271590/ss_1a30befe6a3ed468981a6b2aaed7d2bb79bc1ce0.1920x1080.jpg?t=1765387725 |

### Bundled into `index.html`

| File | Source | Licence |
|---|---|---|
| `src/fonts/*` | Sofia Sans and Sofia Sans Extra Condensed (Latin and Cyrillic subsets) | SIL OFL 1.1 |
| `src/brand/*` | Brand mark and lockup (still the old GAGE-MP G art), copied from `brand/` (the sign-in screen shows the lockup's wordmark only, beside the chrome emblem) | Project-owned |

The chrome G emblem is the only real-time 3D. It is built in code (`src/three/Emblem.tsx`): extruded and bevelled from the brand mark outline (the G), lit by an environment of studio light strips (`src/three/EmblemLayer.tsx`), with no model, texture or HDRI file. The PLAY cue is synthesised with WebAudio (`src/audio/launch.ts`); no audio file ships.

## How the files were made

The tools are one-off scripts kept outside the repo (they need ffmpeg and sharp, which are not dependencies of this package).

- **Downloads.** YouTube trailers with yt-dlp, the Steam trailers and screenshots from the store page's public CDN, all into a scratch folder outside the repo.
- **Loops (held single shots).**
  - Shot boundaries were found with ffmpeg's scene score (`select='gt(scene,0.25)'`), and each candidate shot was checked as a strip of frames every 0.5 s; every in/out point sits inside one continuous shot, so a loop never cuts.
  - Letterboxed sources cropped to 16:9 (`crop=1536:864:192:108`), scaled to 1600x900 (lanczos), slowed 2-2.5x (`setpts`) and re-timed to 24 fps with motion-compensated interpolation (`minterpolate=fps=24:mi_mode=mci:mc_mode=aobmc:me_mode=bidir:vsbmc=1`), so the shot reads as a held, slow camera move.
  - The loop closes on itself: the last 0.8 s dissolves into the first (`xfade`), so frame N-1 flows into frame 0.
  - Encode: `libx264 -profile:v high -level 4.1 -preset slow -tune film -crf 20 -maxrate 5M -bufsize 10M -g 48 -sc_threshold 0 -pix_fmt yuv420p -movflags +faststart -an`.
  - Poster and card thumbnail: frame 0 as WebP q80 (1600x900 and 640x360).
- **Stills.** 1920x1080 screenshots resized to 1600x900 WebP q80 (backgrounds) and 640x360 WebP q78 (card thumbnails) with sharp; `vip` is a trailer frame (above).

## Playback contract

The host serves whole files with no Range support. A `<video>` over such a response is not seekable, so `src/media/Backdrop.tsx` fetches each loop once into a Blob and plays it from a `blob:` URL, which always seeks and loops. The video pauses whenever `renderActive` says so (hidden, idle and unfocused, in game); it keeps playing through a launch, because the launch is set over the same shot. Under prefers-reduced-motion, and on runtimes without an H.264 decoder (Windows N without the Media Feature Pack, open-source Chromium builds), the poster stands in for the loop.

## Retired assets

Moved, not deleted, to `H:\Users\Administrator\Downloads\gage new\launcher-3d-retired-2026-10-29\`:

- `footage-round2\`: replaced after the second review. `video\night.*` and `video\thumb\night.webp` (a police cruiser at night, AN 58.25-59.5 s: too dark and soft to read at window size); `stills\vip.webp` (the penthouse terrace, VN 63.6 s, now the Vinewood loop's shot) and `stills\watch.webp` (Vinewood Boulevard by day, `ss_95a1f04e...`: it did not show the product), each with its `thumb\`. The old Vinewood loop (TU) is now the Night Racing loop.

- `footage-round1\`: the first footage round, replaced on 2026-10-29 by the held single shots above. `video\` holds the five montage loops (`city`, `harbor`, `wild`, `night`, `sunset`; 4-5 shots each, 7.4-11.5 s) and their posters; `stills\` holds the stills no screen uses any more (`freeroam`, `harbor`, `profile`, `racing`, `vinewood`, the previous `settings`, `vip` and `watch`) and `stills\thumb\` the previous card thumbnails (server cards now show clip thumbnails; `vip` and `watch` have new ones). Their sources were the AN, CE, FO, TU and VN trailers above and the Steam screenshots `ss_6efecf50...`, `ss_43e9c59d...`, `ss_8340fd39...`, `ss_bb2ee3b9...`, `ss_04c349fc...`, `ss_60690d0f...`, `ss_3ce5439c...` and `ss_ea78dfa1...`.
- `public-assets\`: the real-time street scene, character, car, props, HDRI and textures of the design before the footage (26 files: `models/` 9 GLB, `hdri/`, `textures/`); `ASSETS-retired.md` in that folder lists their sources and licences. The source files that loaded them (`Stage.tsx`, `Street.tsx`, `Showroom.tsx`, `prints.ts`, `assets.ts`, `paint.ts` from `src/three/`, and `src/audio/shutter.ts`) were moved to `docs/archive/launcher-3d-street/` at the repo root; its README says how to bring them back.
