# SOLAR RP HUD: Vitals + Minimap build spec (V2 Solar)

For a native game overlay with no CSS runtime. Everything below is derived from the design files (`HudSolar.dc.html`); the PNGs were rendered from that same markup.

## 0. Conventions (read first)

- Screen: 1920 x 1080. **Origin = bottom-left corner of the screen, x to the right, y UP.** All numbers are px at this size. For other resolutions scale everything by `screenHeight / 1080`.
- Boxes are `(x, y, w, h)` = bottom-left corner of the box + size. Circles are centre `(cx, cy)` + radius.
- Component box: `(56, 56, 288, 352)`: 56 px from the left edge, 56 px from the bottom edge. Its top-right corner is `(344, 408)`.
- Angles: degrees, 0 = +x (3 o'clock), positive = counter-clockwise (y-up). A point on a circle is `(cx + R*cos(a), cy + R*sin(a))`. If your API is y-down, use `y_down = 1080 - y` and `angle_down = -angle`.
- Colours are `#RRGGBBAA`, straight (non-premultiplied) alpha, sRGB.
- Blur is given as Gaussian sigma in px. CSS `filter: drop-shadow(0 0 Rpx)` and `box-shadow` blur R means sigma = R/2. CSS `blur(Rpx)` means sigma = R.
- Text: `x` = left edge of the text, `y` = baseline. Font Sofia Sans **Bold (700)** only, SIL OFL 1.1 (Google Fonts). `fonts/SofiaSans-Bold-latin.ttf` is a Latin-only conversion; download the full family if you need other scripts.
- Digits must be tabular (fixed width). Values are always `clamp(round(v), 0, 100)`.

## 1. PNG exports (in `png/`)

Every PNG is **288 x 352, 1:1, transparent RGBA**. Place its bottom-left corner at screen `(56, 56)`. They contain the HUD layer only: the backdrop blur is not baked in (nothing behind it). Glow tails are cut at the PNG edge (at most 12% alpha at the left edge).

| File | State |
|---|---|
| `hp100_sat64_map-on.png` / `hp100_sat64_map-off.png` | HP 100 (full), SAT 64 |
| `hp50_sat64_map-on.png` / `hp50_sat64_map-off.png` | HP 50, SAT 64 |
| `hp20_sat64_map-on.png` / `hp20_sat64_map-off.png` | HP 20 (low, under 25), SAT 64 |
| `hp08_sat64_pulse-hi_map-on.png` / `hp08_sat64_pulse-hi_map-off.png` | HP 8 (critical) pulse frame at opacity 1.0 |
| `hp08_sat64_pulse-lo_map-on.png` / `hp08_sat64_pulse-lo_map-off.png` | HP 8 (critical) pulse frame at opacity 0.4 |
| `hp00_sat64_map-on.png` / `hp00_sat64_map-off.png` | HP 0 (dead), SAT 64 |
| `hp82_sat100_map-on.png` / `hp82_sat100_map-off.png` | SAT 100 (full), HP 82 |
| `hp82_sat20_map-on.png` / `hp82_sat20_map-off.png` | SAT 20 (low), HP 82 |
| `hp82_sat00_map-on.png` / `hp82_sat00_map-off.png` | SAT 0 (empty), HP 82 |

`contact-sheet.png` shows all 18 on the sand scene colour `#D8C8A6`. "map-off" = map circle, its ring/glow, vignette and N label not drawn; the glass disc stays (assumption: the design has no dedicated no-map state).

## 2. Colour tokens

| Token | Value |
|---|---|
| ground | #0A0907FF |
| amber (HP, ring, route) | #F2B134FF |
| bone (SAT) | #D9D1C3FF |
| cream (numerals) | #F7F2E8FF |
| ember (low / critical / dead) | #E5603AFF |
| disc glass | #0A0907A8 |
| strip glass | #0A0907BD |
| arc track | #F7F2E829 |
| strip border (1 px, inside) | #F7F2E824 |
| strip divider | #F7F2E82E |
| amber map ring | #F2B134D9 |
| amber ring glow | #F2B1344D |
| HP glow | #F2B134CC |
| SAT glow | #D9D1C38C |
| low glow (ember) | #E5603AD9 |
| vignette rim | #0A09076B |

## 3. Layers, back to front (all coordinates screen, y-up)

**3.1 Glass disc.** circle centre `(200, 264)`, r 140 (box `(60, 124, 280, 280)`). Fill #0A0907A8. No border, no shadow. Optional backdrop blur sigma 14 behind it (not available in plain ImGui: raise the fill alpha to about 0.74-0.80 if the scene is bright).

**3.2 Arc tracks.** Ring centre `(200, 264)`, radius R = 130, stroke width 10 (covers r 125..135), **round caps** (cap radius 5), colour #F7F2E829. Each track sweeps 140 degrees.

- HP track (left): from a=250 to a=110. End-point centres: `(155.54, 141.84)` (bottom) and `(155.54, 386.16)` (top).
- SAT track (right, mirror of HP across x = 200): from a=-70 to a=70. End-point centres: `(244.46, 141.84)` (bottom) and `(244.46, 386.16)` (top).

**3.3 Arc fills** (same radius, width 10, round caps). Both fill **bottom to top**.

- HP: angle at value v = `250 - 1.4*v` (sweeps clockwise on screen from the bottom end, full = a 110).
- SAT: angle at value v = `-70 + 1.4*v` (sweeps counter-clockwise on screen from the bottom end, full = a 70).
- v = 0: draw nothing (no cap dot, no glow).
- Glow: draw the same arc blurred (Gaussian sigma 4.5, i.e. CSS 9 px) in the glow colour underneath the crisp arc. With no blur available: re-stroke the arc under it with width 14 at 28% of the glow alpha and width 22 at 12%.
- Colours: HP #F2B134FF, glow #F2B134CC. SAT #D9D1C3FF, glow #D9D1C38C. If v <= 25 either gauge uses ember #E5603AFF with glow #E5603AD9.

**3.4 Minimap** (hidden in the "map off" state). Circle centre `(200, 264)`, r 120 (box `(80, 144, 240, 240)`); the live minimap texture is drawn into this 240 x 240 box and clipped to the circle. Then, inside the clip, a **vignette**: radial gradient centred on `(200, 264)`, stop 1 at r 0..93.6 (78% of r) = #0A090700, stop 2 at r 120 = #0A09076B, linear between.
Then outside the circle: **ring** = 1.5 px band from r 120 to r 121.5, colour #F2B134D9; **ring glow** = the r 120 circle blurred with sigma 11 (CSS 22 px) in #F2B1344D, drawn only outside r 121.5.
Placeholder art in the PNGs is drawn in a 216-unit design grid (origin top-left, y down) and scaled by 240/216 = 1.11111 into the box: `X = 80 + u*1.11111`, `Y = 384 - v*1.11111`. Real game draws its own minimap, but these overlay marks should be kept:

| Mark | Design units (u, v) | Spec |
|---|---|---|
| player halo | (108,108) r 12 | fill #F2B13438 |
| player dot | (108,108) r 6 | fill #F2B134FF, stroke 2 #F7F2E8FF |
| view cone | triangle (108,108) (76,30) (140,30) | fill #F2B13424 |
| route | polyline (108,108) (108,60) curve via (108,36) to (132,36) then (160,36) | stroke 4 #F2B134FF, round caps and joins, glow sigma 2 #F2B13499 |
| destination | (160,36) r 7 and r 2.5 | r7: fill #0A0907FF stroke 2 #F2B134FF; r2.5 fill #F2B134FF |
| other blips | (62,92) and (150,160), r 4 | fill #F7F2E8E6 |
| N label | centre-x u 108, baseline v 22 | Sofia Sans 700, 11 units = 12.22 px, #F7F2E8CC |

Placeholder base colours (not needed in game): background #12110EFF, water #0F171BFF, park #151A13FF, blocks #1A1813FF, minor roads #26231CFF (4 units), main roads #3A352AFF (8 units).

**3.5 Strip.** Pill box `(56, 56, 288, 60)`, corner radius 30, fill #0A0907BD, 1 px border drawn inside the box in #F7F2E824. Optional backdrop blur sigma 14.

| Element | Box / position | Spec |
|---|---|---|
| HP label | text x 83.19, baseline y 77 | "HP", 13 px Bold, letter-spacing 2.08 px (0.16 em, also after the last letter), colour = HP arc colour (amber, ember when low). Advance width 20.12 |
| HP numeral | text x 113.31, baseline y 77 | 32 px Bold, tabular, **left aligned** in a 54 px wide slot (x 113.31..167.31), colour cream (ember when low). "100" is 60.5 px wide so it overruns the slot by 6.5 px; that is as designed |
| divider | `(195.70, 73, 1, 26)` | fill #F7F2E82E |
| SAT label | text x 225.11, baseline y 77 | "SAT", same style, advance width 27.69, colour = SAT arc colour (bone, ember when low) |
| SAT numeral | text x 262.80, baseline y 77 | same style as HP numeral, slot x 262.80..316.80 |

Label-to-numeral gap is 10 px. Line height is 1.0 (label box 13 px, numeral box 32 px), so the numeral box is `(113.31, 70, 54, 32)`.

## 4. State table

Arc geometry per state. Angles in degrees (y-up). "End point" is the centre of the cap at the end of the fill, screen coordinates.

| State | Value | Fill sweep | End angle | End point | Arc colour | Glow | Label colour | Numeral colour | Pulse |
|---|---|---|---|---|---|---|---|---|---|
| HP 100 full | 100 | 140.0° | 110.0° | (155.54, 386.16) | #F2B134FF | #F2B134CC | #F2B134FF | #F7F2E8FF | no |
| HP 50 | 50 | 70.0° | 180.0° | (70.00, 264.00) | #F2B134FF | #F2B134CC | #F2B134FF | #F7F2E8FF | no |
| HP 20 low | 20 | 28.0° | 222.0° | (103.39, 177.01) | #E5603AFF | #E5603AD9 | #E5603AFF | #E5603AFF | no |
| HP 8 critical | 8 | 11.2° | 238.8° | (132.66, 152.80) | #E5603AFF | #E5603AD9 | #E5603AFF | #E5603AFF | yes (800 ms alternate) |
| HP 0 dead | 0 | 0° | - | hidden | #E5603AFF | - | #E5603AFF | #E5603AFF | no |
| SAT 100 full | 100 | 140.0° | 70.0° | (244.46, 386.16) | #D9D1C3FF | #D9D1C38C | #D9D1C3FF | #F7F2E8FF | no |
| SAT 20 low | 20 | 28.0° | -42.0° | (296.61, 177.01) | #E5603AFF | #E5603AD9 | #E5603AFF | #E5603AFF | no |
| SAT 0 empty | 0 | 0° | - | hidden | #E5603AFF | - | #E5603AFF | #E5603AFF | no |

Rules (exactly as built): low = `v <= 25` (arc, glow, label and numeral all ember). Critical = `1 <= v <= 10` (the fill arc pulses). `v == 0`: fill hidden, label and numeral ember, **no pulse**. HP and SAT are independent. The numerals show the plain integer ("0", "8", "100").
Dead has no extra visual beyond the above (nothing else is designed for it).

Map on / off: toggles only 3.4. Everything else, including the disc and the strip, is identical.

## 5. Animation

There is exactly one animation: the **critical pulse**.

- What moves: nothing moves. The opacity of the fill arc and its glow (one layer) changes. Label, numeral, track, disc, ring do not change.
- Trigger: while `1 <= v <= 10` for that gauge (HP and SAT independently). Phase restarts at opacity 1.0 when the state is entered.
- Opacity goes 1.0 -> 0.4 in 800 ms, then 0.4 -> 1.0 in 800 ms, repeating (period 1600 ms).
- Easing on each 800 ms leg: ease-in-out = cubic-bezier(0.42, 0, 0.58, 1).
- Closed form: `t = (now - start) mod 1600; u = t < 800 ? t/800 : (1600 - t)/800; opacity = 1.0 - 0.6 * ease(u)`.
- Value changes are not animated in the design: arcs and numerals snap to the new value. (Any tween is your call, not designed.)
- Reduced-motion option: no animation, hold opacity 1.0.

## 6. Draw order per frame

1. disc glass (3.1) 2. tracks (3.2) 3. glow pass then crisp fill for HP, then SAT (3.3, with pulse opacity) 4. map texture clipped to the circle, then vignette (3.4) 5. ring glow, then ring (3.4) 6. strip fill + border (3.5) 7. labels, numerals, divider (3.5).

ImGui notes: `PathArcTo` + `PathStroke` for arcs (it has no round caps: add a filled circle of radius 5 at each end of a fill and of each track); there is no blur, so use the re-stroke fallback in 3.3 and 3.4 (ring glow: two extra rings outside the ring at width +6 and +14, in #F2B134 at alpha 0.18 and 0.08); bake the font at 12.22, 13 and 32 px.
