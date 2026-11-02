---
version: 1
slug: "web-src-app-jsx"
primary_target: "web/src/App.jsx"
related_targets: []
---

# Launcher shell (whole window)

Scope: the entire launcher window (web/src/App.jsx and every view in it). Mode: Operate.
Audience and job: a GTA V player opens the launcher to pick a live server and get into the game fast; often returns to one favourite. States that must read instantly: loading, empty, list offline, launching, launched, failed and why.
Constraints: 1240x800 default, resizable 960..1680+, frameless WebView2, single-file build, bridge contract fixed (web/BRIDGE.md). Official GTA V key art allowed; Pricedown only as outlined SVG; no GTA logo or Rockstar marks.
Rejected 2026-10-25: the Night Drive 3D band, highway-sign buttons, the LED board, character cutouts as filler (critic-rules.md lines 2-4).

## Direction contract

THESIS: The launcher is the GTA V pause menu, played straight: the paused city blurred behind floating stacks of translucent black boxes, white reversal for selection, tab strip on top, key hints at the foot. It refuses the launcher-dashboard default (cards, hero band, sidebar) and every second metaphor.

OWN-WORLD: Blurred, darkened Los Santos art full-bleed; panels rgba(0,0,0,.72) with no borders, no radius; rows 36px, white on black, selected row white with black text; tabs equal-width, selected white, a 4px character-colour bar over it (Michael #65b4d4, Franklin #abedab, Trevor #ffa357, freemode #2d6eb9 when nothing is picked). One sans (Overpass) in caps for tabs and labels; Pricedown only for the gage-mp title and big messages. Ping as GTA connection bars. Keyboard glyph boxes for hints.

STORY: The player sees the list, arrows or clicks a server, reads its job card (art, name, players, ping, address), presses Enter or Connect, watches a GTA loading screen while the game starts; a failure is the game's alert screen with Retry and the fix.

FIRST VIEWPORT: Top 84px header: Pricedown gage-mp at 46px left, player name, day and time, servers and players online stacked right, window buttons in the corner. Tab strip of four equal tabs across the content width. Below, the server list (about two thirds) beside the job card column (380px): cover art 16:9, title bar in the lead's colour, stat rows, the Connect row, direct connect. Footer: busy spinner and status left, key hints right. Panels size to their content; the blurred city shows below them.

FORM: GTA V pause menu (the user's pinned pick 2026-10-25, over concept-seed assignment 4 of 7); raises: snap state changes, no easing on navigation (from the acetate manual); rank by weight, case and reversal only (from the timetable rack). Seed key 60f749b5.

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance
