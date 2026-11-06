# Retired engine sources (moved here in 0.4.32)

Not built. Kept for reference, the same way `docs/archive/assets/` keeps the RAGE-MP asset downloader.

- `loading_screen.{h,cpp}`: the first D3D11 loading overlay. It was still compiled into `gage_mp.dll` and `gage_mp_test`,
  but nothing called it after the ImGui overlay (`overlay.cpp` / `overlay_ui.cpp`) took over the loader.
- `dlc_mount.{h,cpp}`: the run-time mount of the logo-tee clothing pack through FiveM-style signatures. It was never in
  CMakeLists.txt and its sites were never confirmed on the decrypted 3889 image. The live content route is
  `apps/engine/pack_stream.cpp` (docs/CONTENT-STREAMING-PLAN.md).

To bring one back: `git mv` it into `apps/engine/`, add it to the `gage_mp` and `gage_mp_test` source lists in
CMakeLists.txt, and build both trees.
