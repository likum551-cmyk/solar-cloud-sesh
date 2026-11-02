// GAGE-MP's Dear ImGui build configuration, selected with
//   -DIMGUI_USER_CONFIG="overlay_imconfig.h"   (cmake/gage_overlay.cmake)
// third_party/imgui/imconfig.h stays the untouched upstream file.
#pragma once

// An ImGui assert is one logged line, never abort(): this code lives inside the
// game process. Defined in overlay.cpp.
void GageImGuiAssertFailed(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : GageImGuiAssertFailed(#_EXPR, __FILE__, __LINE__))

// No imgui_demo.cpp is vendored: nothing may reference the demo windows.
#define IMGUI_DISABLE_DEMO_WINDOWS
// Keep the upstream API surface current only; no ShellExecute (no shell32 link).
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
