---
name: Solar RP website (solar-rp.com)
description: The public site of the Solar RP GTA V roleplay network: landing page, sign-up, log-in and account. Website only; the launcher has its own system in apps/launcher-ui/DESIGN.md.
colors:
  ground: "#0a0907"
  black: "#040302"
  panel: "#100e0b"
  panel-2: "#17140f"
  panel-3: "#201c16"
  ink: "#f7f2e8"
  ink-2: "#d9d1c3"
  ink-3: "#b3aa9a"
  ink-4: "#7a7266"
  line: "rgb(255 244 224 / 0.12)"
  line-2: "rgb(255 244 224 / 0.2)"
  line-3: "rgb(255 244 224 / 0.38)"
  sun: "#f2b134"
  sun-hi: "#ffc75a"
  sun-lo: "#c98d16"
  on-sun: "#1a1206"
  alarm: "#ff8f6b"
typography:
  display:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "clamp(48px, 6vw, 88px)"
    fontWeight: 800
    lineHeight: 0.92
    letterSpacing: "0.005em"
  headline:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "clamp(40px, 4.2vw, 62px)"
    fontWeight: 800
    lineHeight: 0.95
    letterSpacing: "0.01em"
  name:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "clamp(44px, 4.4vw, 64px)"
    fontWeight: 800
    lineHeight: 0.9
    letterSpacing: "0.02em"
  figure:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "44px"
    fontWeight: 800
    lineHeight: 0.9
    letterSpacing: "0.01em"
  title:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "26px"
    fontWeight: 800
    lineHeight: 1
    letterSpacing: "0.02em"
  plate-label:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "19px"
    fontWeight: 800
    lineHeight: 1
    letterSpacing: "0.06em"
  label-nav:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "19px"
    fontWeight: 700
    lineHeight: 1
    letterSpacing: "0.06em"
  label:
    fontFamily: "Sofia Sans Extra Condensed, Arial Narrow, sans-serif"
    fontSize: "16px"
    fontWeight: 700
    lineHeight: 1
    letterSpacing: "0.1em"
  body-lead:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "18px"
    fontWeight: 400
    lineHeight: 1.55
  body:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "17px"
    fontWeight: 400
    lineHeight: 1.55
    fontFeature: "\"tnum\" 1"
  body-strong:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "19px"
    fontWeight: 700
    lineHeight: 1.3
  body-small:
    fontFamily: "Sofia Sans, system-ui, sans-serif"
    fontSize: "14px"
    fontWeight: 400
    lineHeight: 1.55
rounded:
  none: "0"
  dot: "50%"
spacing:
  gutter: "clamp(20px, 4vw, 64px)"
  measure: "1440px"
  bar-h: "64px"
  section-y: "clamp(56px, 6vw, 96px)"
  head-gap: "clamp(28px, 3.6vw, 44px)"
  xs: "8px"
  sm: "12px"
  md: "16px"
  lg: "24px"
  xl: "32px"
  col: "48px"
  col-wide: "64px"
components:
  plate:
    backgroundColor: "{colors.sun}"
    textColor: "{colors.on-sun}"
    typography: "{typography.plate-label}"
    rounded: "{rounded.none}"
    height: "44px"
    padding: "0 22px"
  plate-hover:
    backgroundColor: "{colors.sun-hi}"
  plate-active:
    backgroundColor: "{colors.sun-lo}"
  plate-lg:
    height: "56px"
    padding: "0 30px"
  plate-xl:
    height: "68px"
    padding: "0 38px"
  plate-sm:
    height: "38px"
    padding: "0 16px"
  plate-disabled:
    backgroundColor: "{colors.panel-3}"
    textColor: "{colors.ink-3}"
  plate-ghost:
    backgroundColor: "transparent"
    textColor: "{colors.ink}"
    typography: "{typography.plate-label}"
    height: "44px"
    padding: "0 22px"
  status-panel:
    backgroundColor: "{colors.panel}"
    textColor: "{colors.ink}"
    padding: "24px 24px 20px"
  quest-card:
    backgroundColor: "{colors.panel-2}"
    textColor: "{colors.ink}"
    padding: "24px 28px"
  step-number:
    backgroundColor: "{colors.panel-3}"
    textColor: "{colors.ink}"
    size: "48px"
  field-input:
    backgroundColor: "{colors.panel-2}"
    textColor: "{colors.ink}"
    typography: "{typography.body}"
    rounded: "{rounded.none}"
    height: "50px"
    padding: "0 14px"
  chip:
    backgroundColor: "transparent"
    textColor: "{colors.ink-3}"
    padding: "5px 8px"
---

# Design System: Solar RP website (solar-rp.com)

Scope: this file governs `apps/website` only (the landing page, `/signup/`, `/login/`, `/account/`; tokens in `src/base.css`, landing rules in `src/styles.css`, account rules in `src/account.css`). The repo-root `PRODUCT.md` describes the launcher, and the launcher's visual system is `apps/launcher-ui/DESIGN.md` (blue accent, night ground). Neither governs this site, and this file does not govern the launcher or the in-game HUD.

## Overview

**Creative North Star: "The Project's Front Desk"**

A GTA RP project's home page, played straight: what the server is, whether it is up, how to get in, in that order. The page is a warm near-black ground with one dimmed in-game still at the top, a short column of sections divided by hairlines, condensed caps headings at human sizes, and one gold action. It reads like the big FiveM/RageMP projects' sites and a Steam store page: an info panel beside the pitch, facts in rows, steps in three columns, a download block that says what the file is and what it needs.

This world replaced a trailer cold open (letterbox, a three.js 3D S, giant title cards, footage loops, full-window blends) that the owner rejected three times on 2026-11-06, ending "this is a joke, FULL REDESIGN, remove this disgusting 3D S", after "too animated, make it more professional" and "too many videos, a bit laggy". Those rejections are standing rules here, not history.

Density is moderate and even: sections of 56-96px vertical padding, a 1440px measure, every claim backed by a live value or a shipped feature. Real GTA V stills carry all the colour; the chrome stays neutral warm ink.

**Key Characteristics:**
- One dimmed in-game still per page, never video, never a montage.
- Warm near-black ground, opaque panels, 1px warm hairlines; no glass, no blur.
- One gold: the primary action and a state the system has measured or confirmed.
- Sofia Sans Extra Condensed 800 caps for headings and labels at human sizes (hero max 88px); Sofia Sans for every sentence.
- The mark's single 45-degree top-right cut on plates and panels; no radius anywhere except the status dot.
- One authored motion: the hero copy and status panel rise once on load.

## Colors

A warm, low-chroma night ground with a single sun-gold accent and one warm alarm tone; the picture carries everything else.

### Primary
- **Sun Gold** (sun): the fill of every primary plate (Download, a form's single submit), the live dot when a server has reported itself online, the gold half of the S mark, text selection. Hover lifts to **Sun Lift** (sun-hi), press settles to **Sun Pressed** (sun-lo); text on gold is **Burnt Umber** (on-sun).
- **Sun Lift** (sun-hi): also the focus ring and the hover colour of inline text links.

### Tertiary
- **Ember Alarm** (alarm): errors only (field borders and messages, alerts as a 10% tinted fill with a 38% inset line, the maintenance dot, a failed release fetch). Never decoration, never success.

### Neutral
- **Night Ground** (ground): the page, the opaque bar once scrolled, the footer, the field autofill.
- **Pit Black** (black): behind images before they load (auth art, account band).
- **Panel** (panel), **Raised Panel** (panel-2), **Slot** (panel-3): the status panel and download band; quest card, fields, image placeholders; step numbers, disabled plates, skeleton blocks.
- **Ink** (ink), **Ink 2** (ink-2), **Ink 3** (ink-3), **Ink 4** (ink-4): headings and values; ledes, body, nav at rest; labels, meta, captions; disabled text and the offline dot.
- **Hairline** (line), **Rule** (line-2), **Control Line** (line-3): section and row dividers; panel outlines and list top rules; field, checkbox and ghost-plate outlines and link underlines (about 3.2:1 on the ground).

### Named Rules
**The Measured Gold Rule.** Gold fills only the primary action and a state the system has measured or confirmed: a server that reported itself online, a verified email, a password rule met, coins paid, a claimed quest segment. A static "Open" gets no dot at all; an unmeasured value is never gold. Gold as interaction feedback (focus ring, link hover, field focus border, caret) is allowed; gold as decoration is not.

**The Picture Carries Colour Rule.** Type, panels and chrome stay warm-neutral; any colour beyond the one gold comes from the GTA V still behind it.

**The Alarm Is Not Gold Rule.** Errors take Ember Alarm, never gold; a failing password rule before submit is plain ink-2, it turns alarm only after a submit.

## Typography

**Display Font:** Sofia Sans Extra Condensed (with Arial Narrow, sans-serif), self-hosted, weights 400-900, Latin and Cyrillic subsets.
**Body Font:** Sofia Sans (with system-ui, sans-serif), self-hosted, weights 300-900, Latin and Cyrillic subsets.
**Label/Mono Font:** ui-monospace / Cascadia Mono / Consolas, for the release SHA-256 only (13px).

**Character:** a tall, tight condensed face in heavy caps for names, headings and labels, the voice of a game HUD; a plain humanist sans for every sentence. Figures are tabular everywhere (set on body).

### Hierarchy
- **Display** (800, clamp(48px, 6vw, 88px), 0.92, uppercase, max 15ch): the hero H1 only; 42-60px on phones.
- **Headline** (800, clamp(40px, 4.2vw, 62px), 0.95, uppercase, balanced): section titles.
- **Name** (800, clamp(44px, 4.4vw, 64px), 0.9): a server's name (SOL) in its own section.
- **Figure** (800, 44px, 0.9): the status panel's server name and a reward amount.
- **Title** (800, 26-28px, 1.0, uppercase): fact headings, quest titles.
- **Plate label** (800, 19px, 0.06em, uppercase): plate text; 16 / 17 / 20-24 / 22-28px for xs / sm / lg / xl plates.
- **Label nav** (700, 19px, 0.06em, uppercase): bar links; footer links are 16px at 0.1em.
- **Label** (700, 15-17px, 0.08-0.1em, uppercase): status state, "Next" lead-in, reward unit, status link.
- **Body lead** (400, 18px, 1.55, ink-2, max 46-52ch): section ledes, server description, hero lede (17-19px).
- **Body** (400, 17px, 1.55): running text; 16px in lists and steps, max 42-46ch.
- **Body strong** (700, 19px, 1.3): step titles (Sofia Sans, sentence case).
- **Body small** (400, 13-15px, ink-3): captions, meta lines, fact labels (dt).

### Named Rules
**The Human Size Rule.** Headings stop at human sizes: the hero H1 tops out at 88px (5.5rem), section titles at 62px. No giant slogans, no title cards.

**The Name Face Rule.** The condensed face is for names, headings, figures, plate labels and short caps labels. Every sentence is Sofia Sans in sentence case.

## Layout

One left edge for the whole page: `max(gutter, (100vw - 1440px) / 2)`, so bar, sections and footer align and wide screens centre a 1440px measure. The gutter is clamp(20px, 4vw, 64px). The bar is 64px, fixed; it starts as a top-down fade and turns solid ground with a hairline once scrolled (always solid on account pages).

Sections stack with clamp(56px, 6vw, 96px) vertical padding and a 1px hairline between them. A section head is the title and a lede; above 960px they sit in two equal columns, the lede right-aligned to the bottom. Content grids are asymmetric fractional splits: 5fr / 7fr (server image and text), 7fr / 5fr (HUD plates and facts, account body), 1fr / 1fr (quests, download), three equal columns for steps above 760px. Column gaps are 48-72px, row gaps 24-36px; inner rhythm runs on 8 / 12 / 14 / 16 / 22 / 24 / 28 / 32px.

Facts are rows, not cards: a label and a value separated by a hairline (status facts, server facts, needs, account key/value rows).

Breakpoints: 1000px (hero splits into copy and a 320-380px status column), 960px (section heads, game, quests and download go two-column), 899px (account pages stack), 760px (server and steps stack; on phones the hero photo becomes a 4:3 band above the copy instead of a wash behind it, and lg CTAs go full width), 560px (bar drops the section links and keeps Log in and Download), 480px (the quest card stacks).

## Elevation & Depth

Flat. Depth comes from tonal steps (ground, panel, panel-2, panel-3) and 1px warm hairlines, never from drop shadows. The only shadows are legibility halos on type standing directly on a photograph, a 1px hairline under the solid bar, and the live dot's 3px gold halo. Over photos, a directional shade darkens only the side the type stands on and fades the foot into the ground; the rest of the picture stays.

### Shadow Vocabulary
- **Title halo** (`text-shadow: 0 2px 24px rgb(0 0 0 / 0.35)`): the hero H1 over the still.
- **Nav halo** (`text-shadow: 0 1px 12px rgb(0 0 0 / 0.7)`): bar links over the still before the bar turns solid.
- **Bar rule** (`box-shadow: 0 1px 0 rgb(255 244 224 / 0.12)`): the solid bar's bottom edge.
- **Live halo** (`box-shadow: 0 0 0 3px rgb(242 177 52 / 0.18)`): the online dot only.

### Named Rules
**The Opaque Rule.** Panels, plates and bars are opaque or near-opaque (the status panel is 92% panel over the still). No backdrop blur, no glass, no translucent bar over moving content.

**The No Full-Window Blend Rule.** No fixed full-window overlay, grain layer or blend mode over the page; the shade belongs to the image it grades.

## Shapes

Square corners everywhere. The one silhouette is the S mark's 45-degree top-right cut, made with a clip-path polygon: plates cut 8 / 9 / 12 / 14 / 16px (xs / sm / md / lg / xl), step numbers 10px, panels 18px (status panel, quest card), HUD image plates 22px, the auth art clamp(56px, 7vw, 120px). Because a clip-path cuts a border off at the diagonal, an outline on a cut shape is a ring drawn by a pseudo-element: one evenodd polygon whose hole is inset by the stroke width, with the diagonal's inner points moved by 0.4142 x the width. Images take the cut as a clip only (no ring). The only round shape is the 9px status dot.

**The One Cut Rule.** One corner, top right, 45 degrees. No border radius, no other notches, no rotated or skewed panels.

## Components

### Buttons (plates)
Tactile, cut and heavy; gold is the one call to act.
- **Shape:** square with the top-right 45-degree cut; height 34 / 38 / 44 / 56 / 68px (xs / sm / md / lg / xl), xl at least 340px wide.
- **Primary:** sun fill, on-sun condensed 800 caps, 12px gap for an icon.
- **Hover / Focus:** hover sun-hi, press sun-lo plus 1px down (160ms, ease-out). Focus is a ring that follows the cut: 2px ink inside, 2px on-sun outside.
- **Ghost:** transparent, ink text, a 1px control-line ring; hover 7% warm white, press 11%. Never gold. "Soon" plates are a ghost in ink-2.
- **Disabled:** panel-3 fill, ink-3 text (ghost: ink-4 text).

### Status panel (signature)
The store-page info box beside the pitch: SOL's name as a figure, the state at the right (dot plus condensed caps), then hairline fact rows (label ink-3 14px left, value 15px 600 right), a note and a "How to join" link. Tones: open = no dot; online = gold dot with halo and the Players row shown with the measured count; offline = ink-4 dot, ink-3 text; maintenance = alarm dot. Without a measured count the Players row is hidden, never filled with a stand-in. 92% panel, 18px cut, 1px rule ring.

### Cards / Containers
- **Corner Style:** square with an 18px top-right cut.
- **Background:** panel (status, download band) or panel-2 (quest card).
- **Shadow Strategy:** none (see Elevation & Depth).
- **Border:** a 1px line-2 ring that follows the cut.
- **Internal Padding:** 24px (status), 24px 28px (quest card).

### Lists
- **Fact rows:** a 1px line-2 top rule, condensed 800 26px caps heading, 16px ink-2 sentence.
- **Steps:** three columns, each a 48px panel-3 number plate with a 10px cut and an ink condensed numeral, then a Sofia Sans 700 19px title and a 16px line.

### Inputs / Fields
- **Style:** 50px tall, panel-2 fill, 1px control-line border, square, 17px Sofia Sans, label 600 15px ink-2 above; caret gold.
- **Focus:** border and a 1px inset turn sun (150ms).
- **Error / Disabled:** alarm border and a 600 15px alarm message under the field; a hint line in ink-3 14px otherwise.

### Chips
- **Style:** 700 13px condensed caps at 0.12em, ink-3 text, 1px line-2 inset ring, square.
- **State:** confirmed (chip--ok) turns the text and ring gold; a done quest keeps the ring neutral and only its check gold.

### Navigation
The bar: the lockup at 28px tall on the left, condensed 700 19px caps links in ink-2 (hover ink) on the right, then Log in and a small gold Download plate. Over the hero the bar is a top-down fade with halos; scrolled, it is solid ground with a hairline. Phones below 560px keep only Log in and Download. Footer: the S mark, condensed 16px caps links (hover gold), and the non-affiliation line in ink-3 14px.

### Motion
One authored moment: on load the hero copy and the status panel rise 14px from transparent over 700ms (ease-out cubic-bezier(0.16, 1, 0.3, 1)), 80ms and 220ms delays, once. Everything else is state feedback: plates 160ms, fields 150ms, the bar background 300ms. Account pages add a coin count that holds gold then cools to ink (2200ms) and a single 420ms gold slash across a just-paid row, removed when it ends. Reduced motion turns all of it off.

## Do's and Don'ts

### Do:
- **Do** use one dimmed in-game GTA V still per page, graded only on the side the type stands on, with its source recorded in `ASSETS.md`.
- **Do** spend gold on the primary action and on states the system has measured or confirmed (The Measured Gold Rule).
- **Do** show a live value only when the API returned it; hide the row otherwise.
- **Do** keep headings at human sizes: hero at most 88px, section titles at most 62px.
- **Do** cut plates and panels with the single top-right 45-degree corner, and draw outlines on cut shapes as evenodd rings.
- **Do** put facts in hairline rows and keep sentences in Sofia Sans, sentence case, 42-52ch.
- **Do** keep the bar opaque once anything moves under it.

### Don't:
- **Don't** add a 3D logo or any real-time 3D; the owner rejected the 3D S ("remove this disgusting 3D S").
- **Don't** use video, footage loops or a montage; the owner rejected it ("too many videos, a bit laggy").
- **Don't** add scroll-driven motion, letterboxes, title cards or giant slogans ("too animated, make it more professional").
- **Don't** add fixed full-window overlays, grain layers or blend modes over the page.
- **Don't** use gold as decoration or for an unmeasured state; a static "Open" gets no dot.
- **Don't** invent counts, prices, quotes, rules, testimonials or features; only what the shipped client and SOL do, and Discord stays "soon" with no link until an invite exists.
- **Don't** show Rockstar or GTA V logos, Pricedown, or anything that implies an official Rockstar product.
- **Don't** use glass, backdrop blur, border radius or drop shadows on panels and plates.
- **Don't** set sentences in the condensed face.
