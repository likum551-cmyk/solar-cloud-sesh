# Page map

2026-11-06, second redesign of the day. GI on the quiet community hub: "grim asf", "What SOL runs today is what i dont like", servers "without default gta poster", "it needs more anim". GI chose "Big RP project" (bright, warm, alive, like the big GTA RP project pages) and our own server emblems. Every section has its own ground so no screen reads black. Motion is CSS transform / opacity only, with no video and no WebGL, and all of it is off under reduced motion. The hub is in git history at commit 58fa021.

| # | Section | Ground and image | Copy, data and motion |
|---|---|---|---|
| 1 | Hero (at most 860 px) | `still-skyline` under a warm glow; three tilted photo cards (`reel-car`, `reel-beach`, `reel-freeway`) with gold-rim cut frames | H1 "GTA V roleplay on our own servers", lede, gold DOWNLOAD with a breathing glow (version and size from the signed manifest) and a ghost "Create account". SOL status pill (`GET /api/v1/servers`) and the release line under the CTAs. Counter row: 3 servers and 2 editions tick up; Street NPCs is a static 0. The cards float, and on a fine pointer they follow it (parallax). |
| 2 | Reel | ember band | `reel-boulevard`, `downtown`, `vinewood`, `observatory`, `pool` in a 60 s marquee |
| 3 | Servers "Three servers, one sky" | sunset band | SOL as a large card: our own sun emblem with the S, live state, mode Roleplay, facts (Europe, Los Santos and Blaine County, client version). LUNA (moon) and TERRA (earth) as "Next" cards with no mode claims. Emblems loop: rays turn, stars twinkle, continents drift. |
| 4 | How to join | cream band | 1 Download, 2 Pick SOL, 3 Press PLAY. The line draws in on scroll, then the numbers turn in. |
| 5 | Quests pay coins | ember band, `reel-office` tilted | The one seeded quest: "Link your Discord", +500 coins (ticks up) |
| 6 | See you in Los Santos | `still-canal` under a shade | DOWNLOAD again, release line, status, then needs: system, game, SHA-256 |

Not on the page until GI supplies them: SOL screenshots with players, server rules, an RP feature list (jobs, gangs, police, housing), the Discord invite, live player counts (SOL is not registered with the platform yet).
