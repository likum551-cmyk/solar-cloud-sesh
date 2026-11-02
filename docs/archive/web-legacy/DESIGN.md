---
name: GAGE-MP
description: A server launcher drawn as the GTA V pause menu over a night freeway; every server is a destination on an overhead guide sign, and connecting means taking its exit.
colors:
  asphalt-0: "#0b0c0d"
  asphalt-1: "#121415"
  glass: "rgba(6, 7, 8, 0.58)"
  glass-row: "rgba(0, 0, 0, 0.4)"
  glass-hover: "rgba(255, 255, 255, 0.1)"
  menu-white: "#ffffff"
  menu-ink: "#0b0c0d"
  hairline: "rgba(214, 220, 224, 0.1)"
  hairline-strong: "rgba(214, 220, 224, 0.18)"
  text: "#ecebe4"
  text-2: "#b9bcb6"
  muted: "#9a9f9a"
  sign: "#0d6b42"
  sign-deep: "#0a5a37"
  sign-white: "#f2f1ea"
  amber: "#e8a33d"
  led: "#ffae36"
  advisory: "#f2c230"
  stop: "#c8352b"
  stop-text: "#f2705d"
  good: "#63cd8e"
  hud-michael: "#65b4d4"
  hud-franklin: "#abedab"
  hud-trevor: "#ffa357"
  hud-gold: "#f0c850"
  wasted-red: "#e0322e"
typography:
  lettering:
    fontFamily: "Pricedown Bl (outlined SVG paths only; never a loaded face)"
    fontSize: "26px"
    lineHeight: 0
  legend:
    fontFamily: "Overpass, Bahnschrift, Segoe UI, sans-serif"
    fontSize: "24px"
    fontWeight: 800
    lineHeight: 1.1
  exit-word:
    fontFamily: "Overpass, Bahnschrift, Segoe UI, sans-serif"
    fontSize: "21px"
    fontWeight: 800
    letterSpacing: "0.06em"
  tab:
    fontFamily: "Overpass, Bahnschrift, Segoe UI, sans-serif"
    fontSize: "12px"
    fontWeight: 800
    letterSpacing: "0.12em"
  group-title:
    fontFamily: "Overpass, Bahnschrift, Segoe UI, sans-serif"
    fontSize: "13px"
    fontWeight: 800
    letterSpacing: "0.14em"
  body:
    fontFamily: "Overpass, Bahnschrift, Segoe UI, sans-serif"
    fontSize: "14px"
    fontWeight: 400
    lineHeight: 1.45
    fontFeature: "tnum"
  label:
    fontFamily: "Overpass, Bahnschrift, Segoe UI, sans-serif"
    fontSize: "12px"
    fontWeight: 600
    letterSpacing: "0.01em"
  button:
    fontFamily: "Overpass, Bahnschrift, Segoe UI, sans-serif"
    fontSize: "12px"
    fontWeight: 700
    letterSpacing: "0.08em"
  hud:
    fontFamily: "Overpass, Bahnschrift, Segoe UI, sans-serif"
    fontSize: "10.5px"
    fontWeight: 700
    lineHeight: 1.33
    letterSpacing: "0.12em"
  mono:
    fontFamily: "Overpass Mono, Cascadia Mono, Consolas, monospace"
    fontSize: "12px"
    fontWeight: 400
  vms:
    fontFamily: "Overpass Mono, Consolas, monospace"
    fontSize: "15px"
    fontWeight: 600
    lineHeight: 1.5
    letterSpacing: "0.1em"
rounded:
  square: "0px"
  tag: "3px"
  control: "4px"
  tab-top: "6px 6px 0 0"
  plate: "7px"
  round: "50%"
spacing:
  row-gap: "2px"
  foot-rule: "3px"
  plate-gap: "6px"
  control-gap: "8px"
  deck-gap: "14px"
  section: "16px"
  row: "40px"
  bar: "44px"
  panel-head: "48px"
  list-row: "52px"
  side-column: "324px"
  side-column-wide: "380px"
components:
  panel:
    backgroundColor: "{colors.glass}"
    textColor: "{colors.text}"
    rounded: "{rounded.square}"
  panel-head:
    backgroundColor: "rgba(0, 0, 0, 0.55)"
    typography: "{typography.lettering}"
    height: "{spacing.panel-head}"
    padding: "0 12px 0 16px"
  menu-row:
    backgroundColor: "{colors.glass-row}"
    textColor: "{colors.text}"
    rounded: "{rounded.square}"
    height: "{spacing.row}"
    padding: "0 10px"
  menu-row-hover:
    backgroundColor: "{colors.glass-hover}"
  menu-row-selected:
    backgroundColor: "{colors.menu-white}"
    textColor: "{colors.menu-ink}"
  tab:
    backgroundColor: "rgba(0, 0, 0, 0.5)"
    textColor: "#c4c7c2"
    typography: "{typography.tab}"
    rounded: "{rounded.square}"
    height: "28px"
    padding: "0 13px"
  tab-active:
    backgroundColor: "{colors.menu-white}"
    textColor: "{colors.menu-ink}"
  option-row:
    backgroundColor: "{colors.glass-row}"
    textColor: "{colors.text}"
    rounded: "{rounded.square}"
    padding: "10px 14px"
  option-row-hover:
    backgroundColor: "{colors.menu-white}"
    textColor: "{colors.menu-ink}"
  button:
    backgroundColor: "rgba(0, 0, 0, 0.55)"
    textColor: "{colors.text}"
    typography: "{typography.button}"
    rounded: "{rounded.square}"
    padding: "0 13px"
    height: "34px"
  button-hover:
    backgroundColor: "#23272a"
  button-sm:
    height: "28px"
    padding: "0 10px"
  button-stop:
    backgroundColor: "transparent"
    textColor: "{colors.stop-text}"
  button-stop-armed:
    backgroundColor: "{colors.stop}"
    textColor: "#ffffff"
  connect-plate:
    backgroundColor: "{colors.sign}"
    textColor: "{colors.sign-white}"
    typography: "{typography.exit-word}"
    rounded: "{rounded.plate}"
    padding: "0 18px 0 20px"
    height: "54px"
    width: "100%"
  connect-plate-disabled:
    backgroundColor: "#171a1c"
    textColor: "#8f948f"
  quick-exit:
    backgroundColor: "{colors.sign-deep}"
    textColor: "{colors.sign-white}"
    rounded: "{rounded.control}"
    padding: "0 6px 0 10px"
    height: "30px"
    width: "100%"
  field:
    backgroundColor: "rgba(0, 0, 0, 0.62)"
    textColor: "{colors.text}"
    rounded: "{rounded.square}"
    padding: "0 10px"
    height: "34px"
  vms-board:
    backgroundColor: "#050505"
    textColor: "{colors.led}"
    typography: "{typography.vms}"
    rounded: "{rounded.square}"
    padding: "12px 16px 13px"
  notice-stop:
    backgroundColor: "rgba(200, 53, 43, 0.12)"
    textColor: "#f6b8ad"
    rounded: "{rounded.square}"
    padding: "8px 11px"
  dest-cover:
    backgroundColor: "#000000"
    textColor: "#ffffff"
    typography: "{typography.legend}"
---

# Design System: GAGE-MP

## Overview

**Creative North Star: "The Pause Menu Over the Freeway"**

The launcher is GTA V's own pause menu laid over a night freeway. The top of the window is the "Night Drive": a real-time three.js band seen from a driver's eye on a long lens, rolling down the right carriageway under sodium lamps, past fan palms and traffic, toward a downtown glow. Every 160 m a steel truss gantry carries a guide-green sign with the selected server's name and its EXIT port, beside a through-route panel naming a Los Santos freeway. Below it, the menu takes over. Square, translucent black glass panels sit over a blurred downtown skyline. Each panel has a solid black head bar titled in Pricedown lettering. Its rows are dark bars with 2px gaps, and the picked row turns white with black type, exactly as the game's list does. Official character key art (Michael, Franklin, Trevor) fills the spare spaces the way the game's loading screens do. Each piece of art is edged by its lead's HUD colour.

There are two vocabularies, and each keeps to its own job. The road owns navigation and status: the green Connect plate with its EXIT tab, the gantry signs, the amber LED variable message sign, the warning diamond and the stop octagon. The game owns the frame: the Pricedown titles, the glass, the white picked row and tab, the "< On >" option rows, the HUD clock cluster, and the "game launched" / "launch failed" shards. The window is a dense desktop utility (1240x800 default, 960x640 floor), and every screen serves browse, pick, connect.

Asset boundary (owner decision, 2026-10-24; see PRODUCT.md): the art is real GTA V key art, cropped clear of the GTA V logo and the Rockstar badge, and every shipping raster is listed in `src/assets/gta/PROVENANCE.md`. The lettering is Pricedown outlined to SVG paths at build time (`src/lib/pricedown.js`). The font file never ships. The sign art is drawn in code; the band's models and textures are CC0 and logged in `src/scene/ASSETS.md`.

**Key Characteristics:**
- Pricedown lettering (white fill, black outline) for the wordmark, panel titles, stage titles and the two shards; Overpass for everything else.
- Square black glass panels over a blurred, dimmed downtown, with a solid black head bar.
- Pause-menu rows, tabs and option rows: dark at rest, white with black ink when picked.
- One full-green guide-sign plate (Connect, or Retry after a failure) with the port on an EXIT tab, mirrored by the gantry sign in the band.
- Key art never shows a logo, never shows the same lead twice on screen, and always stands on a 3px HUD-colour foot rule.
- One status voice: the amber LED board.
- A band that never stops while focused and never shakes, in which the sign plate is always the brightest, sharpest object.
- Designed degraded paths: a composed still under reduced motion, and a rendered night still with the live sign pinned onto it when WebGL is absent or lost.

## Colors

Near-black glass and asphalt neutrals carry the frame. Road-sign colours keep their road meanings, and the four GTA HUD colours tie key art to its lead.

### Primary
- **Guide Sign Green** (sign): the bottom stop of every plate gradient (deep to full, lit from below), the Connect plate, and the canvas gantry signs (3D and fallback alike).
- **Deep Sign Green** (sign-deep): the top stop of the plate gradient, and the flat fill of the Quick exits chips. Those chips are also dimmed at rest (saturate 0.75, brightness 0.8).
- **Sign Sheeting White** (sign-white): legends, inset borders and arrows on green. It is a warm off-white, never pure white on a plate.

### Secondary
- **Pause-menu White / Ink** (menu-white, menu-ink): the picked row, the open tab, and the option row in hand. Inside a picked row every tone swaps to a dark twin that holds on white: muted #4e5356, good #0f6a38, advisory #7a5600, stop #b0261d, amber #9c5b00, and buttons fill with menu-ink.
- **Sodium Amber** (amber): focus and the text caret, a set favourite star, the direct-connect on-ramp glyph, the empty-state lane paint, the lane line running on a launching plate, and the ring of the promoted direct-connect section. It echoes the band's sodium lamps, which the scene lights with its own warmer orange.
- **LED Amber** (led): only the VMS glyphs and its lamp. The second line dims to #dc9b3e.

### Tertiary
- **HUD Michael / Franklin / Trevor** (hud-michael, hud-franklin, hud-trevor): the 3px foot rule under a piece of key art, picked from the art's lead through `data-who`. They are never used as fills or as text.
- **Shard Gold** (hud-gold): the "game launched" title only.
- **Wasted Red** (wasted-red): the "launch failed" title only. The build sets it as a literal, not as a custom property.
- **Advisory Yellow** (advisory): warning diamonds, update notices and mid-range ping (60 to 119 ms).
- **Regulatory Red** (stop): the stop octagon, an armed destructive button and the close-button hover. **Stop Text** (stop-text) is its legible tint for error text, invalid borders and slow ping (120 ms and up).
- **Signal Green** (good): good ping (under 60 ms) and success notice borders. It is never a fill.
- **Work-zone Orange** (#e07b1f, canvas only): the "EXIT CLOSED" panel bolted over the gantry sign's arrow after a failed launch.

### Neutral
- **Asphalt 0** (asphalt-0): the window ground and the colour the hero band's foot sinks into.
- **Asphalt 1** (asphalt-1): Tailwind's asphalt base. Panels are glass now, not asphalt.
- **Title Bar Black** (#070809, literal): the opaque backing of the 44px title bar, with a 1px rule at 8% steel.
- **Glass, Glass Row, Glass Hover** (glass, glass-row, glass-hover): the panel body over the backdrop, a resting row and option row, and a hovered row.
- **Steel Hairline** (hairline, hairline-strong): field and button strokes, plus faint dividers (6% white between Settings columns and sections).
- **Text, Text 2, Muted** (text, text-2, muted): primary copy, secondary copy, and labels, counts, addresses and hints. Muted (#9a9f9a) was lifted to clear 4.5:1 at 12px on the glass rows.

### Night Drive Light
The band's palette lives in the scene, not in tokens: sodium point lights (rgb 1.0, 0.56, 0.2) pooling on wet asphalt, a cool blue-black exponential fog and sky (rgb 0.013, 0.016, 0.024; density 0.0034), a cool-over-warm hemisphere fill (#2a3444 over #1a1008), red tail lamps, white or amber headlamps, amber clearance lamps on vans, and a dark paint palette on the traffic. The sodium stays the only warm light in the scene.

### Named Rules
**The Connect Owns Green Rule.** Full sign green as a large plate belongs to the Connect plate (and to the gantry sign it mirrors). Quick exits use sign-deep and are dimmed, so the eye always finds one way in.

**The White Means Picked Rule.** A solid white fill means "this one": the selected row, the open tab, the option in hand. Never use white as a background for anything else.

**The Lead's Colour Rule.** A HUD colour appears only as the foot rule under its own lead's art. It is never decoration, and it never sits under art of a different lead.

**The Road Colour Contract Rule.** Yellow is caution, red is failure or a destructive act, and green is the way in. Never borrow a sign colour for a meaning it does not carry on a road.

## Typography

**Display Font:** Pricedown Bl, as build-time SVG outlines only (`<Pricedown k=... size=...>`)
**Body Font:** Overpass, variable 100 to 900, bundled as OFL woff2 (with Bahnschrift, then Segoe UI)
**Label/Mono Font:** Overpass Mono, 400 and 600 (with Cascadia Mono, Consolas)

**Character:** Pricedown gives the game's own title voice: white under a 9-unit black outline, with the shallow 2px drop the game's titles carry. Overpass is a highway-gothic face, so sign legends read as real sign copy. Numbers are tabular throughout, and mono is reserved for addresses, versions and the LED board.

### Hierarchy
- **Lettering** (Pricedown outlines, sized by em; panel heads 26px, stage titles 30px, Direct connect 21px, top-bar wordmark 33px, shards 58px, splash wordmark 124px): only the 13 fixed strings in `pricedown.js`. Each is wrapped in `PdHeading`, which keeps the real words for screen readers. A string that was never outlined falls back to tracked caps (14px, 800, 0.14em), so a new title never breaks.
- **Legend** (800, 24px, 1.1; 21px in short windows; 18px for the idle "No server selected"): the destination name on the cover card.
- **Sign legend** (canvas, 1280x640): the name in 800, fitted from 170 down to 104px on one line, or split into two balanced lines at 120 to 72px; the host in 600 52px caps at 92% alpha; the facts line (players, ms, or NO REPLY) in 700 50px; the EXIT tab word in 700 50px and the port in 800 up to 92px. Prompt titles are 800 caps, 128 down to 64px, centred.
- **Exit word** (800, 21px, 0.06em, caps): "Connect / Retry / Launching / Launched" on the Connect plate. Its EXIT tab is 800, 12px, 0.12em.
- **Tab** (800, 12px, 0.12em, caps): the pause-menu tabs. Their counts are always rendered, dimmed at zero.
- **Group title** (800, 13px, 0.14em, caps): names a group inside a panel (Settings: Game, Server list, Behavior, Updates). The "Quick exits" head is the same treatment at 11px.
- **Body** (400, 14px, 1.45, tabular numbers): table cells are 13px, names are 14px 600 (700 when picked), option labels are 14px 600, and hints are 12 to 13px.
- **Label** (600, 12px, 0.01em, sentence case, muted): sortable column heads and counts.
- **HUD** (700, 10.5px, 0.12em, caps, #c4c7c2; the player name is 12px 800 white): the top bar's clock cluster, with facts separated by a CSS middle dot.
- **Mono** (400, 12px): host:port, the version string and address fields.
- **VMS** (Overpass Mono 600, 15px, 1.5, 0.1em, caps, LED amber with a 6px glow): the board's two lines only.

### Named Rules
**The Outlines Only Rule.** Pricedown is artwork, not a font. Add a title by outlining the string with the build tool. Never bundle, @font-face or canvas-draw the Pricedown font.

**The Two Voices Rule.** Pricedown names a place (a panel, a stage, a shard). Overpass carries everything a player reads to act. Never set body copy, buttons, legends or numbers in Pricedown.

**The Guaranteed Face Rule.** Anything drawn to canvas uses the bundled Overpass stack, and waits for it to load (800, 700 and 600) before drawing.

## Layout

The window is a three-row grid: a 44px frameless title bar, the hero band (clamp(200px, 36vh, 400px); clamp(170px, 32vh, 330px) under 780px of height), and the deck. The title bar is opaque, so a sign passing overhead leaves the frame through a solid edge and never shows behind the tabs or HUD. The hero sits absolutely behind the bar and extends 48px under the deck. Its foot sinks into asphalt-0 (clear to 66% of its height, 50% at 81%, 90% at 91%, solid at the foot) under a soft corner vignette, so the panels never cut across bright footings and rails. The backdrop (the downtown skyline, blur 5px, brightness 0.5) starts 24px above the deck and fades in over 120px, so the menu rises out of the band.

The deck is two columns: the list panel (fluid) and the launch column (324px, or 380px at 1440px and wider), with a 14px gap and 0 16px 16px padding. Below 900px wide it stacks into one scrolling column. Below 720px of panel width the table drops its Address column. At 1120px and narrower the HUD cluster keeps only the day and time.

The launch column reads top to bottom: the destination cover, the Connect plate, the VMS board, Quick exits, and direct connect bolted to its foot behind a dashed seam (16px on a 30px period, at 14%). The cover and Quick exits share the spare height (both flex basis 0). The cover is 104 to 230px. Quick exits cap at 150px, wrap rows that do not fit out of view whole, and hide entirely when the space cannot hold the head and one chip. Under 780px of height Quick exits drop out, and the Connect plate shrinks to 46px.

In the list panel, a short list leaves its spare height to key art (ListFill, chosen by container queries). From 220px up the lead stands in the right 72%, faded in from the left and from the top. From 101 to 219px a night-graded skyline strip is used instead. At 100px and below nothing is shown. Favorites and Recents end with a muted hint line and isolate Clear behind a hairline at the foot. Settings splits into two columns at 1180px and wider.

## Elevation & Depth

Depth comes from layering, not from a shadow scale. From back to front: the 3D band, the blurred backdrop, the glass, the rows. Panels cast one soft downward shadow. Sign plates hang with a short drop, and the VMS is recessed. Rows, fields, tabs and buttons are flat, and their states are tonal (glass-row, then glass-hover, then white).

### Shadow Vocabulary
- **Glass seat** (`box-shadow: 0 18px 40px -18px rgba(0,0,0,0.85)`): panels only.
- **Hung plate** (`box-shadow: inset 0 0 0 3px var(--sign), inset 0 0 0 5px var(--sign-white), 0 12px 26px -12px rgba(0,0,0,0.9)`): the Connect plate.
- **Recessed board** (`box-shadow: inset 0 0 0 4px #0c0d0e, inset 0 2px 14px rgba(0,0,0,0.9)`): the VMS.
- **Focus ring** (`box-shadow: 0 0 0 3px rgba(232,163,61,0.18)`): a focused field. A field the launcher called ("set the game folder") uses 0.28.
- **Lettering drop** (`filter: drop-shadow(0 2px 0 rgba(0,0,0,0.55))`; 0 4px 0 at 0.5 on the splash wordmark): Pricedown outlines only, as part of the game's title treatment.

### Named Rules
**The Flat Menu Rule.** Only panels and hung plates cast shadows. The hard lettering drop belongs to Pricedown alone and never moves onto a box, a button or a plate.

## Shapes

The menu is square. Panels, rows, tabs, option rows, fields, buttons, the VMS and notices all have 0 radius. Rounding belongs to road furniture: sign plates are 7px with the white border inset 3 to 5px, the EXIT tab is rounded only on top (6px 6px 0 0), Quick exits chips are 4px, and their port tag is 3px. The canvas signs use 34px plates (26px on the EXIT tab) with a 9px white stroke inset by 14px. Sign shapes are literal: the warning diamond is an 18px square turned 45 degrees with a 3px radius, and the stop octagon is a clip-path polygon. Circles appear only on the cover's 32px favourite button, the ping dot and the VMS lamp.

Key art always ends on a straight 3px HUD-colour foot rule (cover card, empty and error stages, list fill). Lane paint survives in three places: the empty-state paint (three 18px amber dashes on a 28px period), the direct-connect seam, and the launch lane (10 on, 8 off, running along the Connect plate's foot).

## Components

### Buttons
Utilitarian and recessive. The only loud button is the sign.
- **Shape:** square (0), 34px tall (28px small, 34x34 icon).
- **Default:** black glass fill (rgba(0,0,0,0.55)), strong hairline stroke, 12px 700 tracked caps.
- **Hover / Focus / Active:** #23272a with a brighter stroke. Focus is a 2px amber outline at a 2px offset, and a pressed button drops 1px. Inside a picked white row, buttons fill with menu-ink.
- **Stop:** transparent with a red-tinted stroke and a stop-text label. A destructive act arms on the first press (filled regulatory red, "Press again to clear", disarms after 3.5s or on blur) and fires on the second.

### Connect Plate (signature)
There is one primary action, and it is a guide-sign plate: a full-width 54px plate with the deep-to-full green gradient, the white inset border, the word at the left, and the "EXIT {port}" tab with an exit arrow at the right. Hover brightens it (1.15). With no selection it becomes a dark blank plate with a grey inset border. After a failed launch the same plate reads "Retry", with no second button beside it. While launching it keeps the sign, reads "Launching" with a spinner, and runs the amber lane along its foot. Once launched it reads "Launched" with a check and is dimmed until the player returns. Enter and double-click on a row take the same exit.

### Destination Cover
A job-card style cover heads the launch column. The key art is fixed per server address (FNV hash over 8 covers), and the idle cover is Franklin. The art is shown from the top so the lead's crown stays in frame. It is shaded to 94% black at the foot, carries the name (legend), host:port in mono and the facts, and stands on its lead's 3px foot rule. A round favourite toggle sits top right and turns amber when set.

### Pause-menu Rows (signature)
These are the list rows in Browse (a 40px table row) and in Favorites and Recents (a 52px list row with a name and a mono address). Each is a dark glass bar with 2px gaps, no ruled grid and no dividers. Hover lightens the bar to 10% white. The picked row turns solid white with black ink, and its focus outline turns menu-ink. Row actions (ping, favourite, remove) appear only on the row in play (hover, picked or focused). A set favourite and a ping in flight stay visible. The sticky head is solid black with sentence-case sortable labels.

### Tabs
The tabs are pause-menu tiles in the title bar: 28px, black at 50% with a faint 6% inner line, 12px 800 caps. The open tab is white with black ink. Counts on Favorites and Recents are always present.

### Option Rows (Settings)
Each on/off setting is a full-width glass bar: a 14px 600 label with a 12px muted description on the left, and "< On >" in 13px 800 caps between chevrons on the right. Hover or focus turns the row white with black ink. Click, Left and Right all flip the value. Text settings stay as fields with a Browse button.

### Quick Exits
Recents and favourites appear together as 30px sign-deep chips, each marked by its source: a clock for recent, an amber star for favourite (also given in screen-reader text). Each chip shows the name and a dark port tag. Chips are dimmed at rest, lift on hover, and lock while a launch runs. The selected server is never repeated here.

### VMS Board (signature)
This is the single status voice: a square black board with a two-line amber LED readout (the second line dimmer), a dot-matrix pitch laid over the glyphs, and a lamp that blinks in two steps while work is in flight. Its height is fixed and long lines ellipsize. A failed launch replaces the board with the stop notice (octagon, red-tinted fill and stroke, the message, and an optional "Set game folder" action) inside the same live region.

### Inputs / Fields
- **Style:** 34px, a black well at 62%, a strong hairline stroke, square corners, 13px text (12px mono for addresses), #80857f placeholder and an amber caret.
- **Focus:** an amber border with a 3px amber ring at 18%.
- **Error / Disabled:** a stop-text border, with the error line below in stop-text.

### Empty and Error Stages
A panel's empty or error body is composed like a loading screen. The lead stands in the right 72% of the panel, faded in from the left. The message sits on a dark block at the left (min(420px, 46%)) and the stage stands on the lead's foot rule. Empty: a Pricedown title, three dashes of amber lane paint, the hint and the action. Error: a stop octagon beside the title, the raw error in mono stop-text, recovery copy and Retry. With no art, the same copy is centred. When there is no list, the direct-connect section is promoted (a faint amber wash and ring) and takes focus if nothing else holds it.

### Key Art Rules
- **Crop:** every raster is cropped clear of the GTA V logo (bottom left) and the Rockstar badge (bottom right), and is listed in PROVENANCE.md with its source and crop.
- **One lead per screen:** `artAvoiding` swaps the list's art (splash rotation for Browse, Franklin for Favorites, Trevor for Recents) whenever its lead matches the cover's lead.
- **Foot rule:** every piece of key art stands on a 3px rule in its lead's HUD colour.
- **Night grade:** a skyline that sits under the night band (the short-list strip) is filtered by `--night-grade` (grayscale, toned blue, contrast 1.3, brightness 0.4). The backdrop behind the glass is blurred and dimmed instead.

### Splash
The start-up loading screen opens on the lead this start rotates to (Michael, Franklin, Trevor; a fresh profile starts on Michael). Each painting drifts slowly (scale 1.06 to 1.12 over 12s, alternate leads drifting back). After 3.4s the next lead crossfades in (900ms), so a fast start sees one painting. The lockup wipes in from the left (760ms), and the hint under it rotates every 2.6s. Bottom right it says what it is doing: "Loading servers", then "Measuring ping x/y", then "Ready". The spinner shows only while it waits. Once the list is in, it also shows "N servers · M players online", with the server total fixed and the players counting up. A 3px freemode bar on the bottom edge runs indeterminate while the list loads, then fills with the share of pings in. It lifts after at least 2.2s, once 90% of pings are in or 1.6s after the list arrived, never mid-crossfade, and at most at 5s. Any key or click lifts it at once. While it is up the title bar keeps only its window buttons. It leaves through black: the art fades first (200ms), then the black lifts (260ms), so key art never double-exposes.

### Game-mode Option (list head)
The list head's left end carries a pause-menu option, `‹ All modes 30 ›`, with 28px chevron buttons around a 13px 700 value and a muted count, on the well. Its arrows, or Left and Right while it has focus, step through the modes the list carries, busiest first. Focused, it reverses to white with black ink, as the Settings option rows do. It is hidden when the list has fewer than two modes.

### Row Tags
After a row's name come its language as a 10px boxed caps code, then its mode as 11px muted caps. A tag that does not fit beside the name wraps onto a clipped second line, so the name never truncates for it and the long mode word drops before the code. With one mode picked, the mode tag is not shown.

### Overview Standouts
With nothing picked, the card's facts end in two full-width rows, "Most players" and "Lowest ping". Each is a button that reverses to white on hover or focus like a list row, and picking it selects that server, brings its row into view and focuses it. While one has focus, the footer offers Enter Select. There is no chevron on these rows: `‹ ›` means cycle an option in this world.

### Shards (signature)
These are the game's mission-result beats, drawn over the hero band. **Launched:** a gold Pricedown "game launched" with "Joining {name}" under it, centred at 44% of the band's height on a soft black band (66%, masked at the top and bottom) over the held skycam aerial. It wipes open in 560ms and fades after 7s. **Wasted:** a red "launch failed" seated lower, at 66% of the band's height over the road and below the held plate, on a near-solid band (90%), while the 3D frame drains to grey (grayscale 1, brightness 0.42) for 2.2s. Colour then returns on the closed exit.

### Night Drive Band (signature)
The hero is a real-time three.js drive: lane 3 at a 3.9m eye on a 17 degree vertical lens, so gantries stack. The world repeats every 160m (a gantry each period, lamps every 40m, lane dashes every 10m) and the wrap is invisible. Furniture: hand-built galvanised truss gantries with zinc maps and grime at the joints, cobra-head sodium lamps, New Jersey barriers and guardrail, sourced fan palms, a facade-textured town and skyline, and instanced traffic in both carriageways whose lamps glow direct and smear down the wet road as reflections. Light is a tight bloom on HDR (strength 0.55, threshold 1.8, radius 0, wide mips almost off: 1, 0.55, 0.22, 0.06, 0.015), so lamps and tail lights flare while the plate stays under the threshold. The plate is lit from three catwalk luminaires below and is always the brightest, sharpest object in frame; the lamp pole 20m behind each gantry is left out so no lamp head sits on its corners.

The gantry carries two panels. The exit sign (8.6 x 4.3m) shares `drawSign()` with the fallback: the green gradient with a retroreflective speckle and foot grime, the white inset border, an EXIT tab with the port, the legend, the host (unless the legend is the host), a facts line only when known (players, ms, or NO REPLY), and a diagonal exit arrow. Prompt signs cover loading ("Fetching servers"), offline, empty and "Choose a server"; offline and empty aim one arrow down-right at direct connect, the list prompt points two arrows down. A failed launch bolts a work-zone "EXIT CLOSED" panel over the arrow. The narrower through-route panel (4.6m) cycles five Los Santos freeways, straight ahead.

**Motion contract:**
- **Cruise:** 17 m/s, easing to 8 m/s as each sign comes within reading range (time constant 1.1s). Lane drift and a slight bob, never shake; the view glances toward a near sign.
- **Select:** a power2.inOut surge (0.7 to 2.2s) carries the car to about 62m short of the next gantry at least 48m ahead; nearer ones pass. Its 14 slats turn like a tri-vision board, left to right over 0.5s, starting at 40% of the surge (0.3 to 0.9s after the click). A sign already within reach turns where it is.
- **Hold:** 8s at a 0.8 m/s creep (about 62m to 49m), then the drive picks up.
- **Connect:** a 1.9s power3.inOut skycam rise onto an aerial of the freeway's light river into downtown, then rendering stops so the game gets the GPU; the "game launched" shard sits over the held frame. Refocus drops back to the road (1.6s).
- **Fail:** the 2.2s grey "wasted" beat, then cruise on under EXIT CLOSED.
- **Boot:** already rolling, the first gantry 112m ahead, lamps striking up over about 2s; the canvas fades in over 900ms once Overpass and the assets are ready.
- **Schedule:** capped at 60 fps on a fixed schedule; rendering stops when the window is hidden, after 20s unfocused and idle, and during a launch.

**Degraded paths:** reduced motion renders one composed still with the sign 42m ahead, re-rendered per state change. No WebGL, or a lost context, shows `band-still.webp` (that still rendered from the scene with every plate hidden), with the live sign canvas pinned onto the plate's place by a projective transform (`FallbackBand`), so every state reads as it does on the gantry. A restored context rebuilds the scene.

## Do's and Don'ts

### Do:
- **Do** set titles in Pricedown outlines (white fill, 9-unit black outline), wrapped in `PdHeading` so the words stay readable to assistive tech.
- **Do** keep panels square glass over the blurred backdrop, with a solid black head bar.
- **Do** mark the picked row, the open tab and the option in hand with solid white and black ink, and swap every inner tone to its dark twin.
- **Do** keep a single Connect plate, and turn it into Retry after a failure rather than adding a second button.
- **Do** crop key art clear of logos, list it in PROVENANCE.md, stand it on its lead's 3px HUD foot rule, and run it through `artAvoiding` so a lead never appears twice on screen.
- **Do** apply `--night-grade` to any skyline photo that sits under the night band.
- **Do** route all status through the VMS board, and replace the board with the stop notice on failure.
- **Do** keep the gantry plate the brightest, sharpest object in the band: under the bloom threshold, lit from its catwalk, never with a lamp head on its corners.
- **Do** draw every sign state through `drawSign()`, so the 3D plate and the fallback never disagree.
- **Do** design the degraded paths: a composed still under reduced motion, and the rendered night still with the live sign pinned onto it when WebGL is absent or lost. Re-render `band-still.webp` and its plate corners whenever the camera, gantry or plate changes.
- **Do** use muted (#9a9f9a) for labels, counts, addresses and hints on glass.

### Don't:
- **Don't** ship, embed or @font-face the Pricedown font. Outline new strings with the build tool.
- **Don't** show the GTA V logo or the Rockstar badge, and never imply the launcher is official.
- **Don't** put a second full-green plate next to Connect. Quick exits stay sign-deep and dimmed.
- **Don't** round the menu: panels, rows, tabs, fields and buttons are square. Radius belongs to sign furniture.
- **Don't** put a hard offset shadow on a box. The 2px hard drop belongs to Pricedown lettering only.
- **Don't** rule the lists with grid lines; rows are separated by 2px gaps of glass.
- **Don't** use generic AI palettes: no purple or indigo gradients, no neon-on-black glow, no rainbow accents.
- **Don't** fall back to a slate sidebar, a card grid or a blue Play button.
- **Don't** invent figures on the signs: the facts line shows players and ping only when the bridge reports them.
- **Don't** stop or shake the band while the window is focused, and don't let a sign show through the title bar.
