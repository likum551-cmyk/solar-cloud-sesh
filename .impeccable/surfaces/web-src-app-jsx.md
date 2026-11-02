---
version: 1
slug: "web-src-app-jsx"
primary_target: "web/src/App.jsx"
related_targets: ["web/index.html"]
---

Scope: the whole GAGE-MP launcher SPA (web/), one window, all four views (Browse, Favorites, Recents, Settings). Mode: Operate.

Audience/job: GTA V multiplayer players picking a server and getting into the game fast; the launcher must always say what it is doing (loading, empty, error, launching, result).

Constraints: bridge contract in web/BRIDGE.md is fixed; frameless window (SPA owns the drag region and the minimize/close buttons); self-contained single HTML, no network; 3D pauses when hidden, unfocused-and-idle, or launching; prefers-reduced-motion gives static frames; no AI palettes. Asset boundary revised by the owner on 2026-10-24 ("grab actual gta 5 things"; chose "3D freeway + real assets"): official GTA V key art IS used, always cropped clear of the GTA V logo and the Rockstar badge; Pricedown IS the title lettering, outlined to SVG paths at build time (the font file never ships). Never show the GTA V logo or Rockstar marks, never claim to be official. Every shipping raster is listed in web/src/assets/gta/PROVENANCE.md.

## Direction contract

THESIS: The server browser is the GTA V pause menu over a night freeway. Every server is a destination on an overhead guide sign, and connecting means taking its exit. It refuses the category default of a slate sidebar, a card grid and a blue Play button.

OWN-WORLD: GTA V's own pause menu and loading screens, over a road-sign system on wet night asphalt.
- Lettering: Pricedown (white fill, black outline) for the wordmark, panel titles, empty and error titles, and the "game launched" / "launch failed" shards. Overpass (Highway Gothic stand-in) for sign legends and UI; mono only for addresses.
- Pause menu: square translucent black glass panels over a blurred night-graded downtown, a solid black head bar; dark list rows with 2px gaps, the picked row white with black type; white active tab; "< On >" option rows in Settings.
- Key art: official character art on the launch column's cover (fixed per server address), the loading screen, the empty/error stages (lead right, copy left) and the list's spare height (lead when tall, night-graded skyline strip when short). Never the same lead twice on screen.
- HUD colours: Michael #65b4d4, Franklin #abedab, Trevor #ffa357 as the 3px foot rule under each piece of key art; gold #f0c850 for "game launched", red for "launch failed".
- Signs: guide green (#0d6b42) plates with a white inset border carry navigation and the one primary action (Connect / Retry), the port on an EXIT tab. Status runs on an amber LED variable message sign. Errors use regulatory red.

STORY: A loading screen (rotating lead, Pricedown wordmark) hands over to the menu. The player sees their destination hung on the gantry, scans the server rows below, picks one (the camera glides to the next gantry, which carries that name), and takes the exit with the green plate or Enter. The VMS narrates the launch; success is a gold "game launched" shard over a held skycam frame, failure drains the band to grey under a red "launch failed".

FIRST VIEWPORT: A 1240x800 frameless window.
- A 44px drag bar: Pricedown wordmark, pause-menu tabs, the HUD cluster (player name over day, time and server counts), minimize and close.
- Below it, a ~300px full-width 3D band: a night freeway, sodium lamps, light-trail traffic, a steel truss gantry whose main sign shows the selected server's name, host, live facts and EXIT port.
- Under the band, glass panels. Left: the server rows with search and refresh, key art in any spare height. Right, 324px: the cover card, the green Connect plate, the VMS, quick exits (recents and favorites, each marked), and direct connect.

FORM: Freeway Gantry, #5 on my ordered list; seed key d69a5767. Raised from challengers: destructive actions isolated with empty space (console); the selected lane brightest while the others stay readable (transit).

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance

Memorable moment: selecting a server flies the camera under the current gantry to the next one, which carries the new name. Taking the exit swings the camera onto the off-ramp, then the scene freezes under the "game launched" shard.

Unresolved: the default server-list endpoint (native side) is still unknown, so empty and error states must look designed, not broken. The 3D band's surroundings are procedural (box buildings, cone palms, untextured truss) and were judged below the key art's bar; sourcing real assets is an open owner decision.
