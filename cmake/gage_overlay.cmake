# Dear ImGui in-game overlay: the vendored ImGui (third_party/imgui, pinned v1.90.4),
# apps/engine/overlay.cpp (D3D11 / Win32 plumbing) and apps/engine/overlay_ui.cpp (the panels)
# as one static library, plus the brand artwork baked into generated headers
# (gen/overlay_*.h) by tools/embed_bytes.cmake.
#
# Included from the top-level CMakeLists.txt (before the gage_mp targets):
#     include(${CMAKE_SOURCE_DIR}/cmake/gage_overlay.cmake)
# and linked with `gage_overlay` by every target that compiles apps/engine/engine.cpp
# (gage_mp and gage_mp_test). Needs the gage_common target (logger + include root libs/).
if(NOT DEFINED GAGE_ROOT)
    set(GAGE_ROOT ${CMAKE_SOURCE_DIR})
endif()

# name | brand file (relative to brand/) | C symbol. One generated header per asset:
#   gen/overlay_<name>.h defines <symbol> and <symbol>Len (see overlay.cpp).
set(GAGE_OVERLAY_ASSETS
    # Solar RP NOON art, rasterised from brand/solar/solar-rp-noon/*.svg into brand/solar/derived by
    # tools/brand_apparel/derive_solar.mjs. The mark is 1:2 (32 x 64, 128 x 256, 512 x 1024).
    "mark64|solar/derived/solar-rp-mark-full-64.png|kOverlayMark64Png"
    "mark256|solar/derived/solar-rp-mark-full-256.png|kOverlayMark256Png"
    "mark1024|solar/derived/solar-rp-mark-ink-1024.png|kOverlayMark1024Png"
    "lockup|solar/derived/solar-rp-lockup-horizontal-full-256h.png|kOverlayLockupPng"
    # The loader's pictures (0.4.22, GI's pick from fresh_images/): GTA V official artwork, cropped to 16:9 from the
    # top, 1920x1080 JPEG, with a soft scrim baked into the bottom-left corner over the artwork's logo. Decoded by WIC
    # like the PNGs; the loader cycles through them (apps/engine/overlay_ui.cpp DrawLoader). The order here is the
    # order kOverlaySceneJpg lists in overlay.cpp.
    "scene0|loader/loader-art-beach-weather.jpg|kOverlayScene0Jpg"
    "scene1|loader/loader-art-chop.jpg|kOverlayScene1Jpg"
    "scene2|loader/loader-art-franklin-and-chop.jpg|kOverlayScene2Jpg"
    "scene3|loader/loader-art-michael-business.jpg|kOverlayScene3Jpg"
    "scene4|loader/loader-art-michael-sweatshop.jpg|kOverlayScene4Jpg"
    "scene5|loader/loader-art-simeon.jpg|kOverlayScene5Jpg"
    "scene6|loader/loader-art-starlet.jpg|kOverlayScene6Jpg"
    "scene7|loader/loader-art-stop-and-frisk.jpg|kOverlayScene7Jpg"
    "scene8|loader/loader-art-tao-cheng.jpg|kOverlayScene8Jpg"
    "scene9|loader/loader-art-trevor-firestarter.jpg|kOverlayScene9Jpg"
)
set(GAGE_OVERLAY_GENERATED "")
foreach(asset IN LISTS GAGE_OVERLAY_ASSETS)
    string(REPLACE "|" ";" parts "${asset}")
    list(GET parts 0 a_name)
    list(GET parts 1 a_file)
    list(GET parts 2 a_symbol)
    set(a_in  ${GAGE_ROOT}/brand/${a_file})
    set(a_out ${CMAKE_BINARY_DIR}/gen/overlay_${a_name}.h)
    add_custom_command(
        OUTPUT ${a_out}
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${a_in}
                -DOUTPUT=${a_out}
                -DSYMBOL=${a_symbol}
                -P ${GAGE_ROOT}/tools/embed_bytes.cmake
        DEPENDS ${a_in} ${GAGE_ROOT}/tools/embed_bytes.cmake
        COMMENT "Embedding ${a_file} into overlay_${a_name}.h"
        VERBATIM
    )
    list(APPEND GAGE_OVERLAY_GENERATED ${a_out})
endforeach()
add_custom_target(gage_overlay_logo DEPENDS ${GAGE_OVERLAY_GENERATED})

add_library(gage_overlay STATIC
    ${GAGE_ROOT}/apps/engine/overlay.cpp
    ${GAGE_ROOT}/apps/engine/overlay_dx12.cpp
    ${GAGE_ROOT}/apps/engine/overlay_ui.cpp
    ${GAGE_ROOT}/third_party/imgui/imgui.cpp
    ${GAGE_ROOT}/third_party/imgui/imgui_draw.cpp
    ${GAGE_ROOT}/third_party/imgui/imgui_tables.cpp
    ${GAGE_ROOT}/third_party/imgui/imgui_widgets.cpp
    ${GAGE_ROOT}/third_party/imgui/backends/imgui_impl_dx11.cpp
    ${GAGE_ROOT}/third_party/imgui/backends/imgui_impl_dx12.cpp
    ${GAGE_ROOT}/third_party/imgui/backends/imgui_impl_win32.cpp
)
add_dependencies(gage_overlay gage_overlay_logo)
# GCC 13 false positive in ImGui 1.90.4's font atlas builder (memset size from an int); upstream code.
set_source_files_properties(${GAGE_ROOT}/third_party/imgui/imgui_draw.cpp
    PROPERTIES COMPILE_OPTIONS -Wno-stringop-overflow)
# Upstream ImGui 1.90.4 casts GetProcAddress's FARPROC to the D3D12SerializeRootSignature PFN.
set_source_files_properties(${GAGE_ROOT}/third_party/imgui/backends/imgui_impl_dx12.cpp
    PROPERTIES COMPILE_OPTIONS -Wno-cast-function-type)
# PRIVATE: the engine sources include only "overlay.h", never ImGui itself.
target_include_directories(gage_overlay PRIVATE
    ${GAGE_ROOT}/third_party/imgui   # "imgui.h", "backends/imgui_impl_*.h"
    ${GAGE_ROOT}/apps/engine         # "overlay.h", "overlay_internal.h", "overlay_imconfig.h"
    ${CMAKE_BINARY_DIR}/gen          # "overlay_mark64.h" and the other generated artwork headers
)
# Our ImGui configuration (logged IM_ASSERT, no shell32) replaces the upstream imconfig.h.
target_compile_definitions(gage_overlay PRIVATE "IMGUI_USER_CONFIG=\"overlay_imconfig.h\"")
target_link_libraries(gage_overlay PUBLIC
    gage_common
    d3d11 d3d12 dxgi d3dcompiler   # D3D11 (Legacy) + D3D12 (Enhanced) devices, swap chain, D3DCompile
    windowscodecs ole32 uuid dxguid # WIC PNG decode, CoCreateInstance, IIDs/CLSIDs (dxguid: D3D12/DXGI IIDs)
    dwmapi gdi32 imm32 user32 # Win32 backend (DWM helpers, DPI caps, IME)
)
