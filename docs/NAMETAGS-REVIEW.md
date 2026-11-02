# W10 player name tags: what the PC reviewer captures

Built on the cloud (cross-compiled, never run in a game). The pure rules (fade, projection, the 5 Hz line-of-sight cache, the eased alpha, `[id] Name` text, the nearest-N list) are `libs/sync/nametag.h`, proven by `gameplay_test` (TestW10, 16 checks, mutant-checked). The game-side wiring (`TickNameTags` in `apps/engine/engine.cpp`) and the drawing (`DrawNameTags` in `apps/engine/overlay_ui.cpp`) have not run. The reviewer tools (impeccable, visual-critic) are local to the PC: run them on the captures below before release. Nothing here has had a visual review yet.

## What it does
- Per remote ped within 40 m of the render camera: head bone (SKEL_Head 31086) + 0.35 m, projected with GET_SCREEN_COORD_FROM_WORLD_COORD, occluded by HAS_ENTITY_CLEAR_LOS_TO_ENTITY(local ped, puppet, 17) at most 5 Hz per peer (the answer is reused for 200 ms and trusted for 1 s, then the tag hides: fail closed).
- Drawn by the overlay in the world layer, under every plate and under the Tab scrim: `[id]` in the small face (kInk2 at 0.62) then the name in Segoe UI Semibold (kInk2), ground-coloured outline plus a soft drop, no panel. Full alpha to 25 m, linear to 0 at 40 m; size 100 % up to 8 m, 76 % at 40 m. Fades in over 160 ms, out over 110 ms. At most 12 tags (the nearest).
- Hidden: dead peer, no ped yet, beyond 40 m, off screen or behind the camera, behind a wall, the pause menu open, the local player not alive, no server session, the loader up.
- A peer seated in a vehicle keeps its tag (at the head bone, inside the car). Whether the LOS trace sees the car body as a wall for a seated peer is not known: `nametags_los=<flags>` (HKCU\Software\GAGE-MP, re-read at Welcome) changes the trace flags without a rebuild; `nametags=0` turns the feature off.
- gage.log: one line when it first runs, then every 10 s while peers are around: `engine: name tags (10 s): frames, drawn avg/max, candidates, LOS asked / reused / blocked / failed, off screen, native failures`. `LOS ... failed` or `native failures` above 0 means a native could not be called (tags then stay hidden).

## Captures (day and night, a bright sky and a dark alley as backgrounds)
1. A friend standing at 5, 15, 30 and 40 m in the open: size steps down, alpha is full at 5/15, about 2/3 at 30, gone at 40. The text must read on the sky and on the pavement (outline, not a box).
2. Behind a wall / a corner / a fence at 10 m: the tag goes within about 0.35 s and comes back when the friend steps out. Stand at a door frame or a fence to see whether the 5 Hz ray flickers (known risk: no hysteresis, one blocked answer starts the 110 ms fade).
3. Friend dead (on the ground): no tag; alive again after respawn: it fades back in.
4. Friend driving past, friend as a passenger, and you seated in a car looking at a pedestrian: tags present? If a seated peer is never tagged the trace treats the car as a wall (try `nametags_los=1`).
5. Many peers (6+ in view, and 3+ standing together): the nearest 12 are drawn; tags can overlap when two heads are close on screen (no de-overlap pass), check it reads acceptably.
6. Overlay scale: 720p, 1080p and 4K (fonts follow display height, floors 15/14 px, tags never below 11/10 px).
7. Chat open and the Tab palette open: tags stay under the plates / dimmed by the scrim. Esc pause menu: no tags. Alt+F4 card on top.
8. Long and odd names (24 chars, Georgian/Cyrillic, a name with spaces): `[id] Name` stays one line, cut with an ellipsis only past 24 code points.
9. The gage.log 10 s line for the counters above.

## Known limits
- IS_SCREEN_FADED_IN is not consulted: during the game's own screen fades (not death/respawn, which hide the tags) a tag stays visible over the fade.
- Tags are placed from the script tick and drawn by the next Present: about one frame of lag while a ped runs.
