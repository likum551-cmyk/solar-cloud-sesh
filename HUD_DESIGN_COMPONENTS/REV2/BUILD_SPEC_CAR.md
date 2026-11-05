# SOLAR RP HUD: Car cluster build spec (V2 Solar speedometer + indicators)

For a native game overlay with no CSS runtime. Derived from `HudCar.dc.html` and `CarIndicator.dc.html`; the PNGs were rendered from that same markup. Same conventions as the vitals spec.

## 0. Conventions (read first)

- Screen 1920 x 1080. **Origin = bottom-left of the screen, x right, y UP.** Scale everything by `screenHeight / 1080` for other resolutions.
- Boxes are `(x, y, w, h)` = bottom-left corner + size. Circles: centre + radius.
- Component box `(1504, 56, 360, 432)`: **56 px from the right edge and 56 px from the bottom edge** (bottom-RIGHT anchor, opposite of the vitals HUD). Top-right corner is `(1864, 488)`.
- Design-to-screen: a design point `(lx, ly)` measured from the component's top-left, y down, maps to screen `(1504 + lx, 488 - ly)`; a design box `(lx, ly, w, h)` maps to `(1504 + lx, 488 - ly - h, w, h)`.
- Angles: degrees, 0 = +x (3 o'clock), positive = counter-clockwise (y-up). A circle point is `(cx + R*cos(a), cy + R*sin(a))`. For y-down APIs: `y_down = 1080 - y`, `angle_down = -angle`.
- Colours are `#RRGGBBAA`, straight alpha, sRGB. Blur is Gaussian sigma in px: CSS `drop-shadow` / `box-shadow` blur R means sigma R/2.
- Text: x = left edge of the glyph run (or centre where stated), y = baseline. Font Sofia Sans **Bold 700** only (OFL 1.1; `fonts/SofiaSans-Bold-latin.ttf` is a Latin-only conversion). Digits tabular. Letter-spacing applies after every character including the last, and counts in the advance width.
- Inputs are clamped and rounded: speed 0..500 (km/h), fuel 0..100.
- **Dial centre** `(cx, cy) = (1684, 308)` (screen). All dial radii are measured from it.

## 1. PNG exports

**Cluster PNGs** (`png/`): 360 x 432, 1:1, transparent RGBA. Place the bottom-left corner at screen `(1504, 56)`. HUD layer only: backdrop blur is not baked in. Glow tails may touch the PNG edge.

| File | State |
|---|---|
| `car_base` | baseline: speed 184, fuel 62, blinker off, locked, engine on, belt buckled, lights on |
| `car_speed000 / 125 / 250 / 400 / 500` | speed only changed (500 = redline colours) |
| `car_fuel100 / car_fuel050` | fuel 100 / 50, static |
| `car_fuel020 / 008 / 000 _pulse-hi / _pulse-lo` | low fuel: pump pulses from 25; the arc also pulses at 1-10. hi = opacity 1.0 frame, lo = 0.4 frame |
| `car_blinker-left / right / hazard _on / _off` | blink frames: on = opacity 1.0, off = opacity 0.2 |
| `car_unlocked` | lock chip off, open shackle |
| `car_engine-off` | engine off, speed 0: numeral + KM/H at 0.4 opacity, engine chip off |
| `car_belt-warn_pulse-hi / _pulse-lo` | unbuckled with engine running: chip pulses 1.0 / 0.4 |
| `car_belt-off_engine-off` | unbuckled with engine off: belt chip off (no pulse), engine off, speed 0 |
| `car_lights-off` | headlight chip off, no beams |

Every state changes one thing from `car_base`. 26 files in all.

**Sprite PNGs** (`sprites/`): each icon state on its own, **with 24 px of transparent padding on every side** so glow is not clipped. Place a sprite's bottom-left corner at `(element x - 24, element y - 24)`. Sizes: chip 94 x 94, pump 70 x 70, arrow 78 x 78.

| Files | Element | Note |
|---|---|---|
| `lock_on / lock_off` | chip 46 | lock chip, locked / unlocked |
| `engine_on / engine_off` | chip 46 |  |
| `belt_on / belt_off / belt_warn_pulse-hi / belt_warn_pulse-lo` | chip 46 | buckled / unbuckled engine off / unbuckled engine on (two pulse frames) |
| `lights_on / lights_off` | chip 46 |  |
| `pump_ok / pump_low_pulse-hi / pump_low_pulse-lo` | bare 22 | fuel pump icon, no chip |
| `arrow-left_* / arrow-right_*  (off, on_blink-hi, on_blink-lo)` | bare 30 | blinker arrows, no chip |

Element positions in the cluster (screen): lock chip `(1568, 65, 46, 46)`, engine `(1630, 65, 46, 46)`, seatbelt `(1692, 65, 46, 46)`, headlights `(1754, 65, 46, 46)`, left arrow `(1611, 265, 30, 30)`, right arrow `(1727, 265, 30, 30)`, fuel pump (value 62) `(1645, 213, 22, 22)` (moves with the value width, see 3.10).

`contact-sheet-*.png` show everything on `#C9B691`.

## 2. Colour tokens

| Token | Value |
|---|---|
| ground | #0A0907FF |
| amber (speed, blinkers, active chips) | #F2B134FF |
| bone (fuel arc, calm chips) | #D9D1C3FF |
| cream (numerals, ticks) | #F7F2E8FF |
| ember (redline, low fuel, warning) | #E5603AFF |
| muted (unit text, E/F, inactive icons) | #A9A295FF |
| disc glass | #0A0907A8 |
| strip glass | #0A0907BD |
| arc track | #F7F2E829 |
| redline track tint | #E5603A61 |
| strip border (1 px inside) | #F7F2E824 |
| speed glow / redline glow | #F2B134CC / #E5603AD9 |
| fuel glow / low glow | #D9D1C38C / #E5603AD9 |

## 3. Layers, back to front (screen coordinates, y-up)

**3.1 Glass disc.** Circle centre `(1684, 308)`, r 176 (box `(1508, 132, 352, 352)`). Fill #0A0907A8. Optional backdrop blur sigma 14 (raise alpha to 0.74-0.80 if you cannot blur).

**3.2 Speed arc track.** Ring R = 164, stroke 12 (r 158..170), round caps (cap radius 6), #F7F2E829. It runs from a = 220 (lower left) **clockwise on screen** through 180 (left), 90 (top), 0 (right) to a = -40 (lower right): a 260 degree sweep with a 100 degree gap at the bottom. End-point centres: `(1558.37, 202.58)` and `(1809.63, 202.58)`.
**Redline track:** the same ring from a = 12 to a = -40 (52 degrees = speed 400..500), #E5603A61, round caps, drawn on top of the track and under the fill.

**3.3 Speed fill.** Same ring, width 12, round caps. Angle at speed v: `a(v) = 220 - v * 260 / 500` (= `220 - 0.52 v`), growing clockwise from the lower-left end. v = 0: not drawn. Colour #F2B134FF with glow #F2B134CC; **speed > 400**: colour #E5603AFF and glow #E5603AD9. Glow = same arc blurred sigma 5 (CSS 10 px) beneath it; without blur re-stroke at width 18 (28% of glow alpha) and 26 (12%).

| Speed | Sweep | Angle a | Fill end point | Needle centre | Needle rotation | Colour |
|---|---|---|---|---|---|---|
| 0 | 0.0° | 220.0° | hidden | (1576.75, 218.01) | 220.0° | #F2B134FF |
| 125 | 65.0° | 155.0° | (1535.37, 377.31) | (1557.12, 367.17) | 155.0° | #F2B134FF |
| 184 | 95.7° | 124.3° | (1591.53, 443.45) | (1605.07, 423.63) | 124.3° | #F2B134FF |
| 250 | 130.0° | 90.0° | (1684.00, 472.00) | (1684.00, 448.00) | 90.0° | #F2B134FF |
| 400 | 208.0° | 12.0° | (1844.42, 342.10) | (1820.94, 337.11) | 12.0° | #F2B134FF |
| 436 | 226.7° | -6.7° | (1846.87, 288.81) | (1823.04, 291.62) | -6.7° | #E5603AFF |
| 500 | 260.0° | -40.0° | (1809.63, 202.58) | (1791.25, 218.01) | -40.0° | #E5603AFF |

**3.4 Dial ticks.** 21 ticks, i = 0..20, angle `a = 220 - 13 i` (one tick per 25 km/h), line from radius r0 to r1 = 148, round caps off (butt).

| i | km/h | angle | type | from | to | width | colour |
|---|---|---|---|---|---|---|---|
| 0 | 0 | 220° | major | (1582.88, 223.15) | (1570.63, 212.87) | 2.4 | #F7F2E8D9 |
| 1 | 25 | 207° | minor | (1558.37, 243.99) | (1552.13, 240.81) | 1.6 | #F7F2E86B |
| 2 | 50 | 194° | minor | (1547.19, 273.89) | (1540.40, 272.20) | 1.6 | #F7F2E86B |
| 3 | 75 | 181° | minor | (1543.02, 305.54) | (1536.02, 305.42) | 1.6 | #F7F2E86B |
| 4 | 100 | 168° | major | (1554.88, 335.44) | (1539.23, 338.77) | 2.4 | #F7F2E8D9 |
| 5 | 125 | 155° | minor | (1556.21, 367.59) | (1549.87, 370.55) | 1.6 | #F7F2E86B |
| 6 | 150 | 142° | minor | (1572.89, 394.81) | (1567.37, 399.12) | 1.6 | #F7F2E86B |
| 7 | 175 | 129° | minor | (1595.27, 417.58) | (1590.86, 423.02) | 1.6 | #F7F2E86B |
| 8 | 200 | 116° | major | (1626.14, 426.64) | (1619.12, 441.02) | 2.4 | #F7F2E8D9 |
| 9 | 225 | 103° | minor | (1652.28, 445.39) | (1650.71, 452.21) | 1.6 | #F7F2E86B |
| 10 | 250 | 90° | minor | (1684.00, 449.00) | (1684.00, 456.00) | 1.6 | #F7F2E86B |
| 11 | 275 | 77° | minor | (1715.72, 445.39) | (1717.29, 452.21) | 1.6 | #F7F2E86B |
| 12 | 300 | 64° | major | (1741.86, 426.64) | (1748.88, 441.02) | 2.4 | #F7F2E8D9 |
| 13 | 325 | 51° | minor | (1772.73, 417.58) | (1777.14, 423.02) | 1.6 | #F7F2E86B |
| 14 | 350 | 38° | minor | (1795.11, 394.81) | (1800.63, 399.12) | 1.6 | #F7F2E86B |
| 15 | 375 | 25° | minor | (1811.79, 367.59) | (1818.13, 370.55) | 1.6 | #F7F2E86B |
| 16 | 400 | 12° | major | (1813.12, 335.44) | (1828.77, 338.77) | 2.4 | #E5603AF2 |
| 17 | 425 | -1° | minor | (1824.98, 305.54) | (1831.98, 305.42) | 1.6 | #E5603AA6 |
| 18 | 450 | -14° | minor | (1820.81, 273.89) | (1827.60, 272.20) | 1.6 | #E5603AA6 |
| 19 | 475 | -27° | minor | (1809.63, 243.99) | (1815.87, 240.81) | 1.6 | #E5603AA6 |
| 20 | 500 | -40° | major | (1785.12, 223.15) | (1797.37, 212.87) | 2.4 | #E5603AF2 |

**3.5 Dial numerals.** Sofia Sans Bold 14 px, centred horizontally on the point and the digits (cap height 9.3 px) centred vertically on it (CSS `text-anchor: middle; dominant-baseline: central`). Baseline = anchor y - 4.5 (y-up). Radius 114 from the dial centre.

| Label | Anchor (screen) | Baseline y | Colour |
|---|---|---|---|
| 0 | (1596.67, 234.72) | 230.22 | #F7F2E8D9 |
| 100 | (1572.49, 331.70) | 327.20 | #F7F2E8D9 |
| 200 | (1634.03, 410.46) | 405.96 | #F7F2E8D9 |
| 300 | (1733.97, 410.46) | 405.96 | #F7F2E8D9 |
| 400 | (1795.51, 331.70) | 327.20 | #E5603AFF |
| 500 | (1771.33, 234.72) | 230.22 | #E5603AFF |

**3.6 Fuel arc.** Ring R = 164, stroke 12, round caps. Track: from a = 235 (lower left, "E") counter-clockwise on screen through 270 (bottom) to a = 305 (lower right, "F"): 70 degrees, #F7F2E829. Track end centres `(1589.93, 173.66)` and `(1778.07, 173.66)`. Fill grows **left to right**: `a(v) = 235 + 0.7 v`. v = 0: not drawn. Colour #D9D1C3FF, glow #D9D1C38C (sigma 4.5, CSS 9 px). **Fuel <= 25**: colour #E5603AFF, glow #E5603AD9. **Fuel 1..10**: fill (with glow) pulses (section 7).

| Fuel | Sweep | Angle a | Fill end point | Colour | Glow | What pulses |
|---|---|---|---|---|---|---|
| 100 | 70.0° | 305.0° | (1778.07, 173.66) | #D9D1C3FF | #D9D1C38C | no |
| 50 | 35.0° | 270.0° | (1684.00, 144.00) | #D9D1C3FF | #D9D1C38C | no |
| 25 | 17.5° | 252.5° | (1634.68, 151.59) | #E5603AFF | #E5603AD9 | pump only |
| 20 | 14.0° | 249.0° | (1625.23, 154.89) | #E5603AFF | #E5603AD9 | pump only |
| 10 | 7.0° | 242.0° | (1607.01, 163.20) | #E5603AFF | #E5603AD9 | arc + pump |
| 8 | 5.6° | 240.6° | (1603.49, 165.12) | #E5603AFF | #E5603AD9 | arc + pump |
| 0 | 0.0° | 235.0° | hidden | #E5603AFF | #E5603AD9 | pump only |

**3.7 Needle.** A bar 28 x 4, corner radius 2, centred on radius 140 at the speed angle a(v): centre `(cx + 140 cos a, cy + 140 sin a)`, long axis along the radius (rotated to a). Colour = speed fill colour (#F2B134FF / #E5603AFF above 400), glow `0 0 10px` of the same glow colour (sigma 5). Always drawn, even at speed 0 (rests on the 0 tick). Drawn above ticks and numerals. Needle runs radius 126..154, so it stays inside the arc's inner edge (158).

**3.8 Speed readout.** Numeral: Sofia Sans Bold 96 px, letter-spacing -1.92 px (-0.02 em), tabular, **centred on x = 1684**, baseline y = 318. Per-digit advance (including letter-spacing) = 58.5625 px, so a run of n digits is `58.5625 * n` wide and starts at `1684 - 58.5625 * n / 2` (for "184": x = 1596.16). Colour #F7F2E8FF; ember #E5603AFF above 400. **Engine off: opacity 0.4** (effective #F7F2E866).
Unit "KM/H": 14 px Bold, letter-spacing 3.36 px (0.24 em) plus a 3.36 px left offset, line-height 1, centred on x = 1684; run starts at x = 1661.69, advance width 47.97, baseline y = 276. Colour #A9A295FF; **engine off: opacity 0.4**.

**3.9 Blinkers.** Two arrows, 30 x 30 each (icon paths in section 5, scale 30/24 = 1.25), left at `(1611, 265, 30, 30)`, right at `(1727, 265, 30, 30)`. Filled and stroked 1.4 (design units) in the same colour, round joins. Off: #A9A295FF drawn at opacity 0.28 (effective #A9A29547), no glow. On: #F2B134FF, glow #F2B134CC sigma 2.5, blinking (section 7). Hazard = both on, same phase.

**3.10 Fuel readout row.** Row box `(1604, 208, 160, 32)`, content centred. Content = pump icon 22 x 22, 8 px gap, then the value group = numeral (26 px Bold, tabular, digit advance 16.3906 px) + 2 px gap + "%" (14 px Bold, advance 13.23, #A9A295FF). Group width = `16.3906 * digits + 2 + 13.23`; content width = `22 + 8 + group`; content left x = `1604 + (160 - content) / 2`. Example "62": pump `(1645, 213, 22, 22)`, numeral x 1675.00, "%" x 1709.77; "100": content left x = 1636.80.
Baseline of both numeral and % = y 217. Numeral colour #F7F2E8FF, ember #E5603AFF when fuel <= 25. Pump icon: state in section 4.
"E": 12 px Bold, letter-spacing 1.2 px, #A9A295FF, run starts x 1607.16, baseline y 189. "F": run starts x 1753.30, same baseline.

**3.11 Strip.** Pill `(1504, 56, 360, 64)`, radius 32, fill #0A0907BD, 1 px border inside in #F7F2E824. Four 46 px circular chips, gap 16, centred, left to right: **lock, engine, seatbelt, headlights**. Chip boxes: `(1568, 65)`, `(1630, 65)`, `(1692, 65)`, `(1754, 65)`, each 46 x 46.

## 4. Chip / indicator states

A chip is a circle of diameter D (46) with a 1.5 px border drawn inside, a fill, an optional outer glow, and an icon of size D/2 = 23 centred in it (the icon's 24-unit design box is scaled by 23/24). Bare icons (arrows 30, pump 22) have no circle, fill, border or outer glow.

| State | Icon colour | Icon opacity | Icon glow | Chip fill | Chip border | Chip outer glow | Animation |
|---|---|---|---|---|---|---|---|
| off | #A9A295FF | 0.55 (arrows 0.28) | none | #F7F2E80D | #F7F2E81F | none | none |
| on (amber: lock, engine, lights, arrows) | #F2B134FF | 1 | #F2B134CC sigma 2.5 | #F2B13424 | #F2B13499 | 0 0 16 px #F2B13459 (sigma 8) | arrows blink |
| on (calm: seatbelt, fuel pump) | #D9D1C3FF | 1 | #D9D1C366 sigma 2 | #D9D1C31A | #D9D1C34D | none | none |
| warn (seatbelt unbuckled and engine on, pump at fuel <= 25) | #E5603AFF | 1 | #E5603ABF sigma 2.5 | #E5603A29 | #E5603A99 | 0 0 16 px #E5603A59 (sigma 8) | whole chip pulses |

Mapping (as built):

| Indicator | Condition -> state |
|---|---|
| lock | locked -> on (closed shackle) / unlocked -> off (open shackle) |
| engine | running -> on / stopped -> off. Stopped also dims the speed numeral and KM/H to 0.4 |
| seatbelt | buckled -> on (calm, strap drawn) / unbuckled and engine running -> warn (no strap) / unbuckled and engine stopped -> off |
| headlights | on -> on (3 beams) / off -> off (lamp only) |
| fuel pump | fuel > 25 -> on (calm) / fuel <= 25 -> warn. Note the pump pulses from 25, the arc only from 10 |
| blinkers | off, left, right, hazard (both) -> on / off per side |

## 5. Icon geometry (24 x 24 design box, stroke 1.9, round caps and joins; "duo" = also filled with the icon colour at alpha .22)

| Icon | Shapes |
|---|---|
| arrow left | fill + stroke 1.4: `M3 12l7.5-7v4.2H21v5.6H10.5V19z` |
| arrow right | fill + stroke 1.4: `M21 12l-7.5-7v4.2H3v5.6h10.5V19z` |
| lock (both) | duo rect x5 y11 w14 h10 rx2.5; keyhole dot centre (12,16) r1.3 filled |
| lock shackle | locked `M8 11V7.5a4 4 0 0 1 8 0V11`; unlocked `M8 11V7.5a4 4 0 0 1 7.2-2.4` |
| engine | `M18.36 6.64a9 9 0 1 1-12.73 0` and `M12 2.5v9` |
| seatbelt | duo head circle (12,5.2) r2.4; duo torso `M8 21v-7a4 4 0 0 1 8 0v7`; strap (buckled only) `M9 11.5l7 6.2` |
| headlights | duo lamp `M10 5.5C6.4 5.5 4 8 4 12s2.4 6.5 6 6.5V5.5z`; beams (on only) `M14 7.5h7 M14 12h7 M14 16.5h7` |
| fuel pump | duo rect x4 y3 w10 h18 rx2; window rect x6.6 y6 w4.8 h4 rx1; nozzle `M14 9h2.5a1.5 1.5 0 0 1 1.5 1.5V16a1.5 1.5 0 0 0 3 0V8l-3-3` |

The sprite PNGs are rasterized from exactly these shapes.

## 6. Dead / empty / extreme states

- Speed 0: fill hidden, needle rests on the 0 tick, numeral "0". Speed 500: fill ends at a = -40, everything ember.
- Fuel 0: fuel fill hidden, value "0%" in ember, pump warn pulsing, no arc pulse (arc pulse needs fuel >= 1).
- Engine off: engine chip off, speed numeral + KM/H at 0.4, unbuckled seatbelt shows as off (not a warning).
- No state is designed beyond these.

## 7. Animation

Two animations; nothing moves, only opacity changes.

- **Blink** (blinker arrows, on): period 800 ms, linear. Opacity 1.0 for 0..392 ms, ramps to 0.2 by 400 ms, stays 0.2 until 800 ms, then jumps back to 1.0. Both arrows share the phase in hazard mode. Restart at opacity 1.0 when switched on.
- **Pulse** (warn chips, and the fuel fill arc while fuel is 1..10, fill + glow as one layer): opacity 1.0 -> 0.4 in 800 ms, then 0.4 -> 1.0 in 800 ms (period 1600 ms), each leg ease-in-out = cubic-bezier(0.42, 0, 0.58, 1). Closed form: `t = (now - start) mod 1600; u = t < 800 ? t/800 : (1600 - t)/800; opacity = 1.0 - 0.6 * ease(u)`. For a chip the opacity applies to the whole chip (fill, border, glow, icon). Restart at 1.0 on entering the state.
- Speed, fuel, needle and numerals are not tweened: they snap to new values. (Smoothing the needle is your call, not designed.)
- Reduce-motion option: no animation, hold opacity 1.0.

## 8. Draw order and ImGui notes

1 disc, 2 speed track, redline track, fuel track, 3 speed glow + fill, fuel glow + fill (with pulse opacity), 4 ticks, 5 numerals, 6 needle (glow then bar), 7 blinkers, 8 speed readout, KM/H, 9 fuel row, E/F, 10 strip, 11 chips (outer glow, fill, border, icon with glow).

ImGui: `PathArcTo` + `PathStroke`; no round caps, so add a filled circle of radius 6 at each end of every arc; no blur, so use the re-stroke glow fallback (widths +6 and +14 at 28% and 12% of the glow alpha) and extra translucent rings for chip glow. Rotate the needle by building the four corners of the 28 x 4 bar by hand. Rasterize the icons from section 5 into an atlas or use the sprites. Bake Sofia Sans Bold at 12, 14, 26 and 96 px.
