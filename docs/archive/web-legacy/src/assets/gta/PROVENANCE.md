# Provenance: shipped GTA V artwork

All images here are official Grand Theft Auto V key art, (c) Rockstar Games / Take-Two Interactive. They are used by owner decision (2026-10-24, see PRODUCT.md "Asset boundary"), with an accepted takedown risk. This file is the removal list: delete these files and their imports in `src/components/gta.jsx` to strip the art.

Source copies were fetched on 2026-10-24 from the GTA Wiki (gta.fandom.com) image CDN, each 2880x1800. Crops are `[x, y, w, h]` in source pixels, chosen to stay clear of the GTA V logo (bottom left) and the Rockstar badge (bottom right). Covers and the backdrop were resized and encoded to WebP in headless Chromium. The full-window splashes (re-exported 2026-10-25) are cut at the source's own resolution and encoded once with sharp, so they stay sharp on 150% displays. The source files are themselves compressed, and that compression is the remaining limit on these splashes.

| File | Source artwork | Source URL | Crop | Output |
|---|---|---|---|---|
| cover-franklin-chop.webp | Artwork-FranklinAndChop-GTAV.jpg | https://static.wikia.nocookie.net/gtawiki/images/a/ac/Artwork-FranklinAndChop-GTAV.jpg | 1000,0,1600,800 | 960x480 q0.80 |
| cover-franklin-hills.webp | Artwork-FranklinVinewoodHills-GTAV.jpg | https://static.wikia.nocookie.net/gtawiki/images/3/3d/Artwork-FranklinVinewoodHills-GTAV.jpg | 970,60,1600,800 | 960x480 q0.80 |
| cover-franklin-bandana.webp | Artwork-FranklinBandana-GTAV.jpg | https://static.wikia.nocookie.net/gtawiki/images/4/46/Artwork-FranklinBandana-GTAV.jpg | 640,100,1600,800 | 960x480 q0.80 |
| cover-michael-city.webp | Artwork-Michael4-GTAV.jpg | https://static.wikia.nocookie.net/gtawiki/images/8/80/Artwork-Michael4-GTAV.jpg | 875,0,1600,800 | 960x480 q0.80 |
| cover-michael-vangelico.webp | Artwork-MichaelVangelico-GTAV.jpg | https://static.wikia.nocookie.net/gtawiki/images/d/dd/Artwork-MichaelVangelico-GTAV.jpg | 1100,30,1600,800 | 960x480 q0.80 |
| cover-trevor-atv.webp | Artwork-Trevor-GTAV.jpg | https://static.wikia.nocookie.net/gtawiki/images/7/7e/Artwork-Trevor-GTAV.jpg | 715,0,1600,800 | 960x480 q0.80 |
| cover-trevor-gas.webp | Artwork-TrevorGasoline-GTAV.jpg | https://static.wikia.nocookie.net/gtawiki/images/5/58/Artwork-TrevorGasoline-GTAV.jpg | 350,20,1600,800 | 960x480 q0.80 |
| cover-trevor-jack.webp | Artwork-TrevorYellowJack-GTAV.jpg | https://static.wikia.nocookie.net/gtawiki/images/7/77/Artwork-TrevorYellowJack-GTAV.jpg | 860,20,1600,800 | 960x480 q0.80 |
| splash-michael.webp | Artwork-Michael4-GTAV.jpg | https://static.wikia.nocookie.net/gtawiki/images/8/80/Artwork-Michael4-GTAV.jpg | 560,0,2320,1497 | 2320x1497 q88 (sharp, no resize) |
| splash-franklin.webp | Artwork-FranklinVinewoodHills-GTAV.jpg | https://static.wikia.nocookie.net/gtawiki/images/3/3d/Artwork-FranklinVinewoodHills-GTAV.jpg | 560,0,2320,1497 | 2320x1497 q88 (sharp, no resize) |
| splash-trevor.webp | Artwork-TrevorYellowJack-GTAV.jpg | https://static.wikia.nocookie.net/gtawiki/images/7/77/Artwork-TrevorYellowJack-GTAV.jpg | 560,0,2320,1497 | 2320x1497 q88 (sharp, no resize) |
| backdrop-city.webp | Artwork-Michael4-GTAV.jpg (skyline left of the figure) | https://static.wikia.nocookie.net/gtawiki/images/8/80/Artwork-Michael4-GTAV.jpg | 0,450,1150,750 | 1150x750 q0.82 |

## Lettering

`src/lib/pricedown.js` holds fixed strings in **Pricedown Bl** (Typodermic Fonts), outlined to SVG paths by a build-time tool. The font file is never shipped: its desktop licence allows outlined lettering as artwork, not embedding. To add a string, outline it with the same tool; never bundle the font.

The UI face, Overpass, ships under the SIL Open Font License (`../fonts/OFL-Overpass.txt`).
