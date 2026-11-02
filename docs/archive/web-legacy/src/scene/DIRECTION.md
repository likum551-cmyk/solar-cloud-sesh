# Hero band: direction

The 3D band across the top of the launcher (src/scene/, mounted by components/Hero.jsx). Rebuilt 2026-10-24 after the owner rejected the first band ("top banner is so shit the animation, fix it remake it"). The first band hovered almost still beside one gantry. Its only motion was light trails, and an art critic judged it placeholder-grade: box town, cone palms, untextured truss, no shadows or bloom.

## Bar: reference works

1. **GTA V night driving on the Del Perro / Olympic freeways (Rockstar, 2013; Enhanced 2022).** Sodium pools on wet asphalt, fan palms black against an orange sky glow, overhead guide signs lit from their catwalks, traffic reading mostly as lights. The palette and the furniture come from here.
2. **The title sequence of *Drive* (2011).** LA freeways at night shot on a long lens from inside the car: steady, unhurried forward motion, lamps sliding over the windscreen, the city as bokeh. The camera language comes from here: long focal length, constant speed, low, no shake.
3. **Slow Roads (slowroads.io) and the Forza Horizon 5 / NFS Heat menu backdrops.** Real-time endless driving that never visibly loops, with a bloom-led night look. These set the technical bar: seamless repetition, light that blooms instead of clipping, motion that never stops.

## Directions considered

| | Direction | Hero frame | Verdict |
|---|---|---|---|
| A | **Night Drive** | Low, long-lens camera rolling down the right carriageway. A green gantry sign carrying the selected server grows out of the distance and slides overhead. Fan palms stream past both verges. The wet road mirrors sodium lamps and tail lights. Downtown glows on the horizon. | **Chosen by the owner** (AskUserQuestion, 2026-10-24) |
| B | Animated loading screen | GTA key art in layered 2.5D parallax with a slow push, with the sign as a DOM plate | Rejected: no freeway motion |
| C | Skycam over Los Santos | A high orbit over a lit city grid that swoops to the sign | Rejected: needs a convincing city from above, the weakest asset position |

The owner's pick replaces the critic tournament, since the owner's choice outranks the critic's.

## Subject read test

The subject is a green freeway guide sign over lanes at night. The silhouette is a wide rectangle on a truss spanning the road, with palms either side. Anyone who has driven reads it in under a second, and GTA players read it as Los Santos. The plate stays the brightest, sharpest object in frame.

## Art rules

- The sign plate is the hero: highest contrast, never bloomed out (under the bloom threshold), legible at 1240x~330.
- Light carries the night: sodium orange, the cool sky, red tail lights, white headlights. Bloom applies to emitters only.
- Nothing hand-built as a primary subject where a sourced asset exists (see ASSETS.md).
- Motion never stops while the window is focused, and never shakes.
- prefers-reduced-motion gets one composed still: the sign at reading distance.
