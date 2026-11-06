# Shot list

2026-11-06, GI: "too animated, make it more professional". The page scrolls as a page: no pinning, no scrubbed timelines, no gold slab wipes, no per-letter reveals, no zooms. The one authored moment is the arrival (the hero title rises 16 px and fades in on the S's curve, expo-out 1.4 s). The S stands still in the hero and the close (it turns up to 2 degrees with the pointer) and renders only while its shot is on screen and its pose changes. The previous trailer structure (pinned beats, slab cuts) is in git history before this date.

Then, GI: "too many videos and it's a bit laggy". One video, the hero; every other shot is a still. Native scrolling (no smoothing library), no full-window grain or blend layers.

| # | Section | Image | 3D S | Copy |
|---|---|---|---|---|
| 1 | Hero, one screen | video `hero` (the skyline at dusk, forward then back, no seam), letterboxed 2.39:1 on desktop (black bars), cropped on phones | At rest right of the title, yaw -11 deg; settles in on load | Title, lede, DOWNLOAD plate with version and size from the signed manifest |
| 2 | Title cards, 3 x 100vh in flow (phones: square plates, the copy on the ground under each) | stills `still-beach`, `still-pool`, `still-canal`; each shade is a soft pool behind its copy only | none | "Our own servers." / "Our own client." / "One button in.", a short fact line under each |
| 3 | Characters, a triptych on wide screens | key art portraits `cast-michael`, `cast-franklin`, `cast-trevor` | none | "The city you know." "The rules are ours." "Roleplay first." |
| 4 | Servers | key art `server-sol`, `server-luna`, `server-terra` | none | SOL (EU, roleplay, open), LUNA (freeroam, soon), TERRA (racing, soon) |
| 5 | Get in | launcher screenshots, one beside each step on wide screens | none | 01 Download, 02 pick SOL, 03 PLAY |
| 6 | Close | still `still-close` (the Galileo Observatory at night over the lit city), rising out of the ground, dark only at the foot | Fades in in the sky above the main dome as the section enters; does not move | "Los Santos is waiting." DOWNLOAD again, what you need, SHA-256, footer |

Reduced motion or Save-Data: no arrival; the hero stays on its poster and the S is rendered once at its hero pose.
