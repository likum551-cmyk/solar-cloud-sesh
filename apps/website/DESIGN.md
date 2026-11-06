---
name: Solar RP website (solar-rp.com)
description: The public site of the Solar RP GTA V roleplay network, "Big RP project" world (landing page, 2026-11-06). The log-in, sign-up and account pages share the base tokens only.
colors:
  sun: "#f2b134"
  sun-hi: "#ffc75a"
  sun-lo: "#c98d16"
  on-sun: "#1a1206"
  ember: "#ff7a2f"
  ember-deep: "#c2410c"
  rim-gold: "#ffd27a"
  band-gold: "#ffd166"
  rose: "#ff4f6d"
  luna: "#9cc2ff"
  terra: "#4fd18b"
  ground: "#0c0906"
  ground-base: "#0a0907"
  ember-dusk: "#2a1208"
  sunset-top: "#34160a"
  sunset-mid: "#7a2e0c"
  sunset-hot: "#b4470f"
  sunset-foot: "#5e250b"
  ember-top: "#24100a"
  ember-mid: "#4a1d0b"
  cream: "#f4ead8"
  cream-ink: "#1f150a"
  cream-ink-2: "#55432e"
  band-ink: "#f0dfc6"
  band-ink-bright: "#f7ead6"
  ink: "#f7f2e8"
  ink-2: "#d9d1c3"
  ink-3: "#b3aa9a"
  ink-4: "#7a7266"
  panel-3: "#201c16"
  line: "rgb(255 244 224 / 0.12)"
  line-2: "rgb(255 244 224 / 0.2)"
  line-3: "rgb(255 244 224 / 0.38)"
  alarm: "#ff8f6b"
typography:
  display:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "clamp(52px, 6.2vw, 88px)"
    fontWeight: 800
    lineHeight: 0.9
    letterSpacing: "0.005em"
  headline:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "clamp(44px, 5vw, 76px)"
    fontWeight: 800
    lineHeight: 0.92
    letterSpacing: "0.005em"
  server-name:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "clamp(72px, 8vw, 120px)"
    fontWeight: 800
    lineHeight: 0.85
    letterSpacing: "0.02em"
  figure:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "clamp(40px, 4vw, 60px)"
    fontWeight: 800
    lineHeight: 0.9
  title:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "30px"
    fontWeight: 800
    lineHeight: 1
    letterSpacing: "0.02em"
  step-title:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "23px"
    fontWeight: 700
    lineHeight: 1.25
  body:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "17px"
    fontWeight: 400
    lineHeight: 1.55
    fontFeature: "tnum"
  lede:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "19px"
    fontWeight: 400
    lineHeight: 1.55
  label:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "15px"
    fontWeight: 700
    lineHeight: 1
    letterSpacing: "0.12em"
  action:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "19px"
    fontWeight: 800
    lineHeight: 1
    letterSpacing: "0.06em"
rounded:
  none: "0px"
  dot: "50%"
spacing:
  gutter: "clamp(20px, 4vw, 64px)"
  edge: "max(clamp(20px, 4vw, 64px), calc((100vw - 1440px) / 2))"
  bar-h: "64px"
  band: "clamp(72px, 8vw, 120px)"
  head-gap: "clamp(36px, 4.5vw, 60px)"
  sm: "14px"
  md: "20px"
  lg: "28px"
  xl: "40px"
components:
  plate-primary:
    backgroundColor: "{colors.sun}"
    textColor: "{colors.on-sun}"
    typography: "{typography.action}"
    rounded: "{rounded.none}"
    height: "44px"
    padding: "0 22px"
  plate-primary-hover:
    backgroundColor: "{colors.sun-hi}"
  plate-primary-active:
    backgroundColor: "{colors.sun-lo}"
  plate-lg:
    backgroundColor: "{colors.sun}"
    textColor: "{colors.on-sun}"
    height: "56px"
    padding: "0 30px"
  plate-xl:
    backgroundColor: "{colors.sun}"
    textColor: "{colors.on-sun}"
    height: "68px"
    padding: "0 38px"
  plate-ghost:
    backgroundColor: "transparent"
    textColor: "{colors.ink}"
    height: "56px"
    padding: "0 30px"
  plate-disabled:
    backgroundColor: "{colors.panel-3}"
    textColor: "{colors.ink-3}"
  status-pill:
    backgroundColor: "rgb(12 9 6 / 0.72)"
    textColor: "{colors.ink}"
    typography: "{typography.label}"
    padding: "10px 16px 10px 14px"
  sol-state:
    backgroundColor: "{colors.sun}"
    textColor: "{colors.on-sun}"
    padding: "8px 12px"
  server-card-next:
    backgroundColor: "rgb(12 9 6 / 0.42)"
    textColor: "{colors.ink}"
    padding: "22px 26px"
  quest-card:
    backgroundColor: "rgb(12 9 6 / 0.55)"
    textColor: "{colors.ink}"
    padding: "24px 26px"
  step-number:
    backgroundColor: "{colors.sun}"
    textColor: "{colors.cream-ink}"
    size: "72px"
  needs-list:
    backgroundColor: "rgb(12 9 6 / 0.78)"
    textColor: "{colors.ink}"
    padding: "6px 22px"
---

# Design System: Solar RP website

## Overview

**Creative North Star: "Golden Hour Over Los Santos"**

A big, warm RP project home that is already moving when it opens. The page never sits on one ground: it walks from a sunset photo through an ember reel, a sunset band, a cream band and a second ember band, and closes on the river at golden hour. Bands meet edge to edge, joined only by short gradient fades of the ember-dusk tone, so no viewport reads as black. Gold and ember light lives inside each band as soft radial glows; it never floats over the page as a full-window layer.

Imagery is in-game footage only, framed in the S mark's 45-degree top-right cut with a gold rim that follows the diagonal. The servers are drawn with the project's own SVG emblems (sun, moon, earth on a cut badge), never GTA key art. Names and numbers are tall Sofia Sans Extra Condensed 800 caps; sentences are Sofia Sans. Motion is CSS transform and opacity loops plus scroll reveals; a script only drives counters, reveals and pointer parallax, and reduced motion stops all of it.

The confirmed rejections are the owner's: the dark quiet hub ("grim asf"), the 3D S, hero video, and any default GTA poster as server art.

**Key Characteristics:**
- Per-section grounds: photo, ember, sunset, cream, ember, photo.
- Gold and ember radial glows contained inside their band.
- One silhouette everywhere: the top-right 45-degree cut (clip-path), on plates, cards, pills, numbers and photos.
- Gold-rim cut frames on every in-game photo outside the reel.
- Own emblems with idle loops (rays turn, stars twinkle, land drifts, coin flips).
- Extra Condensed 800 caps for names and numbers; Sofia Sans for prose; tabular numerals throughout.

## Colors

A sunset palette: gold action, ember and rose light, brown-orange grounds, one cream band, and two server hues reserved for their emblems.

### Primary
- **Solar Gold** (sun): the one action colour (every plate), live and accent words in headlines (`.hot`), tally figures, quest reward, the SOL emblem, the online dot. Hover lifts to **Sun High** (sun-hi), press settles to **Sun Low** (sun-lo); text on gold is **Burnt Ink** (on-sun).

### Secondary
- **Ember** (ember): the warm end of every gold gradient (photo rims, step numbers) and the second stop of glows.
- **Deep Ember** (ember-deep): the accent word and the step line's far end on the cream band, where gold would not hold contrast.
- **Rim Gold** (rim-gold): the light end of the photo-frame rim gradient (135deg, rim-gold to ember).
- **Band Gold** (band-gold): accent words, the mode line and fact labels on the sunset band, where plain sun sinks into the orange.
- **Sunset Rose** (rose): only as a low-alpha second glow (0.22) in the hero and on the sunset band.

### Tertiary
- **Luna Moonlight** (luna) and **Terra Green** (terra): each server card's `--col`, used for its emblem fill, badge stroke, tag text and a 60 % rim. Never used outside its own card.

### Neutral
- **Landing Ground** (ground, #0c0906 on the landing page; the shared base is ground-base #0a0907): the page floor and the colour the hero and close fade into. Dark scrims are this colour at 0.42-0.86 alpha.
- **Ember Dusk** (ember-dusk): the seam colour; the reel fades into it, the sunset band and the river close fade out of it.
- **Sunset band stops** (sunset-top, sunset-mid, sunset-hot, sunset-foot) at 170deg; **Ember band stops** (ember-top, ember-mid, ember-dusk) at 160deg.
- **Cream** (cream) with **Cream Ink** and **Cream Ink 2**: the one light band (how to join).
- **Band Ink** (band-ink) and **Band Ink Bright** (band-ink-bright): prose on the sunset and ember bands.
- **Ink 1-4** (ink to ink-4): text on dark photo grounds, from headline to muted meta.
- **Lines** (line, line-2, line-3): hairlines, the needs list ring, outlined (ghost) plate rings.
- **Alarm** (alarm): errors and maintenance state only, never decoration.

### Named Rules
**The Ground Walks Rule.** Every section owns its ground (photo, ember gradient, sunset gradient or cream). A new landing section picks one of these grounds; it never sits on bare ground with a glow on it.

**The Glow Stays Home Rule.** Glows are radial gradients inside a band or behind one element (the gold call's breathing blob, the stack's sun). No fixed or full-window glow, grain or blend layer.

**The One Gold Action Rule.** Solid gold fill means "act". The quiet action is the ghost plate (outlined with line-3), never a second gold.

## Typography

**Display Font:** Sofia Sans Extra Condensed (with Arial Narrow, sans-serif), self-hosted, Latin and Cyrillic.
**Body Font:** Sofia Sans (with system-ui, sans-serif), self-hosted, Latin and Cyrillic.

**Character:** a tall, tight poster condensed for names, numbers and calls, against a calm humanist sans for everything a player reads as a sentence.

### Hierarchy
- **Display** (800, clamp(52px, 6.2vw, 88px), 0.9, uppercase): the hero H1 (max 15ch) and the close title (max 12ch), with a soft text shadow over photos.
- **Headline** (800, clamp(44px, 5vw, 76px), 0.92, uppercase, balanced): section titles.
- **Server name** (800, clamp(72px, 8vw, 120px), 0.85): SOL; the "next" names run at clamp(40px, 3.6vw, 54px).
- **Figure** (800, clamp(40px, 4vw, 60px), 0.9, gold): tally numbers; the quest reward runs larger at clamp(60px, 5.6vw, 88px) with a 0.32em caps unit.
- **Title** (800, 30px, uppercase): quest titles. Step titles are Sofia Sans 700 23px/1.25, sentence case.
- **Body** (400, 17px, 1.55, tabular numerals): prose; ledes at 19px, max 44-46ch; card prose 16-18px, max 36-52ch. Paragraphs use `text-wrap: pretty`.
- **Label** (700, 14-18px, 0.1-0.14em, uppercase, Extra Condensed): tally terms, fact and needs terms, tags, pill text, nav (19px, 0.06em).
- **Action** (800, 19px; 17 / 20-24 / 22-28px for sm / lg / xl, 0.06em, uppercase): plate labels.

### Named Rules
**The Caps Are For Names Rule.** Extra Condensed caps carry names, numbers, labels and calls; a sentence is always Sofia Sans in sentence case.

## Layout

One left edge for the bar, every section and the footer: the gutter (clamp(20px, 4vw, 64px)), or a centred 1440 px measure on wide screens. The fixed 64 px bar is a fading scrim over the hero and turns opaque ground (with a 1 px line) once the page scrolls 8 px.

Sections are full-bleed bands with vertical padding of clamp(72px, 8vw, 120px) (servers and close a little tighter); the hero is at most min(100svh, 860px). Section heads are a max-760px stack with clamp(36px, 4.5vw, 60px) beneath. Two-column splits are fractional minmax(0, …) grids: hero 1.05 / 0.95 at 1000 px, quests 1 / 1 and close 1.2 / 0.8 at 960 px, SOL card emblem / copy at 760 px plus a 240-300 px facts column at 1100 px, next cards 2-up at 700 px, steps 3-up at 860 px. Gaps step through 14, 20, 22, 28 and 40 px.

Below 1000 px the photo stack moves above the copy; below 760 px CTAs go full-width, the hero and close scrims become a full-width vertical shade, quest and needs rows stack; below 560 px the section nav hides and only the account link and Download remain.

## Elevation & Depth

Depth comes from light and overlap, not from cards lifting off a surface. Photos float on soft, large drop shadows; glows sit behind the thing they light; dark translucent scrims (ground at 0.42-0.86) separate copy from busy pictures. Box shadows are not used for elevation because the cut silhouette (clip-path) would slice them; rings are drawn as evenodd polygon pseudo-elements instead.

### Shadow Vocabulary
- **Photo float** (`filter: drop-shadow(0 30px 40px rgb(0 0 0 / 0.5))`, 0.45 on the quests photo): tilted in-game photos.
- **Coin glow** (`filter: drop-shadow(0 8px 18px rgb(242 177 52 / 0.45))`): the quest coin.
- **Headline lift** (`text-shadow: 0 4px 30px rgb(0 0 0 / 0.35)`, 0.4 on the close): display type over photos; nav text uses `0 1px 12px rgb(0 0 0 / 0.7)`.
- **Gold breath** (radial-gradient(closest-side, rgb(255 170 60 / 0.55), rgb(255 122 47 / 0.18) 60%, transparent) on an 18 x 26 px outset blob): behind the primary Download only.

### Named Rules
**The Light Not Lift Rule.** Elements gain presence through warm light behind them and a soft drop shadow on photos; nothing uses a hard or offset shadow.

## Shapes

Square everywhere, with one cut: the top-right corner is clipped at 45 degrees (`polygon(0 0, calc(100% - c) 0, 100% c, 100% 100%, 0 100%)`), the cut of the S mark. The cut scales with the object: 8-16 px on plates (xs to xl), 9-10 px on small plates and pills, 14 px on step numbers, 18 px on reel stills, 20 px on next cards, 22 px on photo frames and quest cards, 30 px on the SOL card and the large quests photo. The emblem badge is the same cut in SVG (`M2 2H94L118 26V118H2Z`).

Outlines follow the cut: a ring is a pseudo-element clipped to an evenodd polygon whose hole is inset by the ring width, the diagonal's inner points moved by 0.4142 x width. Photo rims are the frame's own gradient fill showing through 3 px of padding, with the image's cut reduced by 1.24 px so the rim stays even on the diagonal. The only round shapes are status dots and the emblems' bodies.

## Components

### Buttons (plates)
- **Shape:** the top-right cut, no radius (12 px cut at the 44 px base).
- **Primary:** Solar Gold fill, Burnt Ink caps label, 44 / 38 / 56 / 68 px heights for base / sm / lg / xl.
- **Hover / Focus:** hover to Sun High, press to Sun Low with a 1 px drop (160 ms); focus is a cut-following double ring, 2 px on-sun outside 2 px ink.
- **Ghost:** transparent with a line-3 cut ring, ink label; hover tint rgb(255 244 224 / 0.07).
- **Soon / disabled:** when the release feed fails, plates turn to the outlined ghost look (`is-soon`); a disabled plate is panel-3 with ink-3 text.
- **Breathing call:** the primary Download sits in a glow wrapper whose blob breathes (3.2 s) behind the plate.

### Chips (status pill and state tag)
- **Status pill:** dark scrim (ground 0.72), 10 px cut, gold 0.45 cut ring, caps label: gold state with dot, ink server name, ink-2 meta.
- **Dot:** shown only for a measured state: online is gold with a pulsing halo (a ::after ring scaling to 3.2x while fading, 2 s; transform and opacity only), offline ink-4, maintenance alarm; open and next carry no dot.
- **SOL state tag:** solid gold, on-sun caps, no cut.

### Cards / Containers
- **Corner Style:** the top-right cut (20-30 px).
- **Background:** dark translucent scrims over the band (ground at 0.25-0.6), the SOL card as a 120deg 0.55-0.25 gradient, the quest card as a gold-to-dark 135deg gradient.
- **Shadow Strategy:** none; a 1 px cut ring in the card's colour (band gold 0.6, the server colour at 60 %, gold 0.6).
- **Internal Padding:** 22-26 px; SOL clamp(28px, 3.4vw, 48px).
- **Hover:** next cards rise 4 px and darken their scrim to 0.6 (400 ms, transform and background).

### Inputs / Fields
Not on the landing page; the account pages own them in account.css.

### Navigation
- Fixed bar: lockup at 28 px tall left, caps section links (Extra Condensed 700 19px, ink-2, hover ink) right, then the account link and a small Download plate. Gradient scrim at rest, opaque ground once scrolled. Below 560 px only the account link and Download remain.

### Gold-rim photo frame (signature)
In-game photo at 16:9, object-fit cover, inside a cut frame whose 3 px rim is a 135deg gradient from rim gold to ember. Used for the hero stack and the quests photo; reel stills use the cut without a rim.

### Hero photo stack (signature)
Three framed photos tilted -5, 4 and -1.5 degrees, overlapping, over a breathing sun glow; they arrive once (1100 ms, staggered 150 ms), float on 7-9 s loops, and on a fine pointer translate by depth factors 26 / 14 / 8 (600 ms ease).

### Server emblems (signature)
120 x 120 SVG on the cut badge (fill: the server colour 16 % over ground 0.55, 1.5 px stroke in the colour). Sun: body with the S mark and turning rays (30 s; 6 s on hover). Moon: masked crescent with twinkling stars. Earth: green disc with drifting continents (14 s). 96 px in "next" cards (76 px on phones), clamp(150px, 16vw, 230px) for SOL.

### Steps
Gold-to-ember cut numbers (72 px, 40px numerals) joined by a 2 px sun-to-deep-ember line (across at 860 px and up, down between numbers on phones) that draws itself in, then each number turns in and its text rises, staggered 220 ms.

### Tally and facts
Hairline-topped columns (1 px gold 0.4): a caps term, a gold figure that counts up once in view (1.6 s, ease-out quart; the markup already holds the final value), a short note. Facts and needs lists are term / value rows divided by 1 px lines.

### Reel
One row of cut stills (clamp(280px, 30vw, 460px)) duplicated once and translated -50 % over 60 s, paused on hover; a hovered still scales 1.04 and saturates.

## Do's and Don'ts

### Do:
- **Do** give every new landing section one of the established grounds (photo under a scrim, ember gradient, sunset gradient, cream) and join it to its neighbour with an ember-dusk fade.
- **Do** cut every plate, card, pill, number and photo at the top-right 45 degrees, scaled to the object (8-30 px), and draw outlines as cut-following rings.
- **Do** frame in-game photos in the gold-rim cut frame; use in-game footage only.
- **Do** keep loops on transform and opacity, start them under the `.enter` class, and stop every animation and transition under `prefers-reduced-motion: reduce` with content left in its final, visible state.
- **Do** put the final value in the markup of anything that counts or reveals, so a page whose script never runs still reads right.
- **Do** keep the solid gold fill for the one action; the secondary action is the ghost plate.
- **Do** use Deep Ember for accents on cream and Band Gold for accents on the sunset band.

### Don't:
- **Don't** use GTA key art or a default GTA poster to represent a server; servers get their own emblems.
- **Don't** add a fixed or full-window glow, grain or blend layer over the page.
- **Don't** use video or WebGL on the landing page.
- **Don't** round corners or use box shadows on cut shapes; the clip-path cuts them off.
- **Don't** use alarm for anything but errors and the maintenance state.
- **Don't** use Luna or Terra outside their own server card.
- **Don't** show an unmeasured number: a live dot or player count appears only when the server reports it.
