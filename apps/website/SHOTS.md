# Shot list

One scroll source of truth: Lenis drives GSAP's ticker; every beat is a ScrollTrigger timeline scrubbed by it. The 3D S lives on one fixed transparent canvas behind the type and in front of the footage; it is drawn only while beat 1 or beat 6 is on screen.

| # | Scroll | Footage | 3D S (camera fixed, the mark moves) | Overlay copy | Transition out |
|---|---|---|---|---|---|
| 1 | Hero, pinned 0-160vh | `city` loop, letterboxed 2.39:1 on desktop (black bars), 4:5 crop on phones; scale 1 to 1.08 | Enters on load: rises 6% and settles, yaw -9 deg. Scroll: yaw -9 to +12 deg, lifts toward the haze sun, scale 1 to 0.55, fades by 85% | Kicker "GTA V roleplay on our own network", lockup, DOWNLOAD plate with version and size from the signed manifest | Bars open to full bleed as the S leaves; a 45-degree gold slash wipes to beat 2 |
| 2 | Title cards, pinned 3 x 100vh | `harbor`, `night`, `wild`, one per card | none | "Our own servers." / "Our own client." / "One button in." one line each at 12-16vw, a short fact line under each | Gold slash wipe between cards (clip-path, the mark's 45-degree cut) |
| 3 | Characters, pinned 3 x 90vh | Michael, Franklin, Trevor splashes | none | "The city you know." "The rules are ours." "Roleplay first." | Each panel grows from a framed 45-degree-cut panel to full screen, then the next takes over |
| 4 | Servers | thumbs of `city`, `wild`, `night` | none | SOL (EU, roleplay, open), LUNA (freeroam, soon), TERRA (racing, soon) | Cards rise in, stagger 80 ms |
| 5 | Get in | launcher screenshots (sign in, Play) | none | 01 Download, 02 Sign in, 03 PLAY; what you need (Windows 10/11 64-bit, GTA V Legacy for PC on Steam, Epic or Rockstar Games Launcher) | Steps reveal on enter |
| 6 | Close | `vinewood` loop, dimmed | Returns small, at three-quarter yaw settling to 0 as the section enters, beside the DOWNLOAD plate | "Los Santos is waiting." DOWNLOAD again, SHA-256, footer (Discord soon, not affiliated with Rockstar Games or Take-Two, footage credit) | end |

Reduced motion: no smoothing, no pinning, no wipes; every section is a static framed shot, the S rendered once at its hero pose.
