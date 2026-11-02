# Website assets

| Asset | Source | Licence / basis | Verdict |
|---|---|---|---|
| 3D Solar RP S (hero subject) | Extruded at run time from the two hook polygons of the brand mark (`brand/solar/solar-rp-noon/solar-rp-mark-noon-full.svg`, 50x100 grid) | Our own brand art | KEEP. The subject is our logo, so no library or Sketchfab model can stand in for it (rungs 1-2 of the sourcing ladder do not apply); the geometry is the official vector, bevelled, not an approximation. |
| Footage loops `city`, `harbor`, `wild`, `night`, `vinewood` (+ posters) | `apps/launcher-ui/public/assets/video` (cut from official GTA V / GTA Online trailers) | Owner-accepted IP risk, see `apps/launcher-ui/ASSETS.md` | KEEP. 1600x900; soft above 1600px, so they sit letterboxed or under scrims. |
| Key art `michael`, `franklin`, `trevor` | `docs/archive/web-legacy/src/assets/gta/splash-*.webp` (official GTA V artwork, 2320x1497) | Owner-accepted IP risk, see `PROVENANCE.md` there | KEEP. Crops clear of the GTA V logo and Rockstar badge. |
| Stills `vehicle`, `downtown`, `dusk` | launcher `stills/` (Steam store screenshots) | as above | KEEP |
| Launcher screenshots | Captured from the real launcher build (`apps/launcher-ui/dist`, demo data, labelled on screen) | Our own UI | KEEP |
| Fonts | Sofia Sans / Sofia Sans Extra Condensed woff2 from `apps/launcher-ui/src/fonts` | SIL OFL | KEEP |
| Environment light for the S | Per stage: `envScene(poster)` in `src/mark3d.ts`. The stage's own poster (`/media/city.webp` for the hero, `/media/vinewood.webp` for the close) is mapped on a sphere, with a bright band above the horizon and one strip light added, then run through PMREM and cached per look. Each look also sets fog, key and rim from that shot (`LOOKS` in `src/main.ts`). | the site's own footage + own code | KEEP. Round 1 history: the bare poster reflected as one flat tone (the faces read as printed); the procedural `horizonEnv()` gave gold a gradient but did not sit in the shot (critic: "WordArt"). Poster + band + strip keeps both the shot's colour and a readable highlight. |

`scripts/media.mjs` copies footage and key art into `public/media` at build time, so the repo keeps one copy of each file. The page credits: "Solar RP is not affiliated with Rockstar Games or Take-Two Interactive. Footage and artwork: Grand Theft Auto V, (c) Rockstar Games."
