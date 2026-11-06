# Launcher UI assets

Everything the client loads at run time lives in `public/assets/`. Vite copies it to `dist/assets/`; the installer ships it as `{app}\launcher-assets\`; the exe serves it at `https://app.gage/assets/...` (see `NATIVE-INTEGRATION.md`). `scripts/asset-manifest.mjs` writes `public/assets/manifest.json` (path, bytes, SHA-256, MIME type) on every `npm run build`; the exe's allow-list comes from that file. `src/media/catalog.ts` is the one place that names these files, and `tests/media.test.ts` fails if a file it names is missing or if a file ships that it does not name.

**GTA V imagery, owner-accepted risk.** Every clip and still below is GTA V footage: the stills from official trailers and Steam store screenshots published by Rockstar Games, the clips from the in-game footage the owner supplied on 2026-11-06 (`fresh_videos/` at the repo root, never committed). The owner accepted the IP risk of shipping them (2026-10-29). Rules kept while cutting them:

- no Rockstar Games or GTA V logo, no Pricedown, no overlay UI, watermark or burned-in caption on any frame (every frame was checked at 1:1; in-world signs such as shop names, billboards and a police livery stay);
- each clip is ONE continuous shot: muted 1600x900 VP9 WebM, 12 s, under 3.5 MB (largest 3,428,736 bytes). A screen plays two or three clips as a reel that passes from one shot to the next through the ground (see Playback contract), so no clip has to loop on itself;
- per-server and per-product art is picked from these files by name, tags or grant (`serverArt`, `productStill` in `src/media/catalog.ts`) until the platform API has art fields. Demo items stay labelled as demo on screen.

Credits shown in the client (Settings, end of the panel): "Solar RP is not affiliated with Rockstar Games or Take-Two Interactive.", "Background footage and stills: official Grand Theft Auto V trailers and screenshots, © Rockstar Games.", and Sofia Sans (OFL).

## Shipped files

Total under `public/assets/`: 36 files and `manifest.json`, 16,116,501 bytes in `manifest.json`'s `totalBytes` (16,123,459 with the manifest itself; budget 60 MB). The clip set alone (8 WebM, 8 posters, 8 thumbnails) is 15,317,279 bytes.

### Clips (`video/`, `video/thumb/`)

Each clip has three files: the WebM, its poster (frame 0, 1600x900 WebP q82, shown until the video plays and under reduced motion) and a 640x360 card thumbnail (frame 180, 7.5 s in, WebP q80), so a server card shows the shot its server plays. Sources are the owner's `fresh_videos/fon_<m>m<s>.webm` (1920x1080 AV1, 15 s each; the name is the clip's place in the owner's source recording).

| Clip | Files and bytes | The shot | Source, in-point |
|---|---|---|---|
| `skyline` | `skyline.webm` 671,206<br>`skyline.webp` 72,616<br>`thumb/skyline.webp` 19,878 | Downtown towers at dusk against the hills, the city lights coming on. Sign-in, Quests (lead) and the Play fallback. | `fon_25m38`, 0.2 s |
| `vinewood` | `vinewood.webm` 756,305<br>`vinewood.webp` 58,104<br>`thumb/vinewood.webp` 17,750 | Through the legs of the Vinewood sign to the Downtown towers in pink haze. | `fon_0m00`, 2.2 s |
| `beach` | `beach.webm` 1,745,771<br>`beach.webp` 88,408<br>`thumb/beach.webp` 25,448 | Vespucci Beach at sunset: palms, the boardwalk and the Del Perro Pier wheel. SOL, and Quests' second shot. | `fon_4m05`, 0.2 s |
| `canal` | `canal.webm` 3,428,736<br>`canal.webp` 102,288<br>`thumb/canal.webp` 26,334 | Down the Los Santos River canal to Downtown at golden hour. Rotation. | `fon_1m46`, 0.2 s |
| `boulevard` | `boulevard.webm` 1,435,303<br>`boulevard.webp` 101,548<br>`thumb/boulevard.webp` 32,044 | Down Vinewood Boulevard by day between the theatres and billboards. Roleplay servers and rotation. | `fon_40m04`, 0.2 s |
| `streets` | `streets.webm` 2,204,331<br>`streets.webp` 282,924<br>`thumb/streets.webp` 62,056 | Straight down on a city block and its intersections from above. TERRA. | `fon_29m37`, 0.2 s |
| `pool` | `pool.webm` 2,898,316<br>`pool.webp` 93,468<br>`thumb/pool.webp` 20,484 | Straight down on a lit oval pool at night, the water moving. | `fon_2m58`, 0.2 s |
| `observatory` | `observatory.webm` 1,074,187<br>`observatory.webp` 78,136<br>`thumb/observatory.webp` 21,638 | The Galileo Observatory at night, the city lit behind it. LUNA. | `fon_20m27`, 0.2 s |

Source keys (stills):

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
- **Clips (2026-11-06, from `fresh_videos/`).** ffmpeg n8.1.3 (`.toolchain/ffmpeg/`).
  - Each source is one continuous 15 s shot. 12 s are cut from the in-point above (`fon_0m00` starts at 2.2 s, past its opening frames), scaled to 1600x900 (`scale=1600:900:flags=lanczos,format=yuv420p`).
  - Encode, two-pass VP9: `libvpx-vp9 -b:v 0 -crf 37 -g 150 -row-mt 1 -tile-columns 2 -auto-alt-ref 1 -lag-in-frames 25 -deadline good` (`-cpu-used 4` first pass, `1` second), bt709 tags, `-an`. CRF 37 was picked against 1:1 crops of the busiest shots (`streets`, `pool`, `canal`) next to the source.
  - Why VP9 and not the source's AV1 or H.264: Chromium decodes VP9 itself (libvpx, or the GPU), so it plays on Windows N without the Media Feature Pack, which H.264 needs. AV1 has no hardware decode on many older GPUs, and in Edge it can depend on the AV1 extension. Edge 156 headless reported VP9 `supported`, `smooth` and `powerEfficient` and played it.
  - Poster: frame 0 as WebP q82 (1600x900). Card thumbnail: frame 180 as WebP q80 (640x360).
- **Earlier loops (retired).** These were trailer shots slowed 2-2.5x, H.264, each looped on itself with a 0.8 s dissolve.
- **Stills.** 1920x1080 screenshots resized to 1600x900 WebP q80 (backgrounds) and 640x360 WebP q78 (card thumbnails) with sharp; `vip` is a trailer frame (above).

## Playback contract

The host serves whole files with no Range support. A `<video>` over such a response is not seekable, so `src/media/Backdrop.tsx` fetches each clip once into a Blob and plays it from a `blob:` URL, which always seeks.

**Reels.** A screen's scene names a lead clip. `reelFor` in `catalog.ts` returns that clip's curated reel, 2-3 shots paired by horizon and light:

| Lead | Reel |
|---|---|
| skyline (sign-in, Play fallback) | skyline, canal |
| Quests (its own, `QUESTS_REEL`) | skyline, beach |
| vinewood | vinewood, skyline, boulevard |
| beach (SOL) | beach, skyline |
| canal | canal, boulevard, skyline |
| boulevard | boulevard, canal, skyline |
| observatory (LUNA) | observatory, skyline |
| streets (TERRA) | streets, pool |
| pool | pool, canal, skyline |

A reel never borrows another server's signature shot (`SIGNATURE_CLIPS`: beach, observatory, streets), and `tests/media.test.ts` checks it. The rotation for unmatched servers (skyline, canal, vinewood, boulevard) never uses one either. `boulevard` is not SOL's, because a legible in-world "for lease" billboard stays in frame for the whole shot, right under the rail. Quests has its own reel (the `reel` field of the scene): the shared skyline reel puts the Vinewood letters' girders behind its title and the canal's haze over its list, and the boulevard's billboards stay in frame at any framing. It plays SOL's beach, the one screen besides Play that does: it pays for backing Solar RP, and no server is chosen on it. Quests frames its reel at `object-position: 85% 50%` (the `frame` field): the towers stand over the title, clear of the wide quest panel.

When a clip is `REEL_FADE_MS` (1500 ms) from its end, the reel passes to the next clip through Night Ground: the outgoing clip sinks to transparent over 700 ms (linear) while the next one, mounted on top, rises from 300 ms to 1400 ms (`--ease-in-out`), so the frame dips to about a quarter of its light, never to black (`ui.css` `.is-leaving` / `.is-reel`). Under a playing reel the layer hides its poster, so the ground shows in the trough, not the lead's first frame. Then the outgoing clip is dropped. After the last clip, the reel wraps to the first.
- No shot ever dissolves into itself, there is never a hard cut, and two shots are never both strong at once (no double exposure of two horizons).
- Only two `<video>` elements exist at once, and the next clip's Blob is fetched while the current one plays.
- If that fetch fails, the current clip loops on its own instead.

The video pauses whenever `renderActive` says so (hidden, idle and unfocused, in game). It keeps playing through a launch, because the launch is set over the same shot. Under prefers-reduced-motion, and on runtimes that cannot play VP9 WebM (`canPlayLoops`), the lead clip's poster stands in for the reel.

## Retired assets

- 2026-11-06: the five H.264 loops (`city`, `harbor`, `wild`, `night`, `vinewood`: `video/<name>.mp4`, `video/<name>.webp`, `video/thumb/<name>.webp`) were replaced by the eight clips above. They were removed from `public/assets/` and `dist/assets/`, and are kept in git history only (the commit before the replacement). Their sources were CE, AN, FO, TU and VN.

Earlier rounds were moved, not deleted, to `H:\Users\Administrator\Downloads\gage new\launcher-3d-retired-2026-10-29\`:

- `footage-round2\`: replaced after the second review. `video\night.*` and `video\thumb\night.webp` (a police cruiser at night, AN 58.25-59.5 s: too dark and soft to read at window size); `stills\vip.webp` (the penthouse terrace, VN 63.6 s, now the Vinewood loop's shot) and `stills\watch.webp` (Vinewood Boulevard by day, `ss_95a1f04e...`: it did not show the product), each with its `thumb\`. The old Vinewood loop (TU) is now the Night Racing loop.

- `footage-round1\`: the first footage round, replaced on 2026-10-29 by the held single shots above. `video\` holds the five montage loops (`city`, `harbor`, `wild`, `night`, `sunset`; 4-5 shots each, 7.4-11.5 s) and their posters; `stills\` holds the stills no screen uses any more (`freeroam`, `harbor`, `profile`, `racing`, `vinewood`, the previous `settings`, `vip` and `watch`) and `stills\thumb\` the previous card thumbnails (server cards now show clip thumbnails; `vip` and `watch` have new ones). Their sources were the AN, CE, FO, TU and VN trailers above and the Steam screenshots `ss_6efecf50...`, `ss_43e9c59d...`, `ss_8340fd39...`, `ss_bb2ee3b9...`, `ss_04c349fc...`, `ss_60690d0f...`, `ss_3ce5439c...` and `ss_ea78dfa1...`.
- `public-assets\`: the real-time street scene, character, car, props, HDRI and textures of the design before the footage (26 files: `models/` 9 GLB, `hdri/`, `textures/`); `ASSETS-retired.md` in that folder lists their sources and licences. The source files that loaded them (`Stage.tsx`, `Street.tsx`, `Showroom.tsx`, `prints.ts`, `assets.ts`, `paint.ts` from `src/three/`, and `src/audio/shutter.ts`) were moved to `docs/archive/launcher-3d-street/` at the repo root; its README says how to bring them back.
