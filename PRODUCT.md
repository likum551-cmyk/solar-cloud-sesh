# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

(A React SPA rendered inside a Windows desktop window via WebView2; desktop-only, default window 1240x800, resizable.)

## Users

GTA V players on Windows who play on the Solar RP network: our own few official servers (roleplay first, plus freeroam/racing modes as they come). They open Solar Launcher to log in, see which of our servers are up and how full, buy or check donations, tweak settings, and hit PLAY. Many open it daily; first-timers arrive from a Discord or a video and must get from download to in-game with no confusion.

## Product Purpose

Solar Launcher is the front door of one game network, not a public server browser (changed 2026-10-29: "our own gta5, our own server, our own client"). It requires an account (email + password, or Log in with Discord), lists only our official servers with live player counts and maintenance state, launches GTA V and the Solar RP engine into the chosen server with a single-use join token, runs a donation shop (currency packs, VIP, cosmetics) and keeps per-account settings. Success = a logged-in player is in our server within seconds, always knows what the client is doing, and finds the shop desirable and trustworthy.

## Positioning

Solar RP is its own GTA V multiplayer network: its own client, in-game engine, servers, accounts and protocol (`docs/ENGINE-PLAN.md`), under its own brand and its own visual world (chosen 2026-10-29 over GTA V's pause-menu language). It is not an official Rockstar Games or Take-Two product and must never present itself as one.

## Operating Context

- Runs as a tray-resident desktop client; stays open while GTA V boots and runs.
- Native (C++) owns game path, launch/injection, the registry handoff and secure token storage; the SPA talks to native through the bridge in `apps/launcher/BRIDGE.md` and to the platform API (`apps/platform/`: accounts, join tokens, official servers, shop, settings) over HTTPS.
- The SPA ships as one self-contained HTML file embedded in SolarLauncher.exe; fonts, images, 3D models and scripts are bundled, no third-party network assets at runtime.
- The platform API has no public domain yet; offline, API-down, maintenance and empty states are real and frequent.

## Capabilities and Constraints

- Screens: login / register / verify email / reset password (email + password, Log in with Discord); home with our servers (live counts, maintenance banner, one-click PLAY, launch progress and result); shop (products, checkout handoff, order history, owned items); settings (game folder, launch options, language, start minimized, close game on exit, updates; account, linked Discord, devices/sessions); profile.
- Payments are provider-agnostic for now: checkout opens the provider's page; the client never handles card data.
- 3D (owner, 2026-10-29: "sick design pls. 3d cool stuff"): a shop showroom (VIP cars, outfits, cosmetics as real rotatable models), a cinematic home scene, the G emblem as a lit 3D object reacting to PLAY, and the player's character on login/profile. Rendering pauses when minimized, hidden to tray, unfocused-and-idle, or while the game runs, so it never costs in-game FPS. Must stay fast to first interaction.
- Animation libraries requested by the user: GSAP and Lenis.

## Brand Commitments

- Name: Solar RP (renamed from GAGE-MP on 2026-11-01), with the logo assets in `brand/` (icon, lockup, svg).
- GTA V's own imagery is the visual world (owner, 2026-10-29, second change that day: "this is not gta5 3d? its just dogshit 3d wannabe ... you can download anything and use any tool"; owner accepts the IP risk). Real GTA V footage and screenshots (official Steam/Rockstar trailers and screenshots) carry every screen, the way the big GTA RP launchers do. No Rockstar or GTA V logos, no Pricedown; Solar RP stays its own brand on top. Never imply it is an official Rockstar product.
- The 3D G emblem stays (owner: "the g logo is good"). Home-made real-time 3D scenes are rejected twice: the night-freeway band (2026-10-25, "complete dogshit") and the free-model flash-lit street with figure, car and props (2026-10-29, "dogshit 3d wannabe"). Free or procedural models never stand in for GTA V.
- The film-strip sprocket motif is rejected ("top dotted shit is bad", 2026-10-29).
- "No AI colory shits" (user, 2026-10-24): no generic AI palettes (purple/indigo/violet gradients, neon-on-black glow templates, rainbow accents).
- Quality bar named by the owner: Riot Client / Valorant and the big GTA RP project launchers.

## Evidence on Hand

- The old GAGE-MP logo assets exist (`brand/`); new Solar RP art is being designed. No product renders, car or character models, or screenshots supplied; 3D content must be sourced (licence-clean) or authored.
- No live servers, prices or player counts yet; nothing commercial (prices, counts, testimonials) may be invented for display beyond what the API provides. Demonstration content is labelled as such.

## Product Principles

1. Get the player into the game: login → server → PLAY in as few steps as possible.
2. Always say what is happening: loading, empty, offline, maintenance, launching, success and failure are first-class states.
3. Spectacle never costs the game: motion and 3D yield to the game process and to the task.
4. Trust where money is involved: the shop is clear about what you get, what it costs and where you pay.

## Accessibility & Inclusion

Keyboard operable (tab order, Enter to play, focus visible); respect `prefers-reduced-motion` by disabling 3D camera motion and large transitions; text contrast WCAG AA on all states.
