---
version: 1
slug: "client-ui-src-app-tsx"
primary_target: "client-ui/src/App.tsx"
related_targets: ["client-ui/index.html"]
---

Scope: the whole GAGE client SPA (client-ui/), one 1240x800 frameless window: auth (login, register, verify, reset, Discord), Play (our servers, launch), Shop (showroom, products, orders, owned), Profile, Settings (game, launch, account, devices). Mode: Operate, with the Shop leaning Persuade.

Audience/job: logged-in GAGE players getting into our server in seconds; first-timers registering; buyers trusting the shop. Proof/content: live server state and shop products come only from the platform API; nothing commercial invented, demo content labelled.

Constraints: bridge in web/BRIDGE.md (native: game path, launch with join token, secure token storage); platform API in platform/docs/api.md; offline-bundled assets; 3D pauses when hidden, unfocused-idle or in-game; prefers-reduced-motion = static flash frames; no Pricedown, no GTA V key art or marks; no AI palettes or neon glow; owner bar: Riot Client / Valorant and big GTA RP launchers.

## Direction contract

THESIS: Every screen is a frame shot at night with one hard on-camera flash: the subject blows to white, the city falls to black, and you choose your shot off the contact sheet. It refuses the dark-slate side rail, the key-art banner and the blue PLAY tile.

OWN-WORLD: Crushed black #050506 to flash white #F4F1EA on an 11-step zone ramp, no in-between greys; GAGE blue #2D6EB9 reserved for the one active choice (the circled frame, PLAY, focus); film-edge orange #E8642C for edge codes and live counts only. Real-time 3D lit by a single camera-mounted key light with hard falloff, fine grain and halation on highlights. Contact-sheet frames with sprocket edges and printed edge codes; grease-pencil circles and crosses in blue; every plate, mask and bevel cut with the G mark's 45-degree chamfer on the mark's construction grid. Condensed grotesk caps for edge codes and labels, a sharp neo-grotesk for reading, tabular figures everywhere.

STORY: The client opens on black; a flash pops and your character stands lit in the street. You log in on a frame. On Play, our servers lie along the bottom as a contact strip; you circle one, its name develops large, PLAY fires the flash and cuts to the game. The Shop is a flash-lit showroom on black: each item a frame with a name plate and one honest line; orders stamp PAID, DELIVERED, REFUNDED and stay on the record.

FIRST VIEWPORT: 1240x800. Top 40px film-edge rail: sprocket holes, frame numbers 01 PLAY / 02 SHOP / 03 PROFILE / 04 SETTINGS, account and window controls. Full-bleed 3D: the player's character and the chrome G emblem flash-lit at left-centre, street falling to black. Right third: the circled server's name at display scale, live count in orange edge code, the chamfered blue PLAY plate. Bottom 200px: the contact strip of our servers.

FORM: Hard Flash (night street and car-meet flash photography), #4 on my ordered list; seed key 03e785ee (re-roll 1, steer "more cinematic and cool"). Raises kept: blue reserved for the active choice (orienteering), 11-step zone ramp (exposure record), the mark's grid as layout grid (Crouwel), name plate plus one honest line per item (goods catalog), the whole extent visible as a film-edge rail (acetate manual).

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance

Memorable moment: PLAY fires the flash: the frame blows to white, the shutter sound clicks, and the client cuts to black as the game launches.

Unresolved: 3D content sourcing (character, cars, outfits) needs licence-clean models; the platform API has no public domain yet.
