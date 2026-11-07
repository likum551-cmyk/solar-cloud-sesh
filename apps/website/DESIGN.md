---
name: Solar RP website (solar-rp.com)
description: The front door of Solar Launcher, a calm technical product page on charcoal whose hero holds SOL live (landing page, 2026-11-07). The log-in, sign-up and account pages still run on base.css and are not part of this system yet.
colors:
  sun: "#f2b134"
  sun-hi: "#ffc75a"
  sun-lo: "#c98d16"
  on-sun: "#1a1206"
  ground: "#0d0e10"
  black: "#08090a"
  panel: "#15171b"
  panel-2: "#1b1e23"
  panel-3: "#272b31"
  ink: "#f3f4f6"
  ink-2: "#ccd0d6"
  ink-3: "#a2a7b0"
  ink-4: "#6a6f78"
  line: "rgb(255 255 255 / 0.08)"
  line-2: "rgb(255 255 255 / 0.14)"
  line-3: "rgb(255 255 255 / 0.28)"
  fill-1: "rgb(255 255 255 / 0.07)"
  fill-2: "rgb(255 255 255 / 0.12)"
  fill-3: "rgb(255 255 255 / 0.16)"
  ok: "#4fd18b"
  warn: "#f2b134"
  down: "#ff8f6b"
  luna: "#9cc2ff"
typography:
  display:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "clamp(42px, 5vw, 74px)"
    fontWeight: 800
    lineHeight: 1.02
    letterSpacing: "-0.03em"
  headline:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "clamp(34px, 3.6vw, 52px)"
    fontWeight: 800
    lineHeight: 1.06
    letterSpacing: "-0.025em"
  title:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "clamp(26px, 2.4vw, 34px)"
    fontWeight: 800
    lineHeight: 1.12
    letterSpacing: "-0.02em"
  subtitle:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "21px"
    fontWeight: 700
    lineHeight: 1.25
    letterSpacing: "-0.01em"
  server-name:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "26px"
    fontWeight: 800
    lineHeight: 1
    letterSpacing: "0.02em"
  figure:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "22px"
    fontWeight: 800
    lineHeight: 1
    letterSpacing: "-0.01em"
    fontFeature: "tnum"
  lede:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "18px"
    fontWeight: 400
    lineHeight: 1.6
  body:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "17px"
    fontWeight: 400
    lineHeight: 1.55
    fontFeature: "tnum"
  nav:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "15px"
    fontWeight: 600
    lineHeight: 1
    letterSpacing: "0"
  button:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "16px"
    fontWeight: 700
    lineHeight: 1
    letterSpacing: "0"
  label:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "12px"
    fontWeight: 600
    lineHeight: 1.3
  mono:
    fontFamily: "ui-monospace, Cascadia Mono, Consolas, monospace"
    fontSize: "12.5px"
    fontWeight: 500
    lineHeight: 1.6
rounded:
  none: "0px"
spacing:
  section: "clamp(88px, 9vw, 136px)"
  gutter: "clamp(20px, 4vw, 64px)"
  bar: "64px"
  xs: "8px"
  sm: "12px"
  md: "20px"
  lg: "30px"
  panel-inset: "clamp(28px, 4vw, 48px)"
components:
  button-primary:
    backgroundColor: "{colors.sun}"
    textColor: "{colors.on-sun}"
    typography: "{typography.button}"
    rounded: "{rounded.none}"
    padding: "0 22px"
    height: "46px"
  button-primary-hover:
    backgroundColor: "{colors.sun-hi}"
  button-primary-active:
    backgroundColor: "{colors.sun-lo}"
  button-primary-focus:
    backgroundColor: "{colors.ink}"
    textColor: "{colors.black}"
  button-primary-sm:
    padding: "0 16px"
    height: "36px"
  button-primary-lg:
    padding: "0 26px"
    height: "54px"
  button-primary-xl:
    padding: "0 32px"
    height: "62px"
  button-ghost:
    backgroundColor: "rgb(13 14 16 / 0.28)"
    textColor: "{colors.ink}"
    typography: "{typography.button}"
    rounded: "{rounded.none}"
    padding: "0 22px"
    height: "46px"
  button-ghost-hover:
    backgroundColor: "{colors.fill-1}"
  button-ghost-active:
    backgroundColor: "{colors.fill-2}"
  button-disabled:
    backgroundColor: "{colors.panel-3}"
    textColor: "{colors.ink-3}"
  server-card:
    backgroundColor: "rgb(21 23 27 / 0.94)"
    textColor: "{colors.ink}"
    rounded: "{rounded.none}"
    padding: "22px 22px 18px"
    width: "400px"
  panel:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink-2}"
    rounded: "{rounded.none}"
    padding: "{spacing.panel-inset}"
  panel-strip:
    backgroundColor: "{colors.panel-2}"
    textColor: "{colors.ink}"
    padding: "20px 30px 22px"
  download-panel:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink}"
    rounded: "{rounded.none}"
    padding: "clamp(32px, 4vw, 52px) clamp(28px, 4vw, 56px)"
---

# Design System: Solar RP website

## Overview

**Creative North Star: "The Launcher's Front Door"**

The site is the launcher seen from outside: a calm, technical product page on neutral charcoal, in the register of fivem.net and alt:V rather than a trailer or a fan poster. One in-game photo world, a bright sunset still of Vespucci Beach, opens the page full-bleed and closes it under the download panel; everything between is charcoal panels, hairlines and sentence-case Sofia Sans. The hero's right column holds SOL as the launcher shows it, live, so the first thing a visitor learns is that the server is up and how many people are on it.

Density is moderate and quiet. Copy is short, in sentence case, and sits on panels or directly on the ground; the gold of the S mark is rationed to what a visitor can act on and to the few figures that matter (capacity, the coin reward). Shapes come from the mark itself: every button, panel and picture is cut at 45 degrees on the top-right and bottom-left corners, the way the S is, and nothing is rounded.

This world replaces three rejected ones (the trailer page, the quiet community hub, and the warm "Big RP project" home): the warm sunset/ember/cream section grounds, floating tilted photos, photo marquees, ticking hero numbers and condensed all-caps slogans of those rounds are confirmed rejections.

**Key Characteristics:**
- Neutral charcoal ground with three panel steps; no warm tint anywhere in the chrome.
- One accent, the mark's gold, on actions, the capacity fill, the 2 px top rule of the two key panels and the coin reward.
- Sofia Sans only: 800 with negative tracking for headings, 400 for sentences, sentence case everywhere.
- The S-mark cut (top-right and bottom-left, 45 degrees) on every rectangle; no border-radius.
- A live server card is the signature: real status, real player count, an age that ticks.
- Motion is a single rise on load, one slow push-in of the hero photo on desktop, and rows that fade in once.

## Colors

A cool, nearly neutral charcoal scale with one warm voice, the gold of the S mark, and the launcher's own three status colours.

### Primary
- **Solar Gold** (sun): the fill of every primary button, the capacity bar inside the server card, the 2 px top rule of the SOL card and the download panel, the coin reward figure, and the brand marks (the lower half of the S, the SOL emblem). Hover lifts it to **Noon Gold** (sun-hi), press drops it to **Dusk Gold** (sun-lo).
- **Gold Ink** (on-sun): text and icons on gold, nothing else.

### Secondary
- **Status Green** (ok): the live dot of an open server, and the TERRA emblem.
- **Status Amber** (warn): maintenance. It is the same hex as Solar Gold, so a maintenance state reads as the brand colour; keep it to the dot.
- **Status Coral** (down): the offline dot and download error text.
- **Moonlight** (luna): the LUNA emblem only.

### Neutral
- **Charcoal Ground** (ground): page background, scrollbar track, the solid top bar, the footer, the end of every photo scrim.
- **Near Black** (black): text on the ink-filled focus state of a button.
- **Panel** (panel): the base of every panel (feature text row, join panel, download panel, picture frames while loading); the SOL card uses it at 94 % over the photo.
- **Panel Raised** (panel-2): the "what you need" strip under the join steps.
- **Panel High** (panel-3): the capacity track, the disabled button, the scrollbar thumb.
- **Ink** (ink): headings, button text on dark, live values in the server card.
- **Ink Soft** (ink-2): body copy, ledes, list items, quiet nav after scroll.
- **Ink Muted** (ink-3): labels (dt), step numerals, the "updated" age, the FAQ chevron.
- **Ink Faint** (ink-4): list tick marks only; never text.
- **Hairlines** (line 8 %, line-2 14 %, line-3 28 % white): line is the bar's bottom edge; line-2 divides rows, facts, steps and FAQ items; line-3 is the ghost button's outline.
- **Fills** (fill-1 7 %, fill-2 12 %, fill-3 16 % white): ghost button hover and press, the "soon" button.

### Named Rules
**The Rationed Gold Rule.** Gold marks only what you can press, the capacity fill, the top rule of the SOL card and the download panel, the coin reward, and the brand marks. Section grounds, headings, labels and decorative lines are never gold.

**The Launcher Parity Rule.** Status colours are the launcher's: green up, amber maintenance, coral down. Do not invent a fourth or recolour them for the site.

**The Neutral Ground Rule.** The chrome is neutral charcoal. Warmth comes from the photograph and the gold, never from a tinted section band.

## Typography

**Display Font:** Sofia Sans (with system-ui, sans-serif)
**Body Font:** Sofia Sans (with system-ui, sans-serif)
**Label/Mono Font:** ui-monospace, Cascadia Mono, Consolas, for the installer SHA-256 only

**Character:** one humanist grotesque doing everything: heavy and tightly tracked for headings so they read as product statements, plain 400 for sentences, tabular figures by default so live numbers do not jitter.

### Hierarchy
- **Display** (800, clamp(42px, 5vw, 74px), 1.02, -0.03em): the hero H1 only; 38 px on phones. Balanced wrap; a soft text shadow over the photo, dropped on phones where the copy sits below the picture.
- **Headline** (800, clamp(34px, 3.6vw, 52px), 1.06, -0.025em): section titles ("How SOL plays", "How to join", "Questions", "Get Solar Launcher").
- **Title** (800, clamp(26px, 2.4vw, 34px), 1.12, -0.02em): feature row titles.
- **Subtitle** (700, 18-21px, 1.2-1.35): step titles (21), the quest title (19), FAQ questions (18).
- **Server name** (800, 26px, 0.02em): the server's own name in the card; the one place caps appear, because SOL is the name.
- **Figure** (800, 22-26px, tabular): the live player count (22) and the coin reward (26, gold).
- **Lede** (400, 17-19px, 1.55-1.6, 44-46ch): hero lede, feature text, download lede; ink-2 except the hero lede over the photo (ink).
- **Body** (400, 17px, 1.55): base text; list items 16 px, FAQ answers 17 px at 62ch.
- **Nav and buttons** (600 / 700, 15-18px, letter-spacing 0): sentence case.
- **Label** (600, 12-13px, ink-3): dt labels in the card and the needs strip, the SHA-256 caption, the "updated" age.

### Named Rules
**The Sentence Case Rule.** Headings, nav, buttons and labels are sentence case at letter-spacing 0 or tighter. No condensed faces and no all-caps text on this page; server names are the only capitals, because they are names.

**The Tight Heading Rule.** Every heading is 700-800 with negative tracking (-0.01 to -0.03em) and `text-wrap: balance`; body text gets `text-wrap: pretty`.

## Layout

A centred column with a 1440 px content cap: margins are `max(gutter, (100vw - 1440px) / 2)` with the gutter at clamp(20px, 4vw, 64px). Sections are separated by one rhythm, clamp(88px, 9vw, 136px), and carry no background bands of their own.

- **Hero:** full-bleed still to min(100svh, 1080px), copy seated low (grid aligned to the end). Two columns, the copy (max 640 px) and the SOL card (320-400 px), gap clamp(40px, 6vw, 96px). Scrim: a left-to-right darkening to 58 % and a bottom fade into the ground.
- **Feature rows:** picture and text at 7:5, flipped to 5:7 on alternate rows; a row without a picture becomes a single cut panel with text left and the list right.
- **Join:** one panel holding three steps in equal columns divided by hairlines, with a raised strip of three requirements below; the quest row sits under the panel on a bottom hairline.
- **FAQ:** two columns of disclosure rows on hairlines.
- **Download:** the hero still returns under a vertical scrim, with one panel: mark, copy, action, and the SHA-256 across the bottom.
- **Top bar:** fixed, 64 px, transparent over the hero with a top fade, solid ground with a hairline after 8 px of scroll.

Breakpoints: at 1020 px the hero stacks (card capped at 520 px) and the download panel stacks without the mark; at 860 px feature rows, steps, needs and FAQ go to one column and the nav hides; at 560 px the hero photo becomes its own 4:3 band above the copy with no scrim, primary actions go full width, and the bar's Download button hides.

## Elevation & Depth

Flat. Depth is tonal: ground, panel, panel-2 and panel-3 step up in lightness, and hairlines separate rows inside a panel. There are no box shadows on panels or buttons. The only shadows are legibility aids over the photograph: text shadows on the hero title, lede and transparent-bar nav, a drop shadow on the lockup, and a 1 px hairline under the solid bar.

### Shadow Vocabulary
- **Hero title** (`text-shadow: 0 2px 24px rgb(0 0 0 / 0.35)`): display text over the photo only.
- **Hero lede** (`text-shadow: 0 1px 14px rgb(0 0 0 / 0.5)`): body text over the photo only.
- **Nav over photo** (`text-shadow: 0 1px 10px rgb(0 0 0 / 0.55)`): removed once the bar turns solid.
- **Lockup** (`filter: drop-shadow(0 1px 8px rgb(0 0 0 / 0.4))`).
- **Solid bar edge** (`box-shadow: 0 1px 0 rgb(255 255 255 / 0.08)`).

### Named Rules
**The Photo-Only Shadow Rule.** A shadow exists only to keep text readable over the photograph. Nothing on charcoal casts one.

## Shapes

The S mark's corner is the whole form language: a 45-degree cut at the top-right and bottom-left, drawn with `clip-path: polygon(0 0, calc(100% - c) 0, 100% c, 100% 100%, c 100%, 0 calc(100% - c))`. The cut depth scales with the object: buttons 8 px (small), 10 px (default), 12 px (large), 14 px (extra large); the SOL card 16 px; panels and pictures 18 px; the download panel 22 px. Rectangles never take a border-radius; the only round things are the status dot and the circular server emblems.

Because a clip-path slices off outlines and inset shadows at the diagonals, a line that must follow the cut is drawn as a second evenodd polygon on a pseudo-element, inset by its width, with the inner diagonal points moved by 0.4142 x the width (tan 22.5 degrees).

### Named Rules
**The Mark's Cut Rule.** Every button, panel and picture frame is cut top-right and bottom-left at 45 degrees, depth 8-22 px by size; never rounded, never cut on one corner only.

## Components

### Buttons
Solid, compact and cut like the mark.
- **Shape:** double 45-degree cut (8 / 10 / 12 / 14 px for sm / default / lg / xl), heights 36 / 46 / 54 / 62 px.
- **Primary:** gold fill, gold-ink text, Sofia Sans 700 sentence case, optional 18 px leading icon.
- **Hover / Active:** fill to sun-hi; press to sun-lo with a 1 px drop (160 ms, ease-out; no transition under reduced motion).
- **Focus:** the cut clips any outline, so focus fills the button with ink and turns the text near-black.
- **Ghost:** charcoal at 28 % with ink text and a 1 px line-3 ring that follows the cut (evenodd polygon); hover fill-1 and a 50 % white ring, press fill-2. Used for "Create account" beside the gold action and for the quest.
- **Disabled / soon:** panel-3 fill with ink-3 text; "soon" is fill-2 with ink-2.

### Cards / Containers
- **Corner Style:** the mark's cut, 16-22 px.
- **Background:** panel; panel-2 for a strip inside a panel; the SOL card at 94 % over the photo.
- **Shadow Strategy:** none (see Elevation).
- **Border:** none, except the 2 px gold top rule on the SOL card and the download panel; internal rows divide with line-2 hairlines.
- **Internal Padding:** 22 px (card), 30 px (steps), clamp(28px, 4vw, 48px) (text panel), up to 52 x 56 px (download).

### Navigation
Fixed 64 px bar: lockup left (24 px high), section links right in Sofia Sans 600 15 px sentence case, a "Log in" link and a small gold Download button. Over the hero the bar is a dark-to-clear fade with ink links and text shadows; after 8 px of scroll it is solid ground with a hairline and ink-2 links. Hover turns links sun-hi. Below 860 px the section links hide; below 560 px the bar's Download button hides (the hero's is full width).

### Feature rows and lists
A 16:9 picture in an 18 px cut frame beside a title, a short paragraph and a list. List items sit between line-2 hairlines with a 10 x 2 px ink-4 dash as the marker.

### Disclosure (FAQ)
Rows between line-2 hairlines, 18 px 700 question, a 16 px ink-3 chevron that turns 180 degrees when open (200 ms), sun-hi on hover, a 2 px sun-hi focus outline.

### Server card (signature)
SOL as the launcher shows it. Top row: the 48 px SOL emblem (gold disc, S cut out, 50 % gold ring), the name, a status line with a 9 px dot (green open, amber maintenance, coral offline) and an "updated N s ago" age in ink-3 at the top right. Below: the player count and a 4 px gold capacity bar on a panel-3 track, shown only when at least one player is on (a non-zero share is drawn at least 1.5 % wide). Then a facts row (mode, region, players, client version) in hairline-divided cells, a one-line instruction, and LUNA and TERRA as one planned line with their 18 px emblems.

Behaviour: it polls every 20 s while the tab is visible, the age ticks between polls, and the capacity fill eases to the new value over 900 ms (scaleX, no transition under reduced motion). The open dot breathes: a ring that scales to 2.8x and fades over 2.4 s, off under reduced motion.

### Quest row
A 44 px gold coin with the S, title and one-line description, the reward as a gold 26 px figure with an ink-2 unit, and a ghost button; on a bottom hairline, no panel.

### Download panel
The closing panel on the returning beach still: 22 px cut, 2 px gold top rule, the two-colour S mark, headline and lede, an extra-large gold button with the release line and status under it, and the SHA-256 in mono across the bottom above a hairline.

### Motion
One soft rise of the hero copy and the server card on load (900 ms, 16 px, ease-out, the card 120 ms later). One slow push-in of the hero photo (scale 1 to 1.04 over 24 s, once), desktop and tablet only: off at 560 px and below, where the photo is a band. Feature rows fade and rise 24 px once as they enter (800 ms). Transform and opacity only; all of it off under `prefers-reduced-motion`.

## Do's and Don'ts

### Do:
- **Do** keep gold to buttons, the capacity fill, the 2 px top rule of the SOL card and the download panel, the coin reward and the brand marks.
- **Do** cut every button, panel and picture at the top-right and bottom-left at 45 degrees (buttons 8-14 px, panels and pictures 16-22 px).
- **Do** draw a line that must follow the cut as an evenodd polygon ring on a pseudo-element, not a border or inset shadow.
- **Do** set headings in Sofia Sans 800 with negative tracking, sentence case; sentences in 400.
- **Do** show real data in the server card: the true player count, the bar only when someone is on, the age of the reading.
- **Do** separate rows inside a panel with 1 px hairlines at white 14 %, and step depth with panel, panel-2 and panel-3.
- **Do** use the launcher's status colours (green up, amber maintenance, coral down) for server state.
- **Do** keep motion to transform and opacity, and switch it all off under reduced motion.

### Don't:
- **Don't** use border-radius on rectangles, or cut only one corner.
- **Don't** use condensed faces or all-caps text for headings, nav or buttons.
- **Don't** tint section grounds with warm gradient bands (sunset, ember, cream); the chrome stays charcoal.
- **Don't** float or tilt photos, run photo marquees, or tick numbers up in the hero.
- **Don't** put box shadows on panels or buttons; shadows exist only for text over the photograph.
- **Don't** use gold for headings, labels, section backgrounds or decorative lines.
- **Don't** show a capacity bar or a player figure the server did not report.
