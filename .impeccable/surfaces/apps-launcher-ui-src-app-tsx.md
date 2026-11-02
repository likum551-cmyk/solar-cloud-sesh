---
version: 1
slug: "apps-launcher-ui-src-app-tsx"
primary_target: "apps/launcher-ui/src/App.tsx"
related_targets: ["apps/launcher-ui/index.html"]
---

Scope: the whole GAGE client SPA (apps/launcher-ui/), one 1240x800 frameless window: auth (login, register, verify, reset, Discord), Play (our servers, launch), Shop (products, orders, owned), Profile, Settings (game, launch, account, devices). Mode: Operate, with the Shop leaning Persuade.

Audience/job: logged-in GAGE players getting into our server in seconds; first-timers registering; buyers trusting the shop. Proof/content: live server state and shop products come only from the platform API; nothing commercial invented, demo content labelled. Imagery is real GTA V footage and screenshots (official trailers and Steam screenshots, owner-accepted IP risk, ASSETS.md).

Constraints: bridge in apps/launcher/BRIDGE.md; platform API in apps/platform/docs/api.md; offline-bundled assets served by the host from launcher-assets/ (no Range: video is played from a blob URL); footage and the emblem pause when hidden, unfocused-idle or in game (the footage and the emblem keep playing under a running launch and stop at hand-off); prefers-reduced-motion = posters and stills only; no Rockstar or GTA V logos, no Pricedown, never presented as an official Rockstar product; no AI palettes or neon glow; references named by the owner: Riot Client / Valorant, Majestic RP, GTA5RP and Grand RP launchers.

## Direction contract

THESIS: The client is a window onto Los Santos: real GTA V footage runs full-bleed behind every screen and GAGE-MP is a thin, hard-edged instrument laid over it: one name, one live number, one blue PLAY. It refuses the home-made 3D scene, the film-strip metaphor, the slate side panel and a card grid on a flat ground.

OWN-WORLD: Footage first, graded at the edges only (a band under the rail, the bottom-left corner the type stands on, a light falloff on the right); the middle of the picture stays untouched footage, and reading surfaces (ledgers, owned shelf) drop it to a near-solid ground. Every loop is one continuous shot, never a montage. Ink #F2F1EE, one secondary grey #A7ABB2 and hairlines at 9-16% white; GAGE blue #2D6EB9 only for PLAY (also while busy), the one submit of a form (sign in, BUY), the chosen server or product, the active tab bar, the running launch segment and focus; a small green dot only for online. Opaque near-black panels (#0B0D10) with one hairline and one 45-degree corner cut from the G mark; no glass. Sofia Sans Extra Condensed 800 caps for names at display scale, Sofia Sans for reading, tabular figures. Cards are screenshots with the name set on them. The chrome G emblem is the only real-time 3D, lit by studio strips over the footage.

STORY: The client opens on the city at dusk; you sign in on the right-hand panel or enter the demo. Play shows the chosen server's own footage behind its huge name and live count; our servers stand in a column of screenshot cards on the right; picking one crossfades the window into that world. PLAY charges the emblem blue and the footage pushes in; the screen gives way and the launch comes up over the same shot, still playing. The Shop shows each item over its own GTA V still with a name plate, one honest line and a price; orders and owned items keep their record.

FIRST VIEWPORT: 1240x800 Play. Top 40px rail: GAGE-MP lockup left; PLAY SHOP PROFILE SETTINGS in condensed caps with a 2px blue bar under the active one; demo pill, account and window controls right. Full-bleed server footage. Bottom-left hero: a 112px chrome emblem beside the server name at about 92px condensed caps, the live count at display size (about 56px) with "Online now", "of 300 slots" and a capacity bar, a meta row (region, mode, ping), then the 64px chamfered blue PLAY plate; a status line appears only when PLAY is closed. Right column, about 320px wide: our five servers as screenshot cards, the chosen one framed in blue, its capacity bar inside the frame.

FORM: Owner-pinned world (2026-10-29: "this is not gta5 3d? ... you can download anything and use any tool"): the GTA V cinematic lobby of the big GTA RP launchers and the Riot Client. No direction roll and no re-roll (task brief); seed key: none, owner-pinned.

FINISH: unreviewed and undocumented is unfinished; this build ends with the finish review, the verdict, DESIGN.md, and every shipping raster carrying its provenance

Memorable moment: PLAY: the emblem's enamel charges blue-white, the footage pushes in and keeps playing, and the launch comes up over the same shot: the server name, a five-segment rail and the current step at display size.

Unresolved: the platform has no per-server or per-product art field yet (art is chosen by server name/tag and product grant); no public API domain yet.
