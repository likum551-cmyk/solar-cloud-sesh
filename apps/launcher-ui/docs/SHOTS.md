# Shot list

This is an app, not a scroll page: each shot belongs to a screen or a chosen server, not a scroll range. The footage is the camera; the only real-time 3D is the emblem.

The values live in `src/App.tsx` (which scene each screen shows, the PLAY timeline), `src/media/catalog.ts` (which loop and still each server and product gets) and `src/media/Backdrop.tsx` (crossfade, drift, pause). Every file is listed with its source in `ASSETS.md`.

## Transitions

- Screen to screen, and server to server on Play: the new loop or still fades in over the old one (900 ms); the screen content rises in.
- A new server picked: the emblem's enamel pulses (charge 0 → 0.42 in 0.06 s → 0 in 0.3 s).
- Stills drift slowly (held at 1.06 scale, panning about 2.4% across and back over 48 s each way); loops are single shots already slowed 2.2-3x, so they play as cut.
- Reduced motion: posters and stills only, no drift, no pulse, a still emblem; PLAY goes straight to the launch overlay.
- Paused (`renderActive` false: hidden, unfocused and idle, in game): the loop, the drift and the emblem stop. The emblem also stops while the launch covers the screen; the loop keeps playing under the launch.

| # | Screen / state | Background | Emblem | Overlay | Out |
|---|---|---|---|---|---|
| 0 | Boot | ground `#07080A` | none | the brand mark (G) alone | fade to the first screen once the first frame is ready (5 s at most) |
| 1 | Auth | loop `city` (Downtown at dusk) | left, beside the lockup | lockup and one line of copy left; the opaque auth panel right | fade to Play after sign-in |
| 2 | Play | the chosen server's loop (`serverArt`): `city`, `harbor`, `wild`, `night` or `vinewood` | top-left of the hero, beside the name | the server name, the online count at display size with its capacity bar, the meta row and blue PLAY bottom-left; the server cards (each a frame of its own loop) right | crossfade to another server's loop |
| 3 | PLAY | the current loop pushes in 5% over 1.4 s and keeps playing; it is never darkened | enamel charges 0 → 1 in 0.46 s | the screen fades out from 0.32 s to 0.62 s | the launch overlay at 0.64 s |
| 4 | Launch | the same loop, pushed in and still playing (it pauses once GTA V runs); one gradient from the left corner; a failed launch greys it | beside the server name, in the hero's place; stops with the footage at hand-off | the server name, phase chip and clock, a five-segment rail, the current step at display size, then launched or failed with what to do next | Back: the loop eases back to 1, the screen returns |
| 5 | Shop showroom | the chosen product's still (`productStill`), drifting | none | name plate, one line, price, blue BUY left; product cards right | crossfade to another product's still |
| 6 | Shop orders / owned | the showroom still under a 92% black scrim, almost all covered by the full-height summary column and record | none | the opaque ledger with its summary; the owned panel of thumbnail cards | showroom |
| 7 | Profile | still `downtown` (the Downtown skyline at dusk over the freeway), drifting | none | name, roles, since; the opaque facts panel right | fade |
| 8 | Settings | still `settings` (the Downtown freeway interchange at dusk, a helicopter by the towers) | none | the title and section index left; the forms in the opaque right panel | fade |
