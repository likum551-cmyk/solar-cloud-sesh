# Page map

2026-11-06, after three rejections of the trailer page ("too animated", "too many videos, laggy", then "FULL REDESIGN, remove this disgusting 3D S"), GI chose a community hub: the page a big GTA RP project ships, played straight. No video, no 3D, no scroll-driven motion. The one authored moment is the hero's arrival (the pitch, then the status panel, rise 14 px and fade in, 700 ms, CSS only; none under reduced motion). The trailer page is in git history up to commit 5ab40f9.

| # | Section | Image | Copy and data |
|---|---|---|---|
| 1 | Hero (84svh, at most 880 px) | `still-skyline` under a left-weighted shade (phones: a 4:3 band above the copy) | H1 "GTA V roleplay on our own servers", lede, gold DOWNLOAD (version and size from the signed manifest) + ghost "Create account" (guests only). SOL status panel: state from `GET /api/v1/servers` (`src/servers.ts`), a Players row only when the server reports a count (gold dot only then), mode, region, game editions, client version from the manifest; with no SOL row it keeps a neutral "Open" |
| 2 | Servers | `server-sol` key art | SOL (roleplay, Europe, live status) as one large row; one "Next" line for LUNA (freeroam) and TERRA (racing) |
| 3 | What SOL runs today | `hud-ring`, `hud-car`: the client's own overlay render, captioned as demo values off the game (in-game screenshots from GI pending) | Four facts the shipped client and server really do, beside the HUD plates: no ambient traffic or pedestrians, the HUD, shared car state, server authority |
| 4 | How to join | none (the launcher screenshot showed demo player counts and was removed) | 1 Download, 2 Pick SOL, 3 Press PLAY, three across (no account step: the launcher does not sign in with an account yet, see the signup page note) |
| 5 | Quests and coins | none | Coins for quests on the account page; the one seeded quest as a row: "Link your Discord", 500 coins, opens with Discord login |
| 6 | Install and play | none (panel ground) | DOWNLOAD again, system / game / account needs, SHA-256, footer |

Not on the page until GI supplies them: server rules, an RP feature list (jobs, gangs, police, housing), the Discord invite.
