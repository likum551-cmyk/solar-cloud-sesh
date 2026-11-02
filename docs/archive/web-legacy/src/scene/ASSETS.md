# Hero band: asset log

Every sourcing attempt for the Night Drive band (DIRECTION.md), in ladder order. Shipped files live in `src/assets/freeway/`. All of them are CC0. Each one was downloaded into its own scratch folder, converted by script, and looked at before it was kept. Sketchfab was searched (its search is public) but nothing was downloaded: downloads need an API token, which is not set, and every relevant hit (Washingtonia palms, sign gantries) was CC-BY or Free Standard anyway.

## Shipped

| File | Source | Licence | Conversion | Bytes |
|---|---|---|---|---|
| `palm.glb` | "Palm tree v2 low-poly edition", Nobiax, reduced by Julius. https://opengameart.org/content/palm-tree-v2-low-poly-edition | CC0. The readme asks for a link back to nobiax.deviantart.com, which is credited here. | OBJ and TGA converted to GLB (UV V flipped for glTF; the TGA rows were already top-down). Then WebP textures via gltf-transform. 451 tris, 512 px maps, alpha-masked double-sided fronds. | 194,940 |
| `asphalt-color/normal.webp` | Poly Haven `asphalt_track`, 1k. https://polyhaven.com/a/asphalt_track | CC0 | 1024 to 512, WebP. The road shader uses the colour map's luminance and the normal map's tilt (to break up the wet reflections). | 70,120 |
| `zinc-color/rough.webp` | ambientCG `Metal038`, 1K. https://ambientcg.com/a/Metal038 | CC0 | 1024 to 512, WebP. Mapped triplanar in world space on all steel. | 17,782 |
| `concrete-color.webp` | ambientCG `Concrete034`, 1K. https://ambientcg.com/a/Concrete034 | CC0 | 1024x512 to 512x256, WebP. Triplanar on the barrier and footings. | 5,972 |
| `facade-office-color/emit.webp` | ambientCG `Facade016` (night office grid with an emission map). https://ambientcg.com/a/Facade016 | CC0 | 1024 to 512, WebP | 114,866 |
| `facade-glass-color/emit.webp` | ambientCG `Facade009` (night glass). https://ambientcg.com/a/Facade009 | CC0 | 1024 to 512, WebP | 55,792 |
| `cars.glb` | Kenney "Car Kit": `sedan`, `suv`, `delivery`. https://kenney.nl/assets/car-kit | CC0 | Geometry only: the three models merged into one GLB with one mesh each (flatten, join, weld, dedup, prune); textures, UVs and materials dropped. 6,982 tris in total. The band scales each body to a real size (sedan 4.7 m, SUV 4.9 m, van 6.4 m long), instances it, and paints it from a dark night palette, so the toy colour map never shows. | 210,144 |
| `band-still.webp` | Rendered from this scene: the reduced-motion frame at 1680 x 380 CSS px (x1.5), with every exit plate and tab hidden. Used by the no-WebGL and lost-context fallback (Hero.jsx `FallbackBand`), which draws the sign with `drawSign` and pins it to the plate corners recorded in `STILL`. | Own render | Captured from the preview build with a Playwright script (stillcap.mjs: reduced motion, hide `g.mat` meshes and `g.tab`, project the near plate's corners, screenshot the canvas alone), then WebP q74. Re-render it, and update `STILL.quad`, whenever the camera, the gantry or the plate size changes. | 56,026 |
| `night-env.hdr` | Poly Haven `rooftop_night`, 1k. https://polyhaven.com/a/rooftop_night | CC0 | Box-filtered 1024x512 to 256x128, hot spots capped at 6 (hue kept), rewritten as flat RGBE. Used for reflections only, never shown, at environmentIntensity 0.45. Uncapped, its ~4000 floodlights turned the metal lamp poles into glowing white bars under bloom. | 131,121 |

## Log

| Rung | Need | Query or source | Candidates | Verdict | Reason |
|---|---|---|---|---|---|
| 1 | Palm | OpenGameArt, Poly Haven, ambientCG, Kenney, Quaternius | Nobiax palm v2 low-poly; Nobiax palm v2 (1k maps); Quaternius nature pack palms; Kenney nature kit palms; OGA "Low Spec Fan Palm" | **KEEP** Nobiax low-poly. REJECT the rest. | Nobiax: the silhouette reads as a palm instantly (vet/palm-b/turntable), with real bark and alpha fronds. It is a coconut palm, not a Washingtonia, but at night it is a silhouette against the sky glow. Quaternius and Kenney: flat-colour toy blades. OGA fan palm: a 42-tri shrub. Poly Haven and ambientCG have no palms. |
| 2 | Palm (Washingtonia) | Sketchfab search | Washingtonia Robusta Palm_01 (CC-BY) and others | Not used | No token, and every hit was CC-BY. Logged for the owner: https://sketchfab.com/3d-models/washingtonia-robusta-palm-01-e59d8c7f054248fbbf32cfe4851b8911 |
| 1 | Overhead gantry | Kenney City Kit Roads (`sign-highway-detailed`) | 1 | REJECT | Toy proportions and flat colormap; the previous critic round failed exactly this look. |
| 2 | Overhead gantry | Sketchfab public search (no token needed to search), downloadable only: "sign gantry", "highway sign", "overhead highway sign", "highway sign gantry" | FIRESTONE ADVERTISING GANTRY (CC-BY, 16k faces); Lagos LED Gantry (CC-BY, 396 faces); Highway Sign (CC-BY, 177k faces); Highway Sign 0 (Free Standard); French Highway Sign - VASTIE (CC-BY); Bulgarian Highway Sign 1 (CC-BY) | Not used | None is a US overhead truss sign bridge: they are advertising frames, roadside post signs, or European signs. All are CC-BY or Free Standard, and downloads need the token anyway. |
| 3 | Overhead gantry | Hand-built (`geometry.js` buildGantryTruss) | | **Built** | No CC0 realistic gantry exists (paid CGTrader only). It keeps the real truss profile, with Metal038 zinc maps added. |
| 1 | Jersey barrier | Poly Haven `concrete_road_barrier`, `_02` | 2 | REJECT | One is checker-painted (80k tris); the other is a rebar block (43k tris). Neither is a Caltrans K-rail. |
| 3 | Jersey barrier, guardrail | Hand-built (`buildBarrier`, `buildGuardrail`) | | **Built** | Correct New Jersey profile, with Concrete034 maps added. |
| 1 | Freeway lamp (cobra head) | Poly Haven `street_lamp_01/02`; Kenney `light-curved` | 3 | REJECT | Victorian cast iron; a toy. |
| 3 | Freeway lamp | Hand-built (`buildLampPole`, `buildLampLens`) | | **Built** | The cobra-head arm profile. |
| 1 | Buildings | Kenney City Kit Commercial; ambientCG facades | Kenney kit; Facade016, 009, 012, 013, 014, 017 | **KEEP** Facade016 and 009 as textures on the town and skyline blocks. REJECT Kenney. | Real night photo facades with emission maps (lit windows); Kenney is cartoon. |
| 1 | Road | ambientCG Asphalt025C (wet); Poly Haven asphalt_track | 2 | **KEEP** asphalt_track | Fine dark grain that tiles at 512; the wetness comes from the road shader, so a wet-baked texture would double it. |
| 1 | Steel | ambientCG Metal038, Metal009 | 2 | **KEEP** Metal038 | Mottled dull steel, the nearest to galvanized; no spangled zinc texture exists as CC0. |
| 1 | Concrete | ambientCG Concrete034; Poly Haven concrete_wall_008 | 2 | **KEEP** Concrete034 | Pale Caltrans concrete. |
| 1 | Environment | Poly Haven rooftop_night, shanghai_bund, modern_buildings_night | 3 | **KEEP** rooftop_night | Sodium-lit rooftop under a dusk sky, a good reflection source; shanghai_bund would read as Shanghai if ever seen. |
| 1 | Traffic cars | Kenney Car Kit | sedan, suv, delivery, others in the kit | **KEEP** sedan, suv, delivery as geometry | Lights alone read as floating pairs once the camera rose to a real eye height (critic round 1). Kenney's shapes are simple, but under a dark paint, at 30 m and more and at night, they read as cars. Their toy colour maps are dropped. |
