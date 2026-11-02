# Archived: server client-asset downloader

Moved here from `libs/net/assets.{h,cpp}` (the folder was `client/net/` until the
2026-10-29 restructure) and `tests/assets_test.{cpp,mjs}` on 2026-10-29
when RAGE-MP compatibility was removed from GAGE-MP. Not built, not shipped, not tested.

What it was: the launcher's downloader for a RAGE-MP server's own client packages
(`/file/N` on the server's HTTP port; the launcher called it before starting the game and
reported progress to the SPA as `assetProgress`, see `apps/launcher/BRIDGE.md`). GAGE servers speak
the GAGE protocol (`libs/protocol`) and have no such file host, so the launcher no longer
calls it. It is kept for reuse: a WinHTTP downloader with a content cache, resume, a
parallel fetch pool, redirect and size guards, and a scripted fixture-server test.

To bring it back:

1. Move `assets.{h,cpp}` to `libs/net/` and `assets_test.{cpp,mjs}` to `tests/`. The archived
   sources still carry the pre-restructure includes: in `assets.{h,cpp}` change
   `"../common/x.h"` to `"common/x.h"`, and in `assets_test.cpp` change
   `"../client/net/assets.h"` to `"net/assets.h"` (`libs/` is the include root), and the
   `client/net/assets.cpp` mention in `assets_test.mjs`'s header comment to `libs/net/assets.cpp`.
2. Add `libs/net/assets.cpp` to the `GAGE-MP` target, and this target to `CMakeLists.txt`:

   ```cmake
   add_executable(assets_test tests/assets_test.cpp libs/net/assets.cpp)
   target_link_libraries(assets_test PRIVATE gage_common winhttp bcrypt advapi32 shell32)
   target_compile_options(assets_test PRIVATE -municode)
   target_link_options(assets_test PRIVATE -municode)
   set_target_properties(assets_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)
   ```

3. Run it: `node tests/assets_test.mjs --exe build/win/selftest/assets_test.exe`
   (29 scenarios expected).
4. Call `net::DownloadServerAssets` from `DoLaunch` in `apps/launcher/gage_gui.cpp` and post
   `assetProgress` messages as `apps/launcher/BRIDGE.md` describes.
