# SOLAR RP: Inventory build spec (V2 Solar)

For a native game overlay with no CSS runtime (ImGui-style draw lists). Everything here was measured from the real markup (`InvMain.dc.html`, `InvStates.dc.html`, `InvContexts.dc.html`, `InvBackpacks.dc.html` in the Inventory artifact): the PNGs are renders of that markup and every number in `layout/*.json` comes from the live DOM of those renders. Same conventions as the vitals and car specs.

## 0. Conventions (read first)

- Screen 1920 x 1080. **Origin = bottom-left of the screen, x right, y UP.** Scale everything by `screenHeight / 1080` for other resolutions.
- Boxes are `(x, y, w, h)` = bottom-left corner + size. For a y-down API: `y_down = 1080 - y - h`.
- Colours are `#RRGGBBAA`, straight alpha, sRGB. Blur is Gaussian sigma in px: CSS `box-shadow` / `drop-shadow` blur R means sigma R/2; CSS `backdrop-filter: blur(R)` means sigma R.
- Text: `x` = left edge of the glyph run, `baseline` = baseline y (bottom-left origin). Fonts are Sofia Sans **Medium 500 / SemiBold 600 / Bold 700** (OFL 1.1, Latin-only TTF conversions in `fonts/`). **Digits are tabular.** `tracking` is letter-spacing in px added after every glyph including the last (so a centred text with tracking is offset by half a tracking step; `ax` already accounts for it).
- Text `anchor` + `ax` tell you how a text is aligned so dynamic strings can be re-laid out: `left` = x is the left edge, `center` = ax is the centre, `right` = ax is the right edge (includes trailing tracking).
- **Borders**: the fill covers the whole box including the border area; the border ring is drawn INSIDE the box. For a centred-stroke API (ImGui `AddRect`) inset the rect by `w/2` and use radius `r - w/2`. Design widths are 1 px (panels, cards, pills, chips) and **1.5 px** (slots, equip slots, active chips, buttons with border). PNGs are rendered at 2x and downsampled 2:1, so 1.5 px strokes are anti-aliased exactly as intended.
- **Dashed borders** (drop target, drag source, locked, empty PACK, no-backpack card): dash 3 px, gap 2 px (period 5) on a 1.5 px stroke, pattern runs along the straight edges and wraps the corner arcs.
- **Opacity**: an `opacity` field on an op is the product of every ancestor's opacity. Where ops overlap (glyph over fill) draw the slot/row into an offscreen layer and apply it once if you want exact parity; per-op alpha is an acceptable approximation.
- **Local coordinates**: absolute positions inside a panel/pill/card are measured from the INSIDE of its 1 px border (padding edge). The draw lists already include that, so use them verbatim and you never have to think about it.
- `backdrop-filter` blur (panels, hint bar) is NOT in the PNGs: draw the world, run a Gaussian blur pass (sigma 14), then draw the dim and the panels. See section 4.

## 1. What is in this package

| Path | What |
|---|---|
| `BUILD_SPEC.md` | this file |
| `contact-sheet-screens.png` | the 7 full screens composited over the blurred sand scene (reference look) |
| `contact-sheet-states.png` | slot, equipment, quick strip, weight pill, tooltip, context menu, split dialog (board render) |
| `contact-sheet-nearby.png` | every state of the right panel (board render) |
| `contact-sheet-backpacks.png` | the three backpack tiers (board render) |
| `png/scene_reference.png` | the sand street scene used behind the contact sheet (look reference only, not part of the HUD) |
| `png/screens/` | **7 full screens, 1920 x 1080, transparent, HUD layer only** (no world, no dim, no blur). Place at (0, 0). |
| `png/panels/` | the three panels cropped 1:1: `left_pack-{none,4,8,16}` and `center_pack-{none,4,8,16}` (448 x 800 / 646 x 800), `right_*` x 7 contexts (646 x 800). Screen positions in section 4. |
| `png/components/` | 33 sprites, each with **24 px transparent padding on every side** (the drag ghost has 48) so glow is never clipped. Place a sprite's bottom-left at `(element x - pad, element y - pad)`. |
| `layout/screen_*.json`, `layout/panel_right_*.json` | draw lists in SCREEN coordinates (origin bottom-left of the screen), paint order |
| `layout/component_*.json` | draw lists in COMPONENT coordinates (origin = bottom-left of the component box, padding excluded) |
| `layout/txt/*.txt`, `layout/COMPONENTS.txt` | the same draw lists as grep-friendly text |
| `icons/svg/*.svg`, `icons/png/*.png`, `icons/icons.json` | all 46 glyphs: 24-unit SVG (stroke #F7F2E8) and 96 x 96 white PNG, tintable |
| `fonts/` | Sofia Sans Medium / SemiBold / Bold (Latin) + OFL license |

Screens (PNG name; the matching draw list is `layout/screen_<name without inv_>.json`): `inv_pack-none_nearby-vehicle`, `inv_pack-4_nearby-vehicle`, `inv_pack-8_nearby-vehicle` (default), `inv_pack-16_nearby-vehicle`, `inv_pack-8_nearby-ground`, `inv_pack-8_nearby-stash`, `inv_pack-8_nearby-nothing`. Right-panel variants: `vehicle-suv`, `vehicle-suv-locked`, `vehicle-moto`, `vehicle-van`, `ground`, `stash`, `nothing`.

## 2. How to build it

1. Draw each panel by replaying `layout/screen_pack-8_nearby-vehicle.json` top to bottom. Every op has a `role` where it matters (`panel`, `slot.<state>`, `equip.<kind>.<state>` with state = empty / filled / hover / selected / source / target / deny / locked / ghost, `chip.*`, `button.*`, `bar.track`, `bar.fill`, `pill`, `card`).
2. Replace the demo data with real data: slot states and glyph names come from the item, texts from the data, counts from the grids. The op list tells you exactly where each piece sits.
3. Components (section 5) are reusable: the same slot / bar / chip / button / pill code draws every panel.
4. The right panel is a switch on context (section 6): same frame, different body.
5. Compare against `png/screens/*.png` at 1:1 for each state.

Everything inside a grid follows one rule: **8 columns, cell pitch 74, slot 64 x 64** (gap 10). Cell (row r, col c) with the TOP row = row 0: `x = x0 + 74*c`, `y = y0 - 74*r` (y up), where `(x0, y0)` is the bottom-left of cell (0, 0).

## 3. Colour tokens

| Token | Value | Used for |
|---|---|---|
| ground | #0A0907FF | glass base, dark text on amber |
| amber | #F2B134FF | active / selected / primary action / pack tag |
| bone | #D9D1C3FF | calm / worn items / weight bars |
| cream | #F7F2E8FF | primary text and icons |
| ember | #E5603AFF | denied, low condition (25 or less), heavy and full weight |
| muted | #A9A295FF | labels, empty glyphs, hints |
| panel glass | #0A0907BD | the three panels (border #F7F2E824, 1 px) |
| overlay glass | #0A0907E6 | tooltip, context menu, split dialog (border #F7F2E824) |
| card fill | #F7F2E80D | detail card, vehicle card, person card (border #F7F2E81F) |
| pill fill | #F7F2E80D | action bar, quick strip, search, tabs (border #F7F2E81F) |
| bar track | #F7F2E829 | every weight / scroll track |
| bar fill | #D9D1C3FF normal, #E5603AFF at 90 percent or more | weight bars |
| world dim | #0A090773 (ground at 0.45) | full-screen rect under the panels |

Chip and button styles (fill / border / text colour):

| Role | Fill | Border | Text |
|---|---|---|---|
| `chip.amber` | #F2B13424 | 1.5px solid #F2B13499 | #F2B134FF |
| `chip.bone` | #D9D1C31A | 1.5px solid #D9D1C34D | #D9D1C3FF |
| `chip.ghost` | #F7F2E812 | 1px solid #F7F2E81F | #A9A295FF |
| `button.ghost` | #F7F2E812 | 1px solid #F7F2E824 | #F7F2E8FF |
| `button.solid` | #F2B134FF | none | #0A0907FF (glow #F2B13459, sigma 8) |

## 4. Screen skeleton (bottom-left px)

Draw order: **world, blur pass (sigma 14), world dim rect (0, 0, 1920, 1080) #0A090773, hint bar, the three panels, then panel contents.** The HUD (vitals, car) is hidden while the inventory is open.

| Element | Box (x, y, w, h) | Radius | Notes |
|---|---|---|---|
| CHARACTER panel | (64, 172, 448, 800) | 32 | backdrop blur sigma 14 behind it |
| INVENTORY panel | (538, 172, 646, 800) | 32 | same |
| NEARBY panel | (1210, 172, 646, 800) | 32 | same |
| Key hint bar | (476.69, 106, 966.63, 40) | 20 | centred on x = 960; width = content + 52 (padding 26 each side); items separated by 24; same glass + border as panels |

Margins: 64 left and right, 26 between panels, panels are 108 from the top and 172 from the bottom. Panel titles are 28 px Bold at the panel's inner left + 32.
Key hint bar (left to right): `SHIFT` click: quick move, `ALT` drag: half stack, `CTRL` drag: split, `RMB` menu, `MMB` drop, `1-4` use quick slot, `TAB` close. Keycap = 22 px high, padding 0 8, radius 7, fill #F7F2E81F, border 1 px #F7F2E82E, text 11 px Bold tracking 0.88 px cream; label 13 px SemiBold bone; keycap to label gap 8.

### 4.1 CHARACTER panel

- Title `CHARACTER` and the name chip `MARA QUILL` (chip.ghost 150 x 34, text 12 px Bold muted, tracking 1.44) at the top right.
- **Equipment slots** (64 x 64, radius 16): two columns of 7, vertical pitch 78. Left column x = 97, right column x = 417. Rows from the top (bottom-left of each slot, default screen):

  - `equip.hat.filled` (97, 823, 64, 64)
  - `equip.glasses.filled` (97, 745, 64, 64)
  - `equip.mask.empty` (97, 667, 64, 64)
  - `equip.ears.empty` (97, 589, 64, 64)
  - `equip.neck.empty` (97, 511, 64, 64)
  - `equip.watch.filled` (97, 433, 64, 64)
  - `equip.wrist.empty` (97, 355, 64, 64)
  - `equip.jacket.filled` (417, 823, 64, 64)
  - `equip.shirt.filled` (417, 745, 64, 64)
  - `equip.armor.empty` (417, 667, 64, 64)
  - `equip.pack.filled` (417, 589, 64, 64)
  - `equip.gloves.empty` (417, 511, 64, 64)
  - `equip.pants.filled` (417, 433, 64, 64)
  - `equip.shoes.filled` (417, 355, 64, 64)

- **Mannequin**: an original silhouette, group opacity 0.085 of #F7F2E8, box (177, 355, 224, 532) (224 x 532), plus a soft amber halo `radial-gradient(closest-side, #F2B13421, #F2B13400)` 300 x 300 behind the torso at (139, 477, 300, 300). Shapes in mannequin-local top-left coords: head circle (112,58) r34; neck line (112,88)-(112,116) width 26 round; torso rect (62,108) 100 x 176 r32; arms lines (58,134)-(38,296) and (166,134)-(186,296) width 30 round; hands circles (36,316) and (188,316) r15; legs lines (88,276)-(84,476) and (136,276)-(140,476) width 44 round; feet ellipses (80,504) and (144,504) rx30 ry14.
- **Quick slots**: label `QUICK SLOTS` 12 px Bold muted tracking 2.4 at baseline 314 (x 97) and `PRESS 1 - 4` right aligned to ax 481. Strip pill (97, 203, 384, 96) radius 48 (fill #F7F2E80D, border 1 px). Four slots 72 x 72 radius 18 at x 110, 206, 302, 398, pitch 96, y 214. Key numeral `1`..`4`: 12 px Bold, x = slot x + 9, baseline = slot top - 16 (cream at 0.55).

### 4.2 INVENTORY panel

| Element | Box / position (bottom-left px) |
|---|---|
| Title `INVENTORY` | x 571, baseline 923, 28 px Bold cream, tracking 0.28 |
| Weight pill | (915, 911, 238, 40) radius 20 (see 5.4) |
| Search pill | (571, 857, 160, 36) radius 18: search icon 16 + `Search` 14 px Medium muted |
| Category chips (h 36, gap 8) | ALL (56 wide, chip.amber when active), TOOLS 70, FOOD 66, MEDICAL 90, DOCS 70; first chip x = 745, y = 857; text 12 px Bold tracking 1.44 |
| Pockets section | label `POCKETS · 16 SLOTS` x 571 baseline 816; right text ax 1153; bar track (571, 801, 582, 4) (w 582, h 4); grid cell (0,0) = (571, 725) |
| Backpack section (tier 4 / 8 / 16) | label `BACKPACK · <NAME> · <N> SLOTS` baseline 614; bar track y 599; grid cell (0,0) = (571, 523) (1 row for 8, 2 rows for 16, half a row for 4) |
| Backpack section (no pack) | label `BACKPACK` + right text `NONE EQUIPPED`; dashed card (571, 449, 582, 138) |
| Detail card | (571, 281, 582, 150) radius 24 (selected item tile 72, name 24 px Bold, category chip, description 14 px Medium bone, three stats, BIND TO QUICK SLOT chips) |
| Action bar | (571, 209, 582, 56) radius 28: minus (40), quantity numeral 20 px Bold, plus (40), USE (button.solid 118 wide), GIVE 92, DROP 92, SPLIT 96 |

Section label = 12 px Bold muted tracking 2.4; its right text (weights) 12 px Bold bone (ember at 90 percent or more) tracking 1.2; track sits 20 px below the label's top, grid starts 36 px below the label's top (both in design px).

### 4.3 NEARBY panel

Title `NEARBY`; tab pill (1475, 911, 350, 40) radius 20 (3 tabs, 114 x 32 each, 4 px inset: VEHICLE, GROUND, STASH; active tab = amber fill #F2B13424 + 1.5 px border #F2B13499, amber text; inactive muted; with no context the whole pill is drawn at opacity 0.55).
Footer pill (1243, 209, 582, 56) radius 28 (same slot as the action bar): left buttons, right hint text 11 px Bold muted tracking 1.54 (ax 1814).

| Context | First grid cell (0,0) bottom-left | Layout |
|---|---|---|
| Vehicle SUV | glovebox (1243, 687), trunk (1243, 559) | card (1243, 811, 582, 76); glovebox 8 slots (1 row); trunk 32 slots (4 rows) |
| Vehicle motorcycle | top case (1243, 619) | card; dashed `NO GLOVEBOX ON THIS VEHICLE` card; top case 4 slots |
| Vehicle van | cargo (1243, 559) | glovebox 8 slots; cargo 48 slots in a clipped viewport 582 x 330 (4.5 rows) with a bottom fade and a scroll track (x 1837 in screen px, 4 wide, track 330 high at y 293, thumb 128 high at y 495) |
| Ground | (1243, 699) | 24 slot grid, then GIVE TO NEARBY person cards 286 x 72 (r24) |
| Stash | (1243, 687) | 48 slots (6 rows) |
| Vehicle locked | no grids | every slot drawn `slot.locked`; message card 424 x 120 centred over the trunk; footer UNLOCK button |

All of it is in `layout/panel_right_*.json` and `layout/screen_*.json`.

## 5. Components and states

### 5.1 Item slot (64 x 64, radius 16; quick slot 72 x 72, radius 18)

Glyph size = 50 percent of the slot (32 in a 64 slot, 36 in a 72 slot), centred. Origin is the slot's bottom-left. One state at a time:

| State | Role in draw list | Fill | Border | Shadow | Glyph colour | When |
|---|---|---|---|---|---|---|
| empty | `slot.empty` | #F7F2E80A | 1.5px solid #F7F2E81A | - | - | no item |
| filled | `slot.filled` | #F7F2E812 | 1.5px solid #F7F2E829 | - | #D9D1C3FF | has item |
| hover | `slot.hover` | #F7F2E81F | 1.5px solid #F7F2E857 | - | #F7F2E8FF | mouse over |
| stack | `slot.filled` | #F7F2E812 | 1.5px solid #F7F2E829 | - | #D9D1C3FF | qty above 1 |
| condition-ok | `slot.filled` | #F7F2E812 | 1.5px solid #F7F2E829 | - | #D9D1C3FF | condition 1 to 99 |
| condition-low | `slot.filled` | #F7F2E812 | 1.5px solid #F7F2E829 | - | #D9D1C3FF | condition 25 or less |
| selected | `slot.selected` | #F2B13424 | 1.5px solid #F2B13499 | #F2B13459 offset(0,0) blur 16 sigma 8 | #F2B134FF + glow #F2B134CC sigma 2.5 | clicked / active quick slot |
| drag-source | `slot.source` | #F7F2E808 | 1.5px dashed #F7F2E847 | - | #D9D1C3FF | slot the drag started from |
| drop-target | `slot.target` | #F2B1341A | 1.5px dashed #F2B134B3 | - | #F2B134FF + glow #F2B134CC sigma 2.5 | valid drop while dragging |
| denied | `slot.deny` | #E5603A29 | 1.5px solid #E5603A99 | #E5603A59 offset(0,0) blur 16 sigma 8 | #E5603AFF + glow #E5603ABF sigma 2.5 | hovered but invalid / overweight |
| locked | `slot.locked` | #F7F2E805 | 1.5px dashed #F7F2E81F | - | #A9A295FF | beyond capacity or locked vehicle |
| drag-ghost | `slot.ghost` | #F7F2E824 | 1.62px solid #F7F2E866 | #00000080 offset(0,-12) blur 24 sigma 12; #F2B13459 offset(0,0) blur 16 sigma 8 | #F7F2E8FF | item following the cursor |

Notes: `drag-source` is drawn at opacity 0.55 (the whole slot); `drag-ghost` is the filled slot scaled 1.08 about its centre (radius and border scale too) with the shadow above; `locked` shows a 23 px lock icon at opacity 0.35; `drop-target` with no item shows a 23 px plus (amber, glow).

Slot details (design offsets from the slot's bottom-left):
- **Quantity tag**: only when qty > 1: `×N` (multiplication sign U+00D7 + number) for 2..999, `1.2k` style for 1000 and up (one decimal, `.0` dropped). 13 px Bold cream, tabular, text-shadow #00000099 offset (0,-1) blur 2 (sigma 1). Right-align the text to slot x + (slot width - 8) (i.e. 8 px in from the slot's right edge, 7 px design + 1 px border); baseline = slot y + 9. The listing below shows a one-digit tag (x 40 = 56 - 16).
- **Condition bar**: only when condition is 0..99: track at (slot x + 11, slot y + 7), width slot width - 22, height 3, radius 2, fill #F7F2E829; fill width = condition percent of the track, colour bone, ember at 25 or less.

```
# slot_stack (qty tag)
rect  (0, 0, 64, 64)  r16  fill #F7F2E812  border 1.5px solid #F7F2E829  # slot.filled
icon  bottle  (16, 16)  size 32  #D9D1C3FF
text  "×6"  x 40  baseline 9  w 16  13px/700  #F7F2E8FF  tabular  anchor left (ax 40)  text-shadow [#00000099 offset(0,-1) blur 2 sigma 1]
# slot_condition-low (condition bar)
rect  (0, 0, 64, 64)  r16  fill #F7F2E812  border 1.5px solid #F7F2E829  # slot.filled
icon  wrench  (16, 16)  size 32  #D9D1C3FF
rect  (11, 7, 42, 3)  r2  fill #F7F2E829
rect  (11, 7, 7.55, 3)  r2  fill #E5603AFF
# slot_selected
rect  (0, 0, 64, 64)  r16  fill #F2B13424  border 1.5px solid #F2B13499  shadow [#F2B13459 offset(0,0) blur 16 sigma 8]  # slot.selected
icon  medkit  (16, 16)  size 32  #F2B134FF  glow #F2B134CC blur 5 sigma 2.5
text  "×2"  x 40  baseline 9  w 16  13px/700  #F7F2E8FF  tabular  anchor left (ax 40)  text-shadow [#00000099 offset(0,-1) blur 2 sigma 1]
```

### 5.2 Equipment slot (64 x 64, radius 16)

14 kinds: `hat glasses mask ears neck watch wrist` (left) and `jacket shirt armor pack gloves pants shoes` (right). Glyph per kind: hat, glasses, mask, ears, chain (neck), watch, bracelet, jacket, tee (shirt), vest (armor), backpack (pack), gloves, pants, shoe.

- Empty: 26 px glyph in muted at opacity 0.45 (glyph box at (20, 28) from the slot's bottom-left, i.e. 10 px below the slot top; caption baseline 10, centred on the slot) + 10 px Bold caption (HAT, GLASSES, MASK, EARS, NECK, WATCH, WRIST, JACKET, SHIRT, ARMOR, PACK, GLOVES, PANTS, SHOES) tracking 1.0, muted #A9A295CC. Slot style = `slot.empty`.
- Worn: 32 px bone glyph centred, slot style = `slot.filled`.
- Selected / item fits / wrong slot: same style as `slot.selected` / `slot.target` / `slot.deny` (roles `equip.<kind>.selected|target|deny`) (caption takes the state colour).
- **PACK slot**: empty = dashed amber (#F2B13473 1.5 px, fill #F2B1340D) with a 28 px amber glyph at opacity 0.6 and caption `PACK` #F2B134BF. Worn = 28 px glyph + a tier tag: pill 16 high, padding 0 7, radius 8, fill amber, text 10 px Bold #0A0907 tracking 0.6, centred 5 px above the slot bottom; text `4 SLOTS`, `8 SLOTS` or `16 SLOTS`. Denied (pack not empty) = `slot.denied` style with the tag kept.

```
# equip_empty
rect  (0, 0, 64, 64)  r16  fill #F7F2E80A  border 1.5px solid #F7F2E81A  # equip.hat.empty
icon  hat  (20, 28)  size 26  #A9A295FF  opacity 0.45
text  "HAT"  x 22.77  baseline 10  w 20.45  10px/700  #A9A295CC  tracking 1px  anchor center (ax 32.99)
# equip_worn
rect  (0, 0, 64, 64)  r16  fill #F7F2E812  border 1.5px solid #F7F2E829  # equip.hat.filled
icon  hat  (17, 15)  size 32  #D9D1C3FF
# equip_pack-8
rect  (0, 0, 64, 64)  r16  fill #F7F2E812  border 1.5px solid #F7F2E829  # equip.pack.filled
icon  backpack  (19, 28)  size 28  #D9D1C3FF
rect  (4.99, 6, 54.02, 16)  r8  fill #F2B134FF
text  "8 SLOTS"  x 11.99  baseline 11  w 40.02  10px/700  #0A0907FF  tracking 0.6px  anchor center (ax 32)
```

### 5.3 Quick slot strip (384 x 96)

Pill radius 48, fill #F7F2E80D, border 1 px #F7F2E81F. Four 72 x 72 slots at x 13 / 109 / 205 / 301, y 11 (pill-local, border included). Key numerals 1 to 4: x = slot x + 9, baseline = slot top - 16, 12 px Bold, cream at 0.55 (amber #F2B134F2 on the pressed slot). Variants: **default** (bound items), **key2-pressed** (the pressed slot uses `slot.selected` for 150 ms), **binding** (all four slots `slot.target`, bound items still drawn), **unbound** (empty slots show a plus glyph, 36 px, muted at 0.55). Quick slots are references to items in pockets or pack, not storage.

```
# quickstrip_default
rect  (0, 0, 384, 96)  r48  fill #F7F2E80D  border 1px solid #F7F2E81F  # pill
rect  (13, 11, 72, 72)  r18  fill #F7F2E812  border 1.5px solid #F7F2E829  # slot.filled
icon  bottle  (31, 29)  size 36  #D9D1C3FF
text  "×3"  x 61  baseline 20  w 16  13px/700  #F7F2E8FF  tabular  anchor left (ax 61)  text-shadow [#00000099 offset(0,-1) blur 2 sigma 1]
text  "1"  x 22  baseline 67  w 5.77  12px/700  #F7F2E88C  anchor left (ax 22)
rect  (109, 11, 72, 72)  r18  fill #F7F2E812  border 1.5px solid #F7F2E829  # slot.filled
icon  medkit  (127, 29)  size 36  #D9D1C3FF
text  "×2"  x 157  baseline 20  w 16  13px/700  #F7F2E8FF  tabular  anchor left (ax 157)  text-shadow [#00000099 offset(0,-1) blur 2 sigma 1]
text  "2"  x 118  baseline 67  w 6.63  12px/700  #F7F2E88C  anchor left (ax 118)
rect  (205, 11, 72, 72)  r18  fill #F7F2E812  border 1.5px solid #F7F2E829  # slot.filled
icon  radio  (223, 29)  size 36  #D9D1C3FF
text  "3"  x 214  baseline 67  w 6.75  12px/700  #F7F2E88C  anchor left (ax 214)
rect  (301, 11, 72, 72)  r18  fill #F7F2E80A  border 1.5px solid #F7F2E81A  # slot.empty
icon  plus  (319, 29)  size 36  #A9A295FF  opacity 0.55
text  "4"  x 310  baseline 67  w 6.84  12px/700  #F7F2E88C  anchor left (ax 310)
```

### 5.4 Weight pill (238 x 40, radius 20) and bars

Pill: icon 18 at (15, 10); numeral 20 px Bold (cream, ember at 90 percent or more) + ` / 27 KG` 12 px Bold muted tracking 1.2 (cap = pocket cap 15 + pack cap); mini bar track 66 x 6 at (159, 16), radius 3, fill = bone/ember, width = 66 x fraction. State by fraction `carried / cap`: under 0.9 normal (fill cream at 0.05, border 1 px cream 0.14, bone bar); 0.9 or more heavy (fill #E5603A1A, border 1.5 px #E5603A80, ember numeral + ember bar); 1.0 full = heavy + the bar fill pulses 1.0 to 0.4 opacity over 0.8 s ease-in-out alternate (infinite). Section bars (pockets, pack, glovebox, trunk, stash) are 582 x 4 radius 2 with the same colours.

```
# weight_normal
rect  (0, 0, 238, 40)  r20  fill #F7F2E80F  border 1px solid #F7F2E824  # pill
icon  weight  (15, 10)  size 18  #D9D1C3FF
text  "14.2"  x 43  baseline 13  w 43.09  20px/700  #F7F2E8FF  tabular  anchor left (ax 43)
text  "/ 27 KG"  x 91.09  baseline 13  w 46.14  12px/700  #A9A295FF  tracking 1.2px  anchor left (ax 91.09)
rect  (159, 16, 66, 6)  r3  fill #F7F2E829  # bar.track
rect  (159, 16, 34.69, 6)  r3  fill #D9D1C3FF  # bar.fill
# weight_heavy
rect  (0, 0, 238, 40)  r20  fill #E5603A1A  border 1.5px solid #E5603A80  # pill
icon  weight  (15, 10)  size 18  #E5603AFF
text  "25.6"  x 43  baseline 13  w 43.09  20px/700  #E5603AFF  tabular  anchor left (ax 43)
text  "/ 27 KG"  x 91.09  baseline 13  w 46.14  12px/700  #A9A295FF  tracking 1.2px  anchor left (ax 91.09)
rect  (159, 16, 66, 6)  r3  fill #F7F2E829  # bar.track
rect  (159, 16, 62.59, 6)  r3  fill #E5603AFF  # bar.fill
# weight_full_pulse-hi (pulse-lo = bar.fill at opacity 0.4)
rect  (0, 0, 238, 40)  r20  fill #E5603A1A  border 1.5px solid #E5603A80  # pill
icon  weight  (15, 10)  size 18  #E5603AFF
text  "27.0"  x 43  baseline 13  w 43.09  20px/700  #E5603AFF  tabular  anchor left (ax 43)
text  "/ 27 KG"  x 91.09  baseline 13  w 46.14  12px/700  #A9A295FF  tracking 1.2px  anchor left (ax 91.09)
rect  (159, 16, 66, 6)  r3  fill #F7F2E829  # bar.track
rect  (159, 16, 66, 6)  r3  fill #E5603AFF  # bar.fill
```

### 5.5 Overlays

Tooltip (300 x 196, radius 24, overlay glass), context menu (210 x 254, radius 24) and split dialog (320 x 294, radius 24): full draw lists in `layout/COMPONENTS.txt` and `layout/component_tooltip.json`, `component_contextmenu.json`, `component_splitdialog.json`. Spawn the tooltip 400 ms after hover, 16 px right/below the cursor, flipped to stay on screen. The context menu opens at the cursor; the hovered row is `fill #F2B13424` radius 12 with amber text and icon. Rows: USE, GIVE >, DROP, SPLIT, BIND TO SLOT >, divider, INSPECT.
The split dialog fraction chips are 1/2, 1/3, 2/3, 1/4, 3/4 (active = chip.amber); stepper buttons 44 round; CANCEL ghost, SPLIT solid.

### 5.6 Other repeated parts

- **Detail card** 582 x 150: selected tile (72 slot, `slot.selected`), name 24 px Bold, category chip (76 x 22, text 10 px Bold tracking 1.4), description 14 px Medium bone, stats (QUANTITY / UNIT / STACK: label 10 px Bold muted tracking 1.6 over value 16 px Bold cream), BIND TO QUICK SLOT chips 1 to 4 (28 x 28: bone when taken by another item, amber for this item, ghost when free).
- **Vehicle card** 582 x 76: tile 52 (radius 16, glyph 28), name 20 px Bold, plate chip (88 x 22) + class chip, lock chip right (136 x 32): `UNLOCKED` = chip.bone with open lock, `LOCKED` = chip.amber with closed lock.
- **Person card** 286 x 72 radius 24: avatar circle 44 (initials 14 px Bold), name 16 px Bold, distance 12 px SemiBold muted; as a drop target: dashed amber border + `GIVE` label.
- **Buttons**: `button.solid` (amber, text #0A0907, glow), `button.ghost`; 13 px Bold tracking 1.56; heights 40 (44 in the dialog).

## 6. Text styles (every one used)

| Size / weight | Colour | Tracking px | Tabular | Count | Samples |
|---|---|---|---|---|---|
| 34px / 700 | #F7F2E8FF | - | yes | 1 | `3` |
| 28px / 700 | #F7F2E8FF | 0.28 | - | 28 | `NEARBY`, `CHARACTER`, `INVENTORY` |
| 24px / 700 | #F7F2E8FF | - | - | 7 | `Medkit` |
| 20px / 700 | #E5603AFF | - | yes | 3 | `27.0`, `25.6` |
| 20px / 700 | #F7F2E8FF | - | yes | 15 | `14.2`, `22.0`, `1` |
| 20px / 700 | #F7F2E8FF | - | - | 12 | `On the ground`, `Apartment 3B`, `Bati 801` |
| 18px / 700 | #A9A295FF | 2.88 | - | 2 | `NOTHING IN REACH` |
| 18px / 700 | #F7F2E8FF | - | - | 2 | `Water`, `Medkit` |
| 16px / 700 | #F7F2E8FF | - | - | 7 | `2 / 5`, `0.6 kg`, `1.2 kg` |
| 16px / 700 | #F7F2E8FF | - | yes | 21 | `2 / 5`, `0.6 kg`, `1.2 kg` |
| 14px / 700 | #A9A295FF | 1.96 | - | 1 | `NO BACKPACK` |
| 14px / 700 | #D9D1C3FF | 0.56 | - | 4 | `NR`, `TB` |
| 14px / 700 | #F7F2E8FF | 2.24 | - | 1 | `VEHICLE LOCKED` |
| 14px / 500 | #A9A295FF | - | - | 9 | `No vehicle, drop or stash within 2 m.`, `Search` |
| 14px / 500 | #D9D1C3FF | - | - | 7 | `Restores 35 HP over 8 s. Single use.` |
| 13px / 700 | #0A0907FF | 1.56 | - | 11 | `SPLIT`, `TAKE ALL`, `UNLOCK` |
| 13px / 700 | #A9A295FF | 1.3 | - | 1 | `INSPECT` |
| 13px / 700 | #F2B134FF | 1.3 | - | 1 | `USE` |
| 13px / 700 | #F7F2E8FF | 1.3 | - | 4 | `GIVE`, `DROP`, `SPLIT` |
| 13px / 700 | #F7F2E8FF | - | yes | 156 | `×3`, `×2`, `1.2k` |
| 13px / 700 | #F7F2E8FF (x 0.55) | - | yes | 1 | `×2` |
| 13px / 700 | #F7F2E8FF | 1.56 | - | 40 | `CANCEL`, `SORT`, `TAKE ALL` |
| 13px / 600 | #D9D1C3FF | - | - | 51 | `menu`, `move`, `click · quick move` |
| 13px / 500 | #A9A295B3 | - | - | 2 | `Walk up to a car and press TAB, or drop an item to see it here.` |
| 13px / 500 | #A9A295FF | - | - | 4 | `Drag an item onto a person to hand it over, or use GIVE in the action bar.`, `Unlock it with your key to open storage.`, `Equip one in the PACK slot for 4, 8 or 16 extra slots.` |
| 13px / 500 | #D9D1C3FF | - | - | 1 | `Restores 35 HP over 8 s. Single use.` |
| 12px / 700 | #A9A295FF | 2.4 | - | 50 | `SPLIT STACK`, `DROPPED ITEMS`, `GIVE TO NEARBY` |
| 12px / 700 | #A9A295FF | 0.48 | - | 4 | `1/3`, `2/3`, `1/4` |
| 12px / 700 | #A9A295FF | 1.2 | - | 11 | `/ 27 KG`, `/ 40 KG`, `/ 20 KG` |
| 12px / 700 | #A9A295FF | 1.44 | - | 59 | `VEHICLE`, `STASH`, `GROUND` |
| 12px / 700 | #A9A295FF (x 0.55) | 1.44 | - | 6 | `VEHICLE`, `GROUND`, `STASH` |
| 12px / 700 | #A9A295FF | 1.92 | - | 1 | `NO GLOVEBOX ON THIS VEHICLE` |
| 12px / 700 | #A9A295FF | 1.68 | - | 3 | `— / 12 KG`, `— / 110 KG`, `NONE EQUIPPED` |
| 12px / 700 | #A9A295FF | - | - | 7 | `4` |
| 12px / 700 | #D9D1C3FF | 1.2 | - | 30 | `9 ITEMS`, `45.3 / 100 KG`, `1.6 / 15 KG` |
| 12px / 700 | #D9D1C3FF | 1.44 | - | 9 | `OWNER`, `UNLOCKED` |
| 12px / 700 | #D9D1C3FF | - | - | 14 | `1`, `3` |
| 12px / 700 | #F2B134F2 | - | - | 1 | `2` |
| 12px / 700 | #F2B134FF | 0.48 | - | 1 | `1/2` |
| 12px / 700 | #F2B134FF | 1.44 | - | 20 | `GROUND`, `STASH`, `VEHICLE` |
| 12px / 700 | #F2B134FF | - | - | 7 | `2` |
| 12px / 700 | #F7F2E88C | - | - | 43 | `1`, `2`, `3` |
| 12px / 600 | #A9A295FF | - | - | 1 | `6 in stack · 0.5 kg each` |
| 12px / 600 | #A9A295FF | 0.72 | - | 4 | `1.2 M`, `1.9 M` |
| 11px / 700 | #A9A295FF | 1.54 | - | 14 | `3 DROPS · 9 ITEMS · WITHIN 2 M`, `ITEMS FADE AFTER 10 MIN`, `SHIFT + CLICK TO MOVE` |
| 11px / 700 | #A9A295FF | 1.32 | - | 18 | `PERSONAL`, `SOL 0713`, `MOTORCYCLE` |
| 11px / 700 | #F2B134FF | 1.76 | - | 2 | `GIVE` |
| 11px / 700 | #F7F2E8FF | 0.88 | - | 51 | `RMB`, `SHIFT`, `ALT` |
| 10px / 700 | #0A0907FF | 0.6 | - | 10 | `16 SLOTS`, `4 SLOTS`, `8 SLOTS` |
| 10px / 700 | #A9A295CC | 1 | - | 43 | `HAT`, `MASK`, `EARS` |
| 10px / 700 | #A9A295FF | 1.4 | - | 8 | `MEDICAL` |
| 10px / 700 | #A9A295FF | 1.6 | - | 31 | `QUANTITY`, `UNIT`, `STACK` |
| 10px / 700 | #E5603AFF | 1 | - | 1 | `HAT` |
| 10px / 700 | #F2B134BF | 1 | - | 2 | `PACK` |
| 10px / 700 | #F2B134FF | 1 | - | 1 | `JACKET` |

## 7. Icons

46 glyphs, one 24-unit viewBox, **stroke 1.9 units (so stroke = 1.9 x size / 24 px), round caps and joins, no fill except the translucent body shapes** (`currentColor` at 0.22). `icons/svg/<name>.svg` are the exact paths (stroke and fill colour #F7F2E8); `icons/png/<name>.png` are 96 x 96 white renders: tint them with the target colour (alpha is kept). Item glyphs: phone, card, key, cash, bottle, medkit, torch, radio, notebook, bandage, burger, wrench, can, box, map, camera, battery. Equipment: hat, glasses, mask, ears, chain, jacket, tee, vest, gloves, pants, shoe, watch, bracelet, backpack. UI: car, moto, van, drop, stash, search, person, weight, lock, unlock, plus, minus, chev, x, ring.
Sizes used: 14 px: chev, key, lock, unlock; 16 px: box, car, drop, lock, medkit, minus...; 18 px: weight; 23 px: lock, plus; 26 px: bracelet, chain, ears, gloves, hat, jacket...; 28 px: backpack, bottle, car, drop, lock, moto...; 32 px: bandage, battery, bottle, box, burger, camera...; 35 px: bandage; 36 px: backpack, bottle, medkit, plus, radio; 40 px: car.
Glow (selected / denied / drop target / active tab): drop-shadow 0 0 5 px, sigma 2.5, colour = glyph colour at 0.8 (`#F2B134CC`, `#E5603ABF`).

## 8. Rules and behaviour

**Backpack (add-on worn in PACK)**

| Tier | Name | Extra slots | Max weight | Total carry |
|---|---|---|---|---|
| 4 | POUCH | 4 | 5 kg | 20 kg |
| 8 | DAYPACK | 8 | 12 kg | 27 kg |
| 16 | RUCKSACK | 16 | 25 kg | 40 kg |

Pockets are always 16 slots / 15 kg. The INVENTORY panel shows a BACKPACK section under POCKETS: 4 slots = half a row, 8 = one row, 16 = two rows of the same 8-column grid. No pack: dashed `NO BACKPACK` card. A pack must be empty to unequip (`equip.pack.deny` while hovering). Dropping or giving the pack removes its tier. Pack weight has its own bar and is added to the total pill.

**Weight**: stored in grams per unit, shown in kg to one decimal (`%.1f / %g KG`). Moving an item that would exceed a cap is denied; a stack moves whole if it fits, otherwise the part that fits. 90 percent or more of any cap: ember. 100 percent: ember + pulsing bar + pickups blocked.

**Vehicle storage** (trunk by GTA vehicle class, scaled to the 8-column grid; glovebox 8 slots / 12 kg for everything except bikes):

| Class | Slots | Max kg |
|---|---|---|
| Motorcycle (top case) | 4 | 15 |
| Compact | 12 | 40 |
| Sports, Super | 12 | 40 |
| Coupe, Muscle | 16 | 60 |
| Sedan | 24 | 80 |
| SUV, Offroad, Utility | 32 | 110 |
| Van, Commercial | 48 | 160 |

Mid-engine cars label the section FRONT TRUNK. Bikes have no glovebox. Bicycles, boats and aircraft have no storage (hide the VEHICLE tab). A locked vehicle shows the LOCKED card. Open within 2 m of the car, or from the driver seat (glovebox only).

**Nearby context**: tabs show only what exists within 2 m; default order vehicle, ground, stash. Ground = all drops within 2 m merged in one grid + GIVE TO NEARBY people (drag onto a person); drops fade after 10 minutes. Stash = owned or keyed storage, 48 slots / 100 kg.

**Quick slots**: bind by dragging an item onto a slot, or from the item card chips. Pressing 1 to 4 uses the bound item. If the item runs out the slot keeps its glyph at 0.35 until a stack of the same item arrives, then re-links.

**Interactions**: click select / detail card; double click use or equip; drag moves, merges on the same item, swaps on a different item; Shift+click quick move to the other side (pockets then pack); Alt+drag half; Ctrl+drag split dialog; right click context menu; middle click drop one stack; Esc or right click while dragging cancels; TAB closes. While dragging: valid slots `drop-target`, invalid hovered `denied`, origin `drag-source`, cursor item `drag-ghost`.

**Motion** (the only animations): bar pulse on a full weight pill (opacity 1.0 to 0.4, 0.8 s, ease-in-out, alternate, infinite); quick-slot key flash 150 ms (proposal); respect reduced motion by turning pulses into static opacity 1.0.

**Suggested data model**
```
item   { id, name, glyph, unitGrams, stackMax, category, condition? 0..100 }
slot   { index, itemId, qty, condition }
inv    { id, kind: pockets|pack|glovebox|trunk|stash|ground, slots, maxGrams }
equip  { hat, glasses, mask, ears, neck, watch, wrist, jacket, shirt, armor, pack, gloves, pants, shoes } -> itemId
quick  [ ref1, ref2, ref3, ref4 ]   ref = { invId, slotIndex } or null
pack item carries { tier: 4|8|16 }; its inv is created on equip and must be empty to destroy
```
Equipment maps to GTA ped data: hat prop 0, glasses prop 1, mask component 1, ears prop 2, neck component 7, watch prop 6, wrist prop 7, jacket component 11, shirt component 8, armor component 9, pack component 5, gloves component 3, pants component 4, shoes component 6.

## 9. Reference data used in the PNGs

| Item | Glyph | Unit kg | Category |
|---|---|---|---|
| Phone | phone | 0.2 | TOOLS |
| ID card | card | 0.02 | DOCS |
| Car key | key | 0.05 | TOOLS |
| Cash | cash | 0 | DOCS |
| Water | bottle | 0.5 | FOOD |
| Sandwich | burger | 0.3 | FOOD |
| Medkit | medkit | 0.6 | MEDICAL |
| Bandage | bandage | 0.05 | MEDICAL |
| Flashlight | torch | 0.4 | TOOLS |
| Radio | radio | 0.35 | TOOLS |
| Notebook | notebook | 0.2 | DOCS |
| City map | map | 0.1 | DOCS |
| Fuel can | can | 4 | TOOLS |
| Toolkit | wrench | 2.2 | TOOLS |
| Battery | battery | 0.2 | TOOLS |
| Camera | camera | 0.5 | TOOLS |
| Supply crate | box | 1.5 | TOOLS |

Pockets (16 slots, 6.2 / 15 kg): Phone, ID card, Car key, Cash x1240, Water x3, Sandwich x2, Medkit x2, Bandage x6, Flashlight, Radio, Notebook, City map. Selected = Medkit (slot 7). Quick slots: Water x3, Medkit x2, Radio, empty.
Backpacks: POUCH Water x2, Sandwich x2, Bandage x6, Battery x3; DAYPACK Water x4, Sandwich x3, Medkit x2, Bandage x8, Battery x4, Camera, Toolkit; RUCKSACK Fuel can, Toolkit, Water x6, Sandwich x4, Medkit x3, Battery x4, Camera, Supply crate, Bandage x10, City map, Notebook.
SUV: glovebox Car key, ID card, City map; trunk Fuel can, Toolkit, Supply crate, Water x8, Sandwich x5, Medkit x2, Battery x6, Bandage x12, Camera. Ground: Water x2, Bandage x4, Car key, Cash x300, Flashlight, Sandwich, Battery x2, Notebook, Phone. Stash holds 24 items (45.3 / 100 kg).

## 10. ImGui notes

- Shapes: rounded rects via `AddRectFilled` (fill) then `AddRect` inset by half the border width. Dashed borders: build the rounded-rect outline polyline and emit 3 px dashes with 2 px gaps.
- Glows (selected, denied, buttons, active tabs) are soft shadows: bake one 9-slice glow sprite per colour (sigma from the tables) or draw 3 to 4 expanding translucent rounded rects. The sprites in `png/components/` already include the glow with 24 px of room.
- Icons: bake `icons/svg` with NanoSVG into one atlas at the sizes in section 7, or draw the 96 px PNGs tinted and scaled. Stroke scales with size.
- Fonts: Sofia Sans Medium / SemiBold / Bold; build atlases at the pixel sizes in section 6 (scaled by screenHeight / 1080), Latin range only. Enable tabular numerals (or use a fixed advance for digits).
- Backdrop blur: capture the frame, Gaussian blur sigma 14 (two passes, half resolution is fine), draw the dim, then the panels.
- Hit boxes: the slot rect is the hit box; drag starts after 4 px of movement; hover tooltip 400 ms.
- Everything is authored at 1920 x 1080: multiply every number by `screenHeight / 1080` and snap slot rects to whole pixels.

## 11. Assumptions made (change freely)

1. Pockets 16 slots / 15 kg; pack tiers 4 / 8 / 16 with 5 / 12 / 25 kg; tier names Pouch, Daypack, Rucksack are placeholders.
2. Trunk sizes are scaled down from ox_inventory so they fit an 8-column grid.
3. Category chips: ALL, TOOLS, FOOD, MEDICAL, DOCS.
4. Timings and ranges (2 m reach, 400 ms tooltip, 10 min drop fade, 150 ms key flash) are placeholders.
5. Not designed yet: shops, crafting, item rarity colours, the small quick-slot hotbar on the HUD while the screen is closed.
