# Archived: the launcher's real-time street scene ("Hard Flash")

Moved here from `apps/launcher-ui/src/three/` (`Stage.tsx`, `Street.tsx`, `Showroom.tsx`, `assets.ts`, `paint.ts`, `prints.ts`) and `apps/launcher-ui/src/audio/shutter.ts` on 2026-10-29, when the launcher UI switched to GTA V footage and stills behind the interface. Not built, not shipped, not tested.

What it was: a React Three Fiber stage that rendered a dusk street (CC0 character, car, props, HDRI) with an on-camera flash, a contact-sheet print pass and a shutter sound for the PLAY moment. The owner rejected it as not looking like GTA V. The chrome G emblem (`src/three/Emblem.tsx`, `EmblemLayer.tsx`, `env.ts`) and the launch sound (`src/audio/launch.ts`) stayed in the app.

The models, textures and HDRI it loaded are outside the repo in `Downloads\gage new\launcher-3d-retired-2026-10-29\`, with their sources and licences in `ASSETS-retired.md` there.

To bring it back: move the files back to the same folders under `apps/launcher-ui/src/`, the assets back to `apps/launcher-ui/public/assets/`, and mount `<Stage>` in `App.tsx` in place of `<Backdrop>`.
