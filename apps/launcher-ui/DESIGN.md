---
name: Solar Launcher
description: A window onto Los Santos. Real GTA V footage runs full-bleed behind every screen; Solar RP is a thin, hard-edged instrument laid over it, with one name, one live number and one gold PLAY.
colors:
  ground: "#0a0907"
  panel: "#100e0b"
  panel-2: "#17140f"
  panel-3: "#201c16"
  ink: "#f7f2e8"
  ink-2: "#d9d1c3"
  ink-3: "#b3aa9a"
  ink-4: "#7a7266"
  line: "rgb(255 244 224 / 0.12)"
  line-2: "rgb(255 244 224 / 0.2)"
  line-soft: "rgb(255 244 224 / 0.07)"
  sun: "#f2b134"
  sun-hi: "#ffc75a"
  sun-lo: "#c98d16"
  on-sun: "#1a1206"
  live: "#4cc67f"
  warn: "#ff8a3d"
  danger: "#f05a4f"
typography:
  display:
    fontFamily: "'Sofia Sans Extra Condensed', 'Arial Narrow', sans-serif"
    fontSize: "clamp(56px, 7.4vw, 104px)"
    fontWeight: 800
    lineHeight: 0.86
    letterSpacing: "0.005em"
  display-profile:
    fontFamily: "'Sofia Sans Extra Condensed', 'Arial Narrow', sans-serif"
    fontSize: "clamp(64px, 8vw, 120px)"
    fontWeight: 800
    lineHeight: 0.84
  display-settings:
    fontFamily: "'Sofia Sans Extra Condensed', 'Arial Narrow', sans-serif"
    fontSize: "clamp(64px, 7.4vw, 112px)"
    fontWeight: 800
    lineHeight: 0.84
  display-product:
    fontFamily: "'Sofia Sans Extra Condensed', 'Arial Narrow', sans-serif"
    fontSize: "clamp(52px, 5.8vw, 88px)"
    fontWeight: 800
    lineHeight: 0.88
    letterSpacing: "0.005em"
  figure:
    fontFamily: "'Sofia Sans Extra Condensed', 'Arial Narrow', sans-serif"
    fontSize: "clamp(48px, 4.6vw, 64px)"
    fontWeight: 800
    lineHeight: 0.82
    letterSpacing: "0.01em"
    fontFeature: "'tnum' 1, 'lnum' 1"
  launch-step:
    fontFamily: "'Sofia Sans Extra Condensed', 'Arial Narrow', sans-serif"
    fontSize: "clamp(40px, 4.4vw, 60px)"
    fontWeight: 800
    lineHeight: 0.92
    letterSpacing: "0.005em"
  headline:
    fontFamily: "'Sofia Sans Extra Condensed', 'Arial Narrow', sans-serif"
    fontSize: "40px"
    fontWeight: 800
    lineHeight: 0.95
    letterSpacing: "0.01em"
  title:
    fontFamily: "'Sofia Sans Extra Condensed', 'Arial Narrow', sans-serif"
    fontSize: "26px"
    fontWeight: 800
    lineHeight: 1
    letterSpacing: "0.02em"
  card-name:
    fontFamily: "'Sofia Sans Extra Condensed', 'Arial Narrow', sans-serif"
    fontSize: "22px"
    fontWeight: 800
    lineHeight: 0.95
    letterSpacing: "0.02em"
  label-nav:
    fontFamily: "'Sofia Sans Extra Condensed', 'Arial Narrow', sans-serif"
    fontSize: "15px"
    fontWeight: 700
    lineHeight: 1
    letterSpacing: "0.1em"
  label:
    fontFamily: "'Sofia Sans Extra Condensed', 'Arial Narrow', sans-serif"
    fontSize: "12px"
    fontWeight: 700
    lineHeight: 1
    letterSpacing: "0.12em"
  body:
    fontFamily: "'Sofia Sans', 'Segoe UI', sans-serif"
    fontSize: "15px"
    fontWeight: 400
    lineHeight: 1.45
    fontFeature: "'tnum' 1, 'lnum' 1"
  body-lead:
    fontFamily: "'Sofia Sans', 'Segoe UI', sans-serif"
    fontSize: "16px"
    fontWeight: 400
    lineHeight: 1.45
  body-small:
    fontFamily: "'Sofia Sans', 'Segoe UI', sans-serif"
    fontSize: "13px"
    fontWeight: 400
    lineHeight: 1.45
rounded:
  none: "0px"
  dot: "50%"
spacing:
  rail: "60px"
  gutter: "clamp(24px, 2.8vw, 44px)"
  column: "clamp(288px, 24vw, 344px)"
  emblem: "clamp(88px, 9vw, 120px)"
  split: "clamp(24px, 3vw, 56px)"
  floor: "clamp(24px, 4.4vh, 44px)"
  cut: "14px"
  cut-s: "8px"
components:
  plate-play:
    backgroundColor: "{colors.sun}"
    textColor: "{colors.on-sun}"
    rounded: "{rounded.none}"
    height: "64px"
    padding: "0 40px"
    width: "clamp(240px, 21vw, 300px)"
  plate-play-hover:
    backgroundColor: "{colors.sun-hi}"
  plate-play-active:
    backgroundColor: "{colors.sun-lo}"
  plate-primary:
    backgroundColor: "{colors.sun}"
    textColor: "{colors.on-sun}"
    rounded: "{rounded.none}"
    height: "40px"
    padding: "0 18px"
  plate-ghost:
    backgroundColor: "{colors.panel-2}"
    textColor: "{colors.ink}"
    rounded: "{rounded.none}"
    height: "40px"
    padding: "0 18px"
  plate-quiet:
    backgroundColor: "transparent"
    textColor: "{colors.ink-2}"
    height: "40px"
    padding: "0 18px"
  plate-disabled:
    backgroundColor: "{colors.panel-3}"
    textColor: "{colors.ink-4}"
  field-input:
    backgroundColor: "{colors.ground}"
    textColor: "{colors.ink}"
    rounded: "{rounded.none}"
    height: "44px"
    padding: "0 14px"
  chip:
    backgroundColor: "rgb(11 13 16 / 0.78)"
    textColor: "{colors.ink-2}"
    typography: "{typography.label}"
    rounded: "{rounded.none}"
    height: "22px"
    padding: "0 8px"
  server-card:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink}"
    rounded: "{rounded.none}"
    height: "clamp(76px, 10.4vh, 94px)"
    padding: "12px 12px 14px 16px"
  panel:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink}"
    rounded: "{rounded.none}"
  auth-panel:
    backgroundColor: "{colors.panel}"
    padding: "26px 28px 24px"
    width: "min(420px, 42vw)"
  rail:
    backgroundColor: "linear-gradient(180deg, rgb(7 8 10 / 0.8), rgb(7 8 10 / 0.42) calc(100% - 18px), rgb(7 8 10 / 0))"
    height: "{spacing.rail}"
  toggle:
    backgroundColor: "{colors.panel-3}"
    width: "60px"
    height: "28px"
---

# Design System: Solar Launcher

## Overview

**Creative North Star: "A Window onto Los Santos"**

The client is one 1240x800 frameless window, and every screen is real GTA V: a muted, held trailer shot (Play, Auth) or an official screenshot (Shop, Profile, Settings) fills the whole window, edge to edge, under the 60px rail. Solar RP does not draw a world; it lays a thin, hard-edged instrument over the one Rockstar already made. Each screen has one name at display size standing on the bottom-left corner, one live figure, and one gold action. The middle of the picture is left alone.

Provenance: this world is owner-pinned, not chosen by a direction roll, and there is no re-roll. It replaced a home-made real-time 3D street and a film-strip design after the owner's verdict on 2026-10-29: "this is not gta5 3d? its just dogshit 3d wannabe. the g logo is good but its not it man", together with the brief "you can download anything and use any tool available go". So the footage is the real game (official trailers and Steam screenshots, owner-accepted IP risk, sources in `ASSETS.md`), and the chrome G emblem the owner kept is the only real-time 3D left. The references the owner named are the cinematic lobbies of the big GTA RP launchers (Majestic RP, GTA5RP, Grand RP) and the Riot Client.

Density is low on Play and the Shop showroom (a hero and a column of five cards), and higher on the reading surfaces (orders, owned, settings, profile facts), where the footage drops to a near-solid ground and opaque panels carry the content.

**Key Characteristics:**
- Full-bleed GTA V footage or stills on every screen; one continuous held shot per loop, never a montage.
- Grading at the edges only: a band under the rail, the bottom-left corner the type stands on, a falloff on the right for the list.
- Warm night ink for all type and panels, so the picture carries the colour.
- Solar gold is rationed to the one active choice; green only for "online"; ember and red only for product states.
- Sofia Sans Extra Condensed 800 caps for names and figures, Sofia Sans for reading, tabular figures everywhere.
- Opaque near-black panels and plates with the G mark's single 45-degree top-right corner cut; no radius, no glass.
- The chrome G emblem, studio-lit, is the only real-time 3D.

**Brand art is switched off until the Solar RP logo lands** (`BRAND_ART` in `src/brand/brand.ts`). The G mark, the lockup SVG and the 3D emblem are the retired GAGE-MP art. While the switch is off, the lockup is the typeset wordmark SOLAR RP (display face, 800 caps, ink), every place that held the flat G shows the typeset monogram SR, emblem slots render nothing (so names close up to the gutter), and no WebGL context is created. Everything below that mentions the G, the lockup or the emblem describes the layout with brand art on. The 45-degree corner cut stays: it is the system's geometry, not the logo.

## Colors

NOON (chosen by the owner, 2026-11-02, for the Solar RP rebrand): warm near-blacks and paper-white ink with one rationed Solar gold; all other hue in the window comes from the footage. Every text pair passes WCAG AA.

### Primary
- **Solar Gold** (`sun`): the brand colour. It is spent on: the PLAY plate (also while busy), the one submit of a form (Log in, BUY), the 2px frame of the chosen server or product card, the 4px chamfered marker under the active rail tab, the running launch segment, and text selection. Text on gold is **On Sun** (`on-sun`, 9.8:1), never white (1.9:1). Hover lifts to **Gold Lift** (`sun-hi`), press sinks to **Gold Pressed** (`sun-lo`). Gold Lift is also the 2px focus ring and the input caret.

### Tertiary (state)
- **Online Green** (`live`): the 8px dot beside "Online now", on server cards and the done tick inside DELIVERED/PAID chips. Nothing else is green.
- **Ember Hold** (`warn`, orange, not gold, so a warning never reads as an action): not settled yet or needs the player: AWAITING PAYMENT and QUEUED chips, "Not verified", the maintenance dot, stale-data line under the hero, an unset GTA V folder, and the warning banner (ember title, 42% ember hairline).
- **Signal Red** (`danger`): something failed or was taken away: FAILED, EXPIRED and REVERSED order chips, the "Stopped" chip of a failed launch, field and form errors (with the authored alert mark), the danger plate, and the hard banner that shows a ban with its reason and expiry. These states are product truth from the platform (`apps/platform/docs/api.md`: orders `failed`, `refunded`, `chargeback`; account bans), not decoration.
- A refunded order is settled and given back: its chip is neutral (ink-2 on a 20% hairline), neither ember nor red.

### Neutral
- **Night Ground** (`ground`): the window's floor; shown before the first frame decodes, inside text fields, and as the base of every scrim gradient (`rgb(10 9 7 / a)`).
- **Panel Black** (`panel`): opaque panels (auth, settings, ledger summary and record), card base under the screenshot, banners.
- **Raised Panel** (`panel-2`): the ghost plate's inner fill, owned tiles, notices.
- **Slot Grey** (`panel-3`): disabled plates, the toggle slot, the rail avatar.
- **Ink** (`ink`): names, figures, active tabs, primary text; the capacity bar fill.
- **Ink 2** (`ink-2`): reading copy, meta rows, chip text.
- **Ink 3** (`ink-3`): the one secondary grey: labels, hints, "of 300 slots". (Rail tabs use ink-2: they sit on footage.)
- **Ink 4** (`ink-4`): disabled text and decoration only (below 4.5:1).
- **Hairlines** (`line-soft` 7%, `line` 12%, `line-2` 20% white): row dividers, field borders, chip and ghost-plate edges, card frames on hover.
- The window's close button turns Windows close red (#c42b1c) on hover, the OS convention; it is not part of the palette.

### Named Rules
**The Rationed Gold Rule.** Gold marks the one active choice on a screen: PLAY, a form's single submit, the chosen card, the active rail tab, the running launch step, focus. Secondary choices are ink: the Shop's sub-tab bar is an ink bar, a toggle that is on turns white, a done launch segment is solid ink.

**The Green Dot Rule.** Green means a server is online, as a dot. It is never a button, a fill or a text colour.

**The Colour Comes From The Picture Rule.** Type, panels and chrome stay neutral; any warmth or neon in the window belongs to the GTA V frame behind it.

## Typography

**Display Font:** Sofia Sans Extra Condensed (with Arial Narrow), 400-900 variable, Latin and Cyrillic subsets bundled.
**Body Font:** Sofia Sans (with Segoe UI), 300-900 variable, Latin and Cyrillic subsets bundled.

**Character:** A tall, tight condensed face set in heavy caps names things the way a game title card does; the open Sofia Sans underneath reads plainly. Figures are tabular and lining everywhere (`tnum`, `lnum` on `body`).

### Hierarchy
- **Display** (800, `clamp(56px, 7.4vw, 104px)`, 0.86, caps): the chosen server's name on Play and the same name on the launch overlay, at the same size so the title holds its place when the screen gives way. Quiet variant (no server) `clamp(44px, 4.6vw, 68px)`. Profile name `clamp(64px, 8vw, 120px)`/0.84; Settings title `clamp(64px, 7.4vw, 112px)`/0.84; Shop product name `clamp(52px, 5.8vw, 88px)`/0.88 (quiet `clamp(40px, 4.2vw, 60px)`). Display names carry a soft dark halo (`0 2px 28px rgb(0 0 0 / 0.4)`).
- **Figure** (800, `clamp(48px, 4.6vw, 64px)`, 0.82): the live player count. Ledger figures: wallet and VIP count 44px (38px at 1180px and below), spend 36px (32px), product price 40px.
- **Launch step** (800, `clamp(40px, 4.4vw, 60px)`, 0.92, caps): the current launch step, the overlay's headline.
- **Headline** (800, 40px, 0.95, caps): the auth panel title (34px on windows 760px tall or less); empty-record title 34px.
- **Title** (800, 26px, caps): panel titles, settings section heads, profile fact figures, VIP days. Card names 22px (server, product and owned tiles). Wallet balance 20px.
- **Section** (800, `clamp(21px, 1.6vw, 23px)`, 1, caps, 0.08em): the rail's PLAY SHOP PROFILE SETTINGS; the username beside the avatar is 700 16px/0.08em.
- **Plate labels** (800, caps, 0.1em): 15px default, 13px small, 18px large, 30px on the PLAY plate (0.12em; 24px for the "set up" variant).
- **Label** (600-700, 12-15px, caps, 0.06-0.14em): sub-tabs 15px/700/0.1em; hero meta row 15px/600/0.08em; "Online now / of 300 slots" 14px/600/0.1em; list head 14px/700/0.14em; codes 13px/600/0.06em; field labels, chips, table heads, launch segment names, card meta 12px; toggle ON/OFF 10px/800.
- **Body** (400, 15px, 1.45): all reading copy. Leads 16px (hero body 48ch, product line 46ch, launch status); launch outcome lead 18px (52ch); secondary 14px (why-not line 36ch, banners, links); hints and fine print 13px; footers 12px. Auth tagline is the one condensed reading line: 600, `clamp(16px, 1.4vw, 20px)`, caps.

### Named Rules
**The Name Face Rule.** Extra Condensed 800 caps is for names, figures, plate labels and small caps labels. Sentences are always Sofia Sans in sentence case.

**The Figures Line Up Rule.** Counts, prices, clocks and dates are tabular lining figures, so a ticking count or launch clock never shifts.

## Layout

One grammar for every screen: the picture holds the window; the title block sits bottom-left on the scrim; the right column carries the list or the opaque panel. Screens are a two-column grid, `minmax(0, 1fr)` beside the column (`column`, about 320px at 1240), with a `split` gap, padded `24px gutter floor`. Profile widens the right side to `min(420px, 40vw)`, Settings to `min(600px, 52vw)`, Auth to `min(420px, 42vw)`. The Shop puts a 14px sub-tab bar above the same grid; its Orders and Owned pages become a ledger: the summary panel as a full-height left column at `column` width, the record filling the rest.

The 60px rail sits on top of everything; its top 8px is left empty for WebView2's resize border and the rail is the window's drag region. The emblem is a `clamp(88px, 9vw, 120px)` square slot beside the display name (auth: `clamp(96px, 9vw, 128px)`).

Verified sizes: 1024x700, 1240x800 (design size), 1440x900. At 1180px wide and below the rail drops the username and the order table drops the order-id column; at 760px tall and below the hero, nameplate and auth panel tighten their gaps and the auth title steps to 34px. Lists and panels scroll inside themselves; the window never scrolls.

## Elevation & Depth

Depth comes from the footage and the scrims, not from shadows. Content sits on the picture at two levels: type directly on a graded edge, or an opaque panel. Edges are drawn as inset 1px box-shadows (hairlines), never as drop shadows.

The grade is per screen and edge-only. Play: a 150px band under the rail (70%), the bottom corner (86% to 42% at 24%, clear by 48%), a left falloff (50%, clear by 40%), a right falloff for the list (66%, clear by 36%). Profile and Settings: deeper bottom (90%, clear by 64%) and a left-to-right ramp. Shop showroom: the deepest bottom-left (94%, clear by 56%) because product stills are bright. Orders and Owned: one flat 92% veil, a near-solid reading ground. The launch overlay keeps the picture and lays one left gradient (78% to clear by 70%) plus a bottom one; a failed launch greys and darkens the footage behind it.

### Shadow Vocabulary
- **Name halo** (`text-shadow: 0 2px 28px rgb(0 0 0 / 0.4)`): display names over footage.
- **Label halo** (`text-shadow: 0 1px 10px rgb(0 0 0 / 0.75)`): small labels standing directly on footage (list head, Shop sub-tabs).
- **Emblem shadow** (`filter: drop-shadow(0 14px 22px rgb(0 0 0 / 0.55))`): the 3D emblem's canvas only.

### Named Rules
**The Edge Grade Rule.** Scrims darken only the edges the type and panels stand on; the middle of the frame stays untouched footage. Reading surfaces are the exception and take a flat near-solid veil.

**The Opaque Rule.** Panels, plates, chips and cards are opaque or near-opaque fills. No backdrop blur, no glass.

## Shapes

Every surface is square-cornered except one 45-degree corner cut at the top right, taken from the G mark, made with `clip-path`. Panels and the PLAY plate cut 14px (`cut`); standard plates cut 8px (`cut-s`); the rail avatar and the toggle slot cut 6px. Chips, fields, cards, banners and the capacity bars are plain rectangles. The only round things are the 8px status dots and the auth waiting pulse. Ghost and danger plates draw their 1px edge as an inner fill inset by 1px, so the edge follows the cut.

**The One Cut Rule.** One corner, top right, 45 degrees. No border radius anywhere, no other notches or angles.

## Components

### Rail
- **Style:** 60px, the top edge of the picture rather than window chrome: graded `rgb(10 9 7 / 0.8)` to `0.42`, then to nothing over its last 18px, so it hands off to each screen's top scrim with no ruled edge. The Solar RP lockup left at `--lockup-h` (26-28px, its letters as tall as the section names; on auth the flat G alone), a 24px hairline rule, then PLAY SHOP PROFILE SETTINGS in the Section style (`clamp(21px, 1.6vw, 23px)`), ink-2 with a soft dark text halo over bright footage (4.4:1 or better on the measured poster frames; ink-3 fell to about 3:1 on Play), ink on hover; the current one is ink on a deeper halo. A 20%-white 1px track runs under the sections only, first label to last. Right: the demo pill (13px caps on a 20% hairline), the account chip (32px avatar with a 7px cut, the username in condensed caps, dropped at 1180px and below), a 24px rule, three 46px window buttons with 1px caption glyphs 10px across on whole pixels; close goes Windows red on hover, painted up through the 8px resize band (paint only). Signed out: a light `0.6`-to-0 shade only, no band, no rule; it keeps its height as the drag region.
- **Active:** one 4px Solar gold marker with the G's 45-degree cut on its top-right corner, riding the track under the current tab's label and sliding to the next tab over 340ms (transform and width; scaleX would stretch the cut). It sits on a 2px Ground keyline with the same cut (5px), a separate layer that travels with it: gold alone can drop under 3:1 on a bright sky, on the keyline it holds 3:1 everywhere. The rail stacks above the launch overlay (`--z-rail` 7), which runs to the window's top edge under it. Hover previews a 4px 34%-white bar under the hovered tab. Focus is the 2px Gold Lift ring drawn 8px inside the tab, clear of the marker. While a launch runs the sections and the account chip are disabled at 50% and the marker turns ink-4 at full opacity (a half-faded gold read as bronze; the running launch segment keeps the only gold); the window buttons keep working.
- **Forced colours:** the rail is solid Canvas, the track CanvasText, the marker Highlight on a Canvas keyline, the lockup and the G drawn in CanvasText, the demo pill outlined.

### Play hero
- **Structure:** bottom-left: emblem slot beside the display name; under it the count block: the live number at figure size, "Online now" with the green dot and "of N slots" stacked beside it, and a 3px capacity bar (16% white track, ink fill set by `--fill`, 600ms ease); a meta row (region, mode, ping) split by 20% hairlines, demo data labelled with a chip; then the PLAY plate. A why-not line (14px, 36ch) appears only when PLAY is closed; stale data shows an ember line.
- **Offline / maintenance / checking:** the count block is replaced by the dot and a word (Offline, Maintenance, Checking server); the maintenance dot is ember, offline and unknown are hollow rings.

### Buttons (plates)
- **Shape:** chamfered rectangle, 8px cut (14px on the 64px PLAY plate).
- **PLAY:** 64px tall, `clamp(240px, 21vw, 300px)` minimum width, 30px caps, gold, On Sun label. Busy: stays gold, label kept, a 2px current-colour bar sweeps the bottom edge every 1.1s, cursor progress.
- **Primary:** gold, On Sun label; Gold Lift on hover, Gold Pressed on press. One per form or screen.
- **Ghost:** Raised Panel fill inside a 20% edge; hover lifts the edge to 42% and the fill to Slot Grey.
- **Quiet:** no fill, ink-2; hover a 7% white wash.
- **Danger:** red label on a near-black red fill inside a 60% red edge.
- **Disabled:** Slot Grey fill, ink-4 label.
- **Focus:** a 2px ring inside the chamfer (an outside outline is clipped by the cut). Ghost and danger: the edge turns Gold Lift and thickens to 2px. Primary: three layers inside the chamfer: an ink outer ring, a 2px On Sun gap, then the gold fill inset 4px (ink straight on gold is only 1.8:1; the dark gap makes both edges read). Quiet: a hollow 2px Gold Lift ring that follows the cut. Unchamfered controls (cards, tabs, links): 2px Gold Lift outline, 3px offset.
- **Press:** primary sinks to Gold Pressed, ghost's fill drops back to Raised Panel, danger's sinks below its rest fill, quiet's wash to 4%.
- **Edges:** a quiet plate that ends a row or starts an action line is outdented by its padding, so its label, not its wash, sits on the column edge.
- **Forced colours (High Contrast):** the cut is dropped, plates and toggles get a ButtonText border, focus is a 2px Highlight outline 2px outside, and the toggle knob, active tab bar and chosen card frame are repainted in system colours.

Fluid layout steps (`gutter`, `column`, `emblem`, `floor`, the launch floor and the card height) are rounded to whole pixels where `round()` is supported, so plate edges and rings never land on half pixels.

### Server and product cards
- **Style:** a GTA V screenshot (the server's own loop frame, or the product still) filling the card, `clamp(76px, 10.4vh, 94px)` tall, darkened from the left (90% to clear) where the 22px name and 12px meta sit; the state or price sits right in a near-black tag. Server cards carry a 3px capacity bar at the bottom.
- **States:** rest at slightly reduced saturation and brightness with a 7% frame; hover brings full colour, scales the image 3.5% and lifts the frame to 20%; chosen: full colour and a 2px gold inner frame, with the capacity bar moved inside the frame and filled in ink; offline or in maintenance: greyscale at 42% brightness, name in ink-2, capacity bar empty. Focus (always on the chosen card, roving tabindex): a 2px ink ring 4px inside the gold frame.

### Launch overlay
- **Structure:** over the same footage, still playing: emblem and server name at display size in the hero's position; a phase chip (Launching, In game, Stopped in red) and the elapsed clock; a five-segment rail named JOIN FILES STORE GAME LOAD (4px segments, 18% track, 12px caps names); the current step at launch-step size; a fine line saying the launch carries on in the tray.
- **Segments:** done = solid ink, current = gold fill that sweeps on a 2.4s loop, pending = track only. Launched shows a lead and a ghost "Back to the client"; failed drops the rail and shows the reason with the next action as plates.

### Ledger and owned pages
- **Summary column:** opaque panel; blocks for wallet, VIP (a tick bar of 1px-gapped segments, ink on, 50% part, 12% off) and spend, split by 7% hairlines.
- **Record:** an opaque panel with a 7% inset hairline; 26px title, 12px caps column heads over a 12% rule, rows split by 7% rules; date 14px condensed, total 17px/700 condensed, state as a chip with any reason on a 12px line under it and a "Pay" link for awaiting payment.
- **Owned shelf:** tiles `minmax(260px, 1fr)`, a 16:9 still above a Raised Panel body (kind label, 22px name, chips); the last slot is a hairline frame pointing back to the showroom.

### Auth panel
- **Style:** opaque Panel Black, 14px cut, `26px 28px 24px` padding, vertically centred on the right over the dusk loop; 40px caps title; fields; full-width gold submit; an "OR" rule; full-width ghost plates for the demo account and Discord; underlined text links. The bottom-left carries the emblem, the wordmark and the condensed tagline.

### Inputs / Fields
- **Style:** 44px, Night Ground fill, 1px 12% border, square; 12px caps label above, 13px hint below.
- **Focus:** 2px Gold Lift outline replaces the border. **Error:** red border and a red 13px message led by the authored alert mark.

### Chips
- **Style:** 22px straight-edged tag, 78% Panel Black, 20% inset edge, 12px/700 caps, ink-2.
- **Variants:** done (ink with a green tick), pending and warn (ember text, 50% ember edge), void (red text, 50% red edge), demo (ink-3). Never rotated.

### Toggles
- **Style:** a 60x28 Slot Grey slot with the 6px cut, a 24x20 square knob in ink-4 (dim, so OFF never reads as lit; 3.6:1 on the slot), OFF/ON in 10px caps. On: the slot goes to 30% warm white and the knob slides 28px and turns ink. Not gold. Focus: a hollow 2px Gold Lift ring inside the slot, following its cut.

### Settings index
- 17px condensed caps jumps with a 12px dash that grows from half to full on the current section; the panel's top edge fades its scrolling content over 26px.

### Motion

- **Screen entrance:** 480ms, `cubic-bezier(0.16, 1, 0.3, 1)`, from 0 opacity and 8px lower. Display titles rise 0.16em over 640ms.
- **Footage:** a new scene crossfades over the old in 900ms (`cubic-bezier(0.65, 0, 0.35, 1)`); the video fades in over its own first-frame poster in 200ms. Stills drift over 48s (scale 1.06, about 1.2% pan, alternate). Loops are 3.7-5.4s single shots made seamless. Footage and emblem pause when the window is hidden, idle and unfocused, or in game.
- **Server pick:** the emblem's enamel pulses (to 0.42 charge in 60ms, back over 300ms) as the footage crossfades.
- **PLAY:** a short launch sound; the emblem charges gold-white (0.46s), the footage pushes in to 1.05 over 1.4s and keeps playing, the screen fades out from 0.32s over 0.3s, the launch overlay fades in over 520ms at 0.64s. Returning eases the push back over 0.6s.
- **Launch rail:** completed segments fill in 500ms; the current one sweeps gold on a 2.4s loop.
- **Reduced motion:** posters and stills only (no video, no drift), no PLAY push (the launch covers at once), every animation and transition cut to 1ms.

## Do's and Don'ts

### Do:
- **Do** put real GTA V footage or an official still behind every screen, full-bleed, one held continuous shot per loop, with its source recorded in `ASSETS.md`.
- **Do** grade only the edges the type stands on, and drop reading surfaces to a flat 92% veil.
- **Do** set every name at display scale in Extra Condensed 800 caps on the bottom-left corner, with the emblem beside it on Play, Auth and the launch.
- **Do** spend gold on the one active choice per screen and keep everything else ink (The Rationed Gold Rule).
- **Do** cut panels and plates with the single 14px or 8px top-right chamfer.
- **Do** show demo or stale data with a chip or line that says so.
- **Do** give state colour only to platform states: ember for unsettled or needing action, red for failed, expired, reversed, banned or stopped, neutral for refunded.

### Don't:
- **Don't** build a home-made 3D scene, a film strip or any drawn stand-in for the city; the owner rejected exactly that ("just dogshit 3d wannabe").
- **Don't** add real-time 3D beyond the chrome G emblem.
- **Don't** cut loops into a montage.
- **Don't** use glass, backdrop blur, border radius or neon glow on panels, plates, chips or cards.
- **Don't** show Rockstar or GTA V logos, Pricedown, or anything that presents the client as an official Rockstar product.
- **Don't** use green for anything but the online dot.
- **Don't** set sentences in the condensed face.
