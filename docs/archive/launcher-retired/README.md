# Retired launcher sources (moved here in 0.4.32)

- `wv2_shims.cpp`: CRT/STL shims for linking the MSVC-built `WebView2LoaderStatic.lib` into the mingw build. Not in
  CMakeLists.txt: WebView2 loading is in-tree (`apps/launcher/webview2_loader.cpp`, see the comment above it in
  CMakeLists.txt), so the MSVC library and these shims are no longer linked.
