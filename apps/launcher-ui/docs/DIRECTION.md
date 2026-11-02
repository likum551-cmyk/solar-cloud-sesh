# Direction: Window on Los Santos

The direction is locked by the surface contract, `.impeccable/surfaces/apps-launcher-ui-src-app-tsx.md`. The owner pinned it on 2026-10-29; there was no direction roll and no re-roll. This file records the bar it is held to, the history, and the rules the imagery follows.

## The bar (reference works)

| Reference | What it does well that we hold ourselves to |
|---|---|
| Riot Client / VALORANT launcher | Full-bleed key art that changes with the chosen game; one unmistakable PLAY; launch progress inside the same window; the UI is a thin, hard-edged layer, never a dashboard. |
| Majestic RP launcher | GTA V footage behind everything, graded dark at the edges so white type always reads; the server is the product, with its online count large. |
| GTA5RP and Grand RP launchers | A column of servers as screenshot cards with live counts and capacity bars; picking one changes the whole window. |

## Direction history

1. **Night Drive** (2026-10-24/25): a low-poly 3D band under official GTA V key art. Rejected ("full redesign"): mixed fidelities and five visual metaphors in one window. Lesson kept: the critic scores whole-window coherence first.
2. **Hard Flash** (2026-10-29, morning): a home-made real-time street (CC0 character, car, props) shot with an on-camera flash, with a film-strip and contact-sheet UI. Rejected: "this is not gta5 3d? its just dogshit 3d wannabe. the g logo is good but its not it man"; "top dotted shit is bad". Retired to `docs/archive/launcher-3d-street/` (source) and `Downloads\gage new\launcher-3d-retired-2026-10-29\` (assets).
3. **Window on Los Santos** (this client): the owner's own world, GTA V itself behind a minimal Solar RP interface. The chrome G emblem is the one thing kept from Hard Flash.

## Thesis

The client is a window onto Los Santos. Real GTA V footage runs full-bleed behind every screen, and Solar RP is a thin instrument laid over it: one name, one live number, one blue PLAY.

- **Footage first.** Each server has its own loop, one continuous trailer shot held slow (3-5.4 s, seamless), and its card shows a frame of that shot; Auth plays the city; Shop, Profile and Settings sit on stills. The grade lives at the edges only (a band under the rail, the bottom-left corner the type stands on, a light falloff right); the middle of the picture is untouched footage. Ledgers and the owned shelf drop the footage to a near-solid ground.
- **Hard, quiet chrome.** Opaque near-black panels with one hairline and one 45-degree corner cut taken from the G mark. No glass, no blur, no glow. Names in Sofia Sans Extra Condensed 800 caps at display size.
- **The emblem.** The chrome G is the only real-time 3D: one small canvas over the footage, lit by studio strips so it reads as polished metal.
- **The memorable moment.** PLAY charges the emblem's enamel blue-white and the footage pushes in; the screen gives way and the launch comes up over the same shot, still playing: the server name, a five-segment rail and the current step at display size.

## Imagery rules

- Only official GTA V and GTA Online trailers and Steam store screenshots (Rockstar Games). The owner accepted the IP risk. CC0 or home-made models never stand in for GTA V.
- No Rockstar or GTA V logo and no Pricedown on screen. Every chosen frame was checked at 1:1 for burned-in logos, watermarks, captions and overlay UI, and cropped or dropped. In-world signs (shop names, billboards) stay.
- No weapons in the foreground of a still the UI sits on.
- Loops are one continuous shot each (never a montage), muted, seamless, 1600×900 H.264 with `+faststart`, under 5 MB, with a WebP poster and card thumbnail each. Every file and its source are in `ASSETS.md`.

## Palette and type (contract)

- **Ground and panels:** `#07080A` ground, `#0B0D10` / `#12151A` / `#1A1E25` panels; hairlines at 7-20% white.
- **Ink:** `#F2F1EE`, `#C9CCD1`, `#A7ABB2` for text (all at least 4.5:1 on the panels); `#6B7079` only for disabled and decoration.
- **Blue:** `#2D6EB9` (hover `#4A8AD6`), only for PLAY/BUY, the chosen server or product, the active tab bar and focus.
- **Status:** green `#3FB66B` for online only; amber `#D8A34A` and red `#E0645A` for warnings and failures.
- **Type:** Sofia Sans Extra Condensed for names, labels and codes; Sofia Sans for reading. Figures are tabular.
