cmake_minimum_required(VERSION 3.16)
project(GAGE_MP C CXX) # C only for the vendored ENet (third_party/enet)

# Cross-compile from Linux to Windows PE x86_64 with mingw-w64.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-O2 -fno-exceptions)
# -static flags must reach the *link* step to embed the mingw C++ runtime,
# otherwise every artifact imports libstdc++-6.dll and needs the mingw
# redistributable on a clean Windows box. add_link_options covers the link.
# Windows only: a fully static glibc binary cannot resolve names (getaddrinfo) reliably.
if(WIN32)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# Server-only build (the Linux VPS entry point; apps/server/CMakeLists.txt is the
# standalone equivalent): just enet, the protocol library, the client session, the
# server core, gage-server and the server tests, none of the Windows client.
#   cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release -DGAGE_SERVER_ONLY=ON
option(GAGE_SERVER_ONLY "Build only the game server and its tests" OFF)
if(GAGE_SERVER_ONLY)
    set(GAGE_ROOT ${CMAKE_SOURCE_DIR})
    include(${GAGE_ROOT}/cmake/gage_server.cmake)
    return()
endif()

add_compile_definitions($<$<CONFIG:Debug>:_DEBUG>)

# All runtime artifacts (the launcher exe + injected DLLs) land next to each
# other in bin/ so the launcher can resolve them by exe directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------------------------------------------------------------------------
# gage_common: shared static library (injector, logger, registry, helpers)
# ---------------------------------------------------------------------------
add_library(gage_common STATIC
    libs/common/common.cpp
    libs/common/registry.cpp
    libs/common/logger.cpp
    libs/common/inject.cpp
    libs/common/config.cpp
    libs/common/crashscan.cpp
)
target_include_directories(gage_common PUBLIC libs) # "common/x.h", "net/x.h", "protocol/x.h", ...
# gage_protocol (defined with the multiplayer stack below) brings the include
# path "protocol/protocol.h" for gage::proto::kDefaultPort, the one default game
# port every client artifact shares.
target_link_libraries(gage_common PUBLIC
    gage_protocol
    psapi
    kernel32
    advapi32
    user32
)

# ---------------------------------------------------------------------------
# gage_v.exe: launcher (admin check, inject chain, server finder)
# ---------------------------------------------------------------------------
add_executable(gage_v
    apps/devtool/gage_v.cpp
    libs/net/serverfinder.cpp
    libs/net/ping.cpp
    libs/net/serverquery.cpp
)
target_link_libraries(gage_v PRIVATE gage_common winhttp shell32 ws2_32)
# gage_v is a CLI self-test harness for the inject chain, not a second client.
# The shipped product is SolarLauncher.exe alone, so gage_v builds to selftest/.
set_target_properties(gage_v PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# ---------------------------------------------------------------------------
# SolarLauncher.exe (CMake target GAGE-MP, OUTPUT_NAME SolarLauncher): GUI launcher, the WebView2
# host for the embedded React+Tailwind SPA (apps/launcher/BRIDGE.md is the authoritative
# JS<->native contract).
# ---------------------------------------------------------------------------
# Embed the built SPA as a byte-array header so the launcher stays one exe:
# apps/launcher-ui (`npm run build` there). Its 3D set, dist/assets, is copied
# next to the exe as launcher-assets/ (see the GAGE-MP target below). The
# legacy SPA's recovered build stays in docs/archive/web-legacy/dist/.
set(GAGE_UI_HTML_IN  ${CMAKE_SOURCE_DIR}/apps/launcher-ui/dist/index.html)
set(GAGE_UI_ASSETS_IN ${CMAKE_SOURCE_DIR}/apps/launcher-ui/dist/assets)
set(GAGE_UI_HTML_OUT ${CMAKE_BINARY_DIR}/gen/ui_html.h)
add_custom_command(
    OUTPUT ${GAGE_UI_HTML_OUT}
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${GAGE_UI_HTML_IN}
            -DOUTPUT=${GAGE_UI_HTML_OUT}
            -P ${CMAKE_SOURCE_DIR}/tools/embed_ui.cmake
    DEPENDS ${GAGE_UI_HTML_IN} ${CMAKE_SOURCE_DIR}/tools/embed_ui.cmake
    COMMENT "Embedding SPA into ui_html.h"
)
add_custom_target(gage_ui_embed DEPENDS ${GAGE_UI_HTML_OUT})

# Baked-in self-update (docs/UPDATES.md). The URL is the signed manifest (version.json) the
# launcher checks: https://solar-rp.com/update/version.json. Clients built before the domain
# existed bake the bare-IP http://5.175.253.187/update/version.json; the VPS keeps serving that
# plain-HTTP path for them (legacy), and a signed manifest can move clients to a new URL (updateUrl). The public key is the ECDSA P-256 key the
# manifest must be signed with: base64 of the raw 64-byte X||Y point (tools/update-sign/keygen.mjs
# prints it). A cache entry made by an older configure keeps its old value: re-run cmake with -D.
set(GAGE_UPDATE_URL "https://solar-rp.com/update/version.json" CACHE STRING "URL of the launcher's signed update manifest")
set(GAGE_UPDATE_PUBKEY "IV5tI6PzbJzIpbVxhLpdV7IZCE/K3SWp8dO6VNBZlE4ECBC4N5E/nRzt/vcZNK6r3L/j8yWv6AJcOTBxLuxfhw==" CACHE STRING "Public key the update manifest must be signed with (base64 of raw X||Y)")
string(LENGTH "${GAGE_UPDATE_PUBKEY}" GAGE_UPDATE_PUBKEY_LEN)
if(NOT GAGE_UPDATE_PUBKEY MATCHES "^[A-Za-z0-9+/]+==$" OR NOT GAGE_UPDATE_PUBKEY_LEN EQUAL 88)
    message(FATAL_ERROR "GAGE_UPDATE_PUBKEY must be the base64 of a 64-byte X||Y point (88 characters ending in ==), got '${GAGE_UPDATE_PUBKEY}'")
endif()
if(NOT GAGE_UPDATE_URL MATCHES "^https?://[^ ]+$")
    message(FATAL_ERROR "GAGE_UPDATE_URL must be an http(s) URL, got '${GAGE_UPDATE_URL}'")
endif()
# The platform API origin; empty until the platform is deployed (the launcher UI
# then runs its labelled demo data). gage_mp.cfg `api_base=` overrides it.
set(GAGE_API_BASE "" CACHE STRING "Platform API origin for the launcher UI (https://...)")
# Test builds only: the server demo-mode PLAY joins (host[:port]).
set(GAGE_TEST_SERVER "" CACHE STRING "Server demo-mode PLAY joins in test builds (host[:port])")
configure_file(${CMAKE_SOURCE_DIR}/tools/build_config.h.in ${CMAKE_BINARY_DIR}/gen/build_config.h @ONLY)

set(GAGE_WV2_ROOT ${CMAKE_SOURCE_DIR}/third_party/webview2/x/build/native)
# WebView2.h includes "EventToken.h"; mingw-w64 ships it as eventtoken.h. Windows hosts do not care about case,
# a Linux cross-build (the VPS, cmake/toolchain-mingw-linux.cmake) does: forward the SDK's spelling there.
set(GAGE_CASEFIX_DIR "")
if(NOT CMAKE_HOST_WIN32)
    set(GAGE_CASEFIX_DIR ${CMAKE_BINARY_DIR}/gen/casefix)
    file(WRITE ${GAGE_CASEFIX_DIR}/EventToken.h "#pragma once\n#include <eventtoken.h>\n")
endif()

# The SHA-256 of each DLL the launcher loads into the game, baked into the
# launcher (gen/chain_hashes.h): a DLL changed after the build is never injected.
set(GAGE_INTEGRITY_OUT ${CMAKE_BINARY_DIR}/gen/chain_hashes.h)
add_custom_command(
    OUTPUT ${GAGE_INTEGRITY_OUT}
    COMMAND ${CMAKE_COMMAND}
            -DOUT=${GAGE_INTEGRITY_OUT}
            "-DFILES=$<TARGET_FILE:gageinjector>|$<TARGET_FILE:gage_loader>|$<TARGET_FILE:gage_mp>"
            -P ${CMAKE_SOURCE_DIR}/tools/integrity.cmake
    DEPENDS gageinjector gage_loader gage_mp ${CMAKE_SOURCE_DIR}/tools/integrity.cmake
    COMMENT "Hashing the injected DLLs into chain_hashes.h"
    VERBATIM
)
add_custom_target(gage_integrity DEPENDS ${GAGE_INTEGRITY_OUT})

# WebView2 loading is in-tree (apps/launcher/webview2_loader.cpp): the MSVC-built
# WebView2LoaderStatic.lib needs the MSVC CRT (TLS thread-safe statics, /GS)
# and crashed when shimmed onto the mingw CRT. Only the SDK headers are used.
# The brand icon resource builds alone: windres hands -I paths to its
# preprocessor unquoted, so the exe's include dirs broke the build whenever the
# source tree path contains a space. The .rc needs no include dirs.
add_library(gage_gui_res OBJECT apps/launcher/gage_gui.rc) # apps/launcher/res/gage-mp.ico

add_executable(GAGE-MP
    apps/launcher/gage_gui.cpp
    apps/launcher/webview2_loader.cpp
    apps/launcher/game_launch.cpp
    $<TARGET_OBJECTS:gage_gui_res>
    libs/net/serverfinder.cpp
    libs/net/ping.cpp
    libs/net/serverquery.cpp
    apps/launcher/integrity.cpp
    apps/launcher/legacy_handoff.cpp
    apps/launcher/updater.cpp
)
add_dependencies(GAGE-MP gage_ui_embed gage_integrity gage_launch_shim)
target_include_directories(GAGE-MP PRIVATE
    ${CMAKE_BINARY_DIR}/gen
    ${GAGE_WV2_ROOT}/include
    ${GAGE_CASEFIX_DIR}
)
target_link_libraries(GAGE-MP PRIVATE
    gage_common
    winhttp comdlg32 gdi32 user32 ws2_32 comctl32 shell32 shlwapi
    ole32 oleaut32 advapi32 bcrypt crypt32
    uuid   # IID_IUnknown (SDK uuid definitions)
)
# The target keeps its internal name; the file the player gets is SolarLauncher.exe.
set_target_properties(GAGE-MP PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin OUTPUT_NAME SolarLauncher)
# The launcher UI's 3D set next to the exe, replaced whole so a renamed or
# deleted asset never lingers (the exe serves only what manifest.json lists).
add_custom_command(TARGET GAGE-MP POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/bin/launcher-assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${GAGE_UI_ASSETS_IN} ${CMAKE_BINARY_DIR}/bin/launcher-assets
    COMMENT "Copying the launcher UI's 3D set to bin/launcher-assets"
    VERBATIM
)
# wWinMain entry (wide-char main) requires -municode at compile AND link so
# mingw's CRT startup (crtexewin.c) resolves the wWinMain symbol.
target_compile_options(GAGE-MP PRIVATE -municode)
# -municode at link resolves the wWinMain entry. --subsystem,windows makes it a
# proper GUI app (no stray console window behind the launcher); logs still land
# in gage.log via the logger.
target_link_options(GAGE-MP PRIVATE -municode -Wl,--subsystem,windows)

# game_launch_test.exe: store detection and launch URLs (tests/), never shipped
add_executable(game_launch_test tests/game_launch_test.cpp)
target_link_libraries(game_launch_test PRIVATE gage_common shell32 advapi32)
set_target_properties(game_launch_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# version_gate_test.exe: the launcher's lockstep policy (apps/launcher/version_gate.h, header-only and pure):
# the protocol gate's decision table, "every available update is mandatory", the 2-minute poll and the 60 s
# minimum gap between event-driven update checks. Never shipped.
add_executable(version_gate_test tests/version_gate_test.cpp)
target_include_directories(version_gate_test PRIVATE libs)  # "protocol/protocol.h": the shared refusal sentences (constants only, nothing linked)
set_target_properties(version_gate_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# legacy_handoff_test.exe: the one-time registry clean-up, on a scratch key, never shipped
add_executable(legacy_handoff_test tests/legacy_handoff_test.cpp apps/launcher/legacy_handoff.cpp)
target_link_libraries(legacy_handoff_test PRIVATE gage_common advapi32)
set_target_properties(legacy_handoff_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# update_check_test.exe: the launcher's signed self-update (apps/launcher/updater.cpp): signature,
# payload, version, size and hash checks plus a download against a local HTTP server. Driven by
# tests/update_check_test.mjs, which signs the fixtures. It never starts an installer. Never shipped.
add_executable(update_check_test tests/update_check_test.cpp apps/launcher/updater.cpp
    libs/net/serverfinder.cpp libs/net/ping.cpp libs/net/serverquery.cpp)
target_include_directories(update_check_test PRIVATE ${CMAKE_BINARY_DIR}/gen) # build_config.h: the baked key
target_link_libraries(update_check_test PRIVATE gage_common winhttp ws2_32 shell32 bcrypt)
set_target_properties(update_check_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# integrity_test.exe: the launcher's check of its own DLLs, never shipped
add_executable(integrity_test tests/integrity_test.cpp)
add_dependencies(integrity_test gage_integrity)
target_include_directories(integrity_test PRIVATE ${CMAKE_BINARY_DIR}/gen)
target_link_libraries(integrity_test PRIVATE gage_common bcrypt)
target_compile_options(integrity_test PRIVATE -municode)
target_link_options(integrity_test PRIVATE -municode)
set_target_properties(integrity_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# crash_diag_test.exe: the in-game crash diagnostics (stack scan, minidump struct,
# MSVC throw decoding, main-thread lookup) on its own process, never shipped
add_executable(crash_diag_test tests/crash_diag_test.cpp)
target_link_libraries(crash_diag_test PRIVATE gage_common)
set_target_properties(crash_diag_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# crash_filter_test.exe: gageinjector's VEH declines benign/continuable
# exceptions (DBG_PRINTEXCEPTION_C from OutputDebugString) without poisoning its
# re-entrancy guard -- the 0x40010006 1-second game exit. Never shipped.
add_executable(crash_filter_test tests/crash_filter_test.cpp)
target_link_libraries(crash_filter_test PRIVATE gage_common)
set_target_properties(crash_filter_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# script_suppress_test.exe: the engine's singleplayer script tables (apps/engine/sp_scripts.h)
# are sound and safe -- the story-script terminate list never overlaps the session roots
# (main_persistent / main / startup), so suppressing the campaign voice/text bleed can never kill
# our own script host or the SP session. Header-only; never shipped.
add_executable(script_suppress_test tests/script_suppress_test.cpp)
set_target_properties(script_suppress_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# serverquery_test.exe: the launcher's UDP server-query client (libs/net/serverquery.cpp)
# against an in-process UDP responder on loopback -- proves a real round trip reads the live
# player counts, that a wrong nonce is ignored, and that a dead port times out. Never shipped.
add_executable(serverquery_test tests/serverquery_test.cpp libs/net/serverquery.cpp)
target_link_libraries(serverquery_test PRIVATE gage_common ws2_32)
set_target_properties(serverquery_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# ---------------------------------------------------------------------------
# gageinjector.dll: injected into the game/anticheat launcher (no exports)
# ---------------------------------------------------------------------------
add_library(gageinjector SHARED
    apps/injector/gageinjector.cpp
)
target_link_libraries(gageinjector PRIVATE gage_common)
set_target_properties(gageinjector PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ---------------------------------------------------------------------------
# gage_loader.dll: injected into the game; loads the engine (no exports)
# ---------------------------------------------------------------------------
add_library(gage_loader SHARED
    apps/injector/gage_loader.cpp
)
target_link_libraries(gage_loader PRIVATE gage_common)
set_target_properties(gage_loader PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ---------------------------------------------------------------------------
# gage_launch_shim.dll: injected by the launcher into PlayGTAV.exe. It IAT-hooks
# PlayGTAV's process-creation calls and redirects the BattlEye-wrapper spawn
# (GTA5_BE.exe) to the real game exe (GTA5.exe), so GTA V boots in PlayGTAV's
# own ROS session with BattlEye never arming. Self-contained (no gage_common):
# the DLL injected into Rockstar's launcher stays tiny and dependency-free.
# ---------------------------------------------------------------------------
add_library(gage_launch_shim SHARED
    apps/launcher-shim/gage_launch_shim.cpp
)
target_link_libraries(gage_launch_shim PRIVATE advapi32 kernel32)
set_target_properties(gage_launch_shim PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# launch_shim_test.exe: the redirect decision (ComputeRedirect) and the IAT hook
# redirecting a real CreateProcessW call in-process, never shipped
add_executable(launch_shim_test tests/launch_shim_test.cpp)
target_link_libraries(launch_shim_test PRIVATE advapi32)
set_target_properties(launch_shim_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# ---------------------------------------------------------------------------
# gage_overlay: the in-game UI (Dear ImGui over the D3D11 swap chain: loading state, logo,
# chat, quick actions, speedometer). Static library linked by everything that compiles
# apps/engine/engine.cpp. See cmake/gage_overlay.cmake.
# ---------------------------------------------------------------------------
include(${CMAKE_SOURCE_DIR}/cmake/gage_overlay.cmake)

# ---------------------------------------------------------------------------
# gage_mp.dll: in-game engine (game hook primitives, mp.* JS bridge, and the
# GAGE protocol session via gage_netclient: ENet + protocol v1, see below)
# ---------------------------------------------------------------------------
add_library(gage_mp SHARED
    apps/engine/gage_mp.cpp
    apps/engine/join.cpp
    apps/engine/hook.cpp
    apps/engine/net_guard.cpp
    apps/engine/engine.cpp
    apps/engine/loading_screen.cpp
    apps/engine/brand_apparel.cpp
    apps/engine/pack_stream.cpp
)
target_link_libraries(gage_mp PRIVATE gage_common gage_netclient gage_sync gage_overlay winhttp ws2_32)
set_target_properties(gage_mp PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# gage_mp_test.exe: the in-game patches, run on the test exe's own image, never shipped
# (pulls in gage_mp.cpp's TU, which references the game hook + engine + join modules)
add_executable(gage_mp_test
    tests/gage_mp_test.cpp
    apps/engine/join.cpp
    apps/engine/hook.cpp
    apps/engine/net_guard.cpp
    apps/engine/engine.cpp
    apps/engine/loading_screen.cpp
    apps/engine/brand_apparel.cpp
    apps/engine/pack_stream.cpp
)
target_link_libraries(gage_mp_test PRIVATE gage_common gage_netclient gage_sync gage_overlay ws2_32)
set_target_properties(gage_mp_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# brand_apparel_test.exe: the branded-tee texture swap (hash, BC1 encoder, composite, D3D11 hooks on a WARP device), never shipped
add_executable(brand_apparel_test tests/brand_apparel_test.cpp apps/engine/brand_apparel.cpp)
target_link_libraries(brand_apparel_test PRIVATE gage_common d3d11 dxgi)
set_target_properties(brand_apparel_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# ---------------------------------------------------------------------------
# Bundle the in-game UI (consumer of the mp.* bridge) next to the exe.
# ---------------------------------------------------------------------------
file(COPY apps/ingame-ui/ DESTINATION ${CMAKE_BINARY_DIR}/bin/ui)

# ---------------------------------------------------------------------------
# GAGE own multiplayer stack (docs/ENGINE-PLAN.md, phase P1): ENet transport,
# protocol v1, server, client session, bot, netcode_test. None of it is shipped
# by the installer: gage-server and gage-bot build outside bin/ (the folder the
# installer packages from), the test into selftest/ like the other tests.
# ---------------------------------------------------------------------------
# enet, gage_protocol, gage_netclient, gage_server_core, gage-server, netcode_test and
# server_test live in cmake/gage_server.cmake, which the standalone Linux-friendly build
# (apps/server/CMakeLists.txt) and the GAGE_SERVER_ONLY option include as well. It also
# defines Threads::Threads.
set(GAGE_ROOT ${CMAKE_SOURCE_DIR})
include(${GAGE_ROOT}/cmake/gage_server.cmake)

add_executable(gage-bot apps/bot/main.cpp)
target_link_libraries(gage-bot PRIVATE gage_netclient)
set_target_properties(gage-bot PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

# join_test.exe: the engine's session adapter (apps/engine/join.cpp) against an in-process
# server on 127.0.0.1, never shipped
add_executable(join_test tests/join_test.cpp apps/engine/join.cpp)
target_link_libraries(join_test PRIVATE gage_common gage_server_core gage_netclient Threads::Threads)
set_target_properties(join_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# ---------------------------------------------------------------------------
# gage_sync: the remote-player smoother (libs/sync/smoother.h): turns the irregular
# WorldState snapshots of one remote player into a smooth pose. Pure computation,
# no platform headers. sync_test drives it against scripted network conditions.
# libs/sync/locomotion.h turns that pose into GTA ped commands (tasks, velocity,
# heading, snaps); locomotion_test drives it against a simulated ped.
# libs/sync/vehicle_drive.h does the same for a remote driver's vehicle (snapshot
# interpolation with quaternions, velocity + angular velocity steering, snaps);
# vehicle_drive_test drives it against a simulated car.
# ---------------------------------------------------------------------------
# libs/sync/combat.h, emote.h and vehicle_link.h are the engine's protocol-v4 decisions (server-owned
# health, respawn gate, hit batching, emote vs locomotion, the local vehicle link); gameplay_test
# drives them. libs/sync/vehicle_entry.h adds the F / G seat choice and the seated drive-by decision.
add_library(gage_sync STATIC libs/sync/smoother.cpp libs/sync/netclock.cpp libs/sync/locomotion.cpp libs/sync/vehicle_drive.cpp
    libs/sync/combat.cpp libs/sync/emote.cpp libs/sync/vehicle_link.cpp libs/sync/vehicle_entry.cpp)
target_include_directories(gage_sync PUBLIC libs) # "sync/smoother.h"
target_link_libraries(gage_sync PUBLIC gage_protocol) # smoother.h uses the protocol's WorldEntry / Kinematics

# sync_test.exe: the smoother under loss, jitter, reordering and teleports, never shipped
add_executable(sync_test tests/sync_test.cpp)
target_link_libraries(sync_test PRIVATE gage_sync)
set_target_properties(sync_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# locomotion_test.exe: the remote-ped locomotion controller against a simulated ped, never shipped
add_executable(locomotion_test tests/locomotion_test.cpp)
target_link_libraries(locomotion_test PRIVATE gage_sync)
set_target_properties(locomotion_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# vehicle_drive_test.exe: the remote-vehicle interpolator and drive controller against a simulated car, never shipped
add_executable(vehicle_drive_test tests/vehicle_drive_test.cpp)
target_link_libraries(vehicle_drive_test PRIVATE gage_sync)
set_target_properties(vehicle_drive_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# gameplay_test.exe: vitals / respawn gate / hit batcher / emote arbiter / vehicle link, never shipped
add_executable(gameplay_test tests/gameplay_test.cpp)
target_link_libraries(gameplay_test PRIVATE gage_sync)
set_target_properties(gameplay_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)

# pack_collection_test.exe: offline proofs for the streaming content-pack TOC builder
# (apps/engine/pack_collection.{h,cpp}); entry bit-packing pinned to live golden vectors. Never shipped.
add_executable(pack_collection_test tests/pack_collection_test.cpp apps/engine/pack_collection.cpp)
set_target_properties(pack_collection_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/selftest)
