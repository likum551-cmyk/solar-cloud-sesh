# SOLAR RP, NOON recolour

Same shapes, same letterforms, same layouts as brand kit v1. Only fills changed. A check script compares every SVG against the original with the fill attributes removed, and the geometry is identical.

## Colour roles

| Role | Colour | Where it is used |
|---|---|---|
| Ground | `#0a0907` | background of the "full colour" plates and the ground icon tile |
| Ink | `#f7f2e8` | letters, the lit hook, the whole mark in the ink-only variant |
| Solar gold | `#f2b134` | the shadow hook (the one gold element per lockup), the gold icon tile, the gold plate |
| Dark on gold | `#1a1206` | the single-colour dark variant, the S on the gold icon tile |

Hover `#ffc75a` and pressed `#c98d16` are interface states. No mark uses them.

## Files

Lockups are `solar-rp-{horizontal|stacked|wordmark|mark}-noon-{full|ink|dark}` as SVG and as transparent PNG. The bounds are tight, so add clear space yourself (half the cap height, 50 units).

- `full`: ink plus one gold shadow hook, for `#0a0907`. The wordmark has no accent, so its `full` and `ink` files are the same drawing.
- `ink`: everything `#f7f2e8`, for use over photos and video.
- `dark`: everything `#1a1206`, for use on gold `#f2b134` or on other light backgrounds.
- PNG widths: horizontal and wordmark 1720, stacked 1290, mark 400 x 800.

`plates/` has full colour on `#0a0907` and dark on gold `#f2b134`, with clear space included. Plates are opaque by nature; the lockup PNGs are the transparent ones.

`icons/` has the 16, 32 and 256 px icons as SVG and transparent PNG, in two treatments, plus `solar-rp-noon-ground.ico` and `solar-rp-noon-gold.ico`, each holding 16, 32 and 256.

- `ground`: `#0a0907` tile, ink and gold S. At 16 px the S is one solid shape with no cut, so it cannot be split in two colours and is all gold.
- `gold`: `#f2b134` tile, `#1a1206` S. This is the one to ship as the favicon. The ground tile is the same colour as a dark tab strip or taskbar, so its cut corners disappear there.

`noon-overview.png` shows everything. `noon-legibility.png` is the warm-background test.

## Legibility over bright sunset footage

The ink-only mark does not stay legible over bright warm sky. Colour alone cannot fix it while the mark is `#f7f2e8`, because that is a near-white.

| Background | Ink `#f7f2e8` | Dark `#1a1206` | Gold `#f2b134` |
|---|---|---|---|
| pale gold `#ffe2b0` | 1.12 | 14.79 | 1.51 |
| sun glow `#ffd18a` | 1.28 | 13.01 | 1.33 |
| peach `#ffb774` | 1.53 | 10.84 | 1.10 |
| orange `#ff9a4d` | 1.89 | 8.81 | 1.11 |
| coral `#f2754a` | 2.54 | 6.55 | 1.50 |

WCAG ratios; 3:1 is the bar for graphics, though logos are formally exempt. The full-colour gold hook is also invisible on those backgrounds, and the dark variant disappears on dark areas (1.04:1 on the skyline stand-in). So no single flat colour works across footage that has both bright sky and dark buildings.

What works:

1. **Ink-only on a flat scrim.** A rectangle of `#0a0907` at 65 percent alpha behind the lockup, with clear space as padding. Measured against the test scene the worst mark pixel is 6.9:1, and 4.5:1 holds on every swatch and on the brightest pixel (the sun disc). The most alpha any swatch needs for 4.5:1 is 0.54 (pale gold), and the sun-disc pixel needs 0.58, so 0.65 leaves margin.
2. **Dark `#1a1206` variant**, only where the footage behind the mark is reliably bright.
3. **An opaque ground plate** (`plates/…-full-on-ground`) when you want the gold hook to show.

The test scene is a procedural stand-in (sky gradient, sun, dark skyline), not GTA V footage. The swatch numbers do not depend on it. Please check the scrim on real footage before fixing the alpha.

## Not changed

`tokens.json`, the Design canvas, the launcher and in-game mockups, shirt decals and installer images still use v1 cobalt. This kit recolours the logo, wordmark, monogram and icon only.
