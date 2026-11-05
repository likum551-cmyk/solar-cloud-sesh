// Seam between overlay.cpp (D3D11 / Win32 plumbing) and overlay_ui.cpp (the panels).
// Not part of the engine's interface: only the two overlay sources include it.
#pragma once

namespace gage::game::Overlay::detail {

    // THE brand-art switch. The old GAGE "G" art is retired; the Solar RP NOON art is
    // brand/solar/derived (made from brand/solar/solar-rp-noon by tools/brand_apparel/derive_solar.mjs).
    //   false: overlay.cpp loads none of those four PNGs and the UI draws its typeset fallbacks:
    //          the "SR" chip in the corner and the "SOLAR RP" wordmark on the loader. The real GTA V
    //          stills behind the loader are not brand art and load either way.
    //   true:  the PNGs embedded from brand/solar/derived (cmake/gage_overlay.cmake) are drawn.
    constexpr bool kUseBrandArt = true;

    // Textures overlay.cpp decoded for the UI (any may be null: the UI has a designed fallback).
    // The Solar S is 1:2 (viewBox 50 x 100), so the three mark textures are 1:2 too (32 x 64, 128 x 256,
    // 512 x 1024) and the UI draws them at that aspect.
    struct UiTextures {
        void* mark64 = nullptr;      // Solar mark, ink + gold hook, 64 px high   (corner mark up to ~72 px)
        void* mark256 = nullptr;     // 256 px high                               (larger marks)
        void* mark1024 = nullptr;    // 1024 px high, all ink                     (the loader's background mark)
        void* lockup = nullptr;      // horizontal lockup (mark + wordmark), 256 px high, ink + gold hook, with 4 units of clear space
        int   lockupW = 0, lockupH = 0;
        void* scenes[10] = {};       // the loader's GTA V pictures (16:9), cycled; the first sceneCount are valid
        int   sceneCount = 0;
    };
    constexpr int kLoaderScenes = 10; // = the scene entries in cmake/gage_overlay.cmake and UiTextures::scenes

    // Build the fonts and the overlay style. ImGui's context must be current and the
    // backends not yet initialised (the font atlas is uploaded on the first frame).
    // scale is display height / 1080.
    void UiInit(float scale);

    void UiSetTextures(const UiTextures& t);

    // One UI frame, between ImGui::NewFrame and ImGui::Render, render thread.
    void UiFrame();

    // Forget everything tied to the ImGui context (fonts, textures, open panels). The
    // pushed data (chat history, queues, phase) survives so a re-init loses nothing.
    void UiReset();

    // Chat or quick actions open (same as Overlay::Interactive).
    bool UiInteractive();
}

// DX12 backend (overlay_dx12.cpp), for GTA V Enhanced. The public Overlay::Init/Render/
// Shutdown/... in overlay.cpp dispatch here when the game's swap chain is Direct3D 12
// (no ID3D11Device). The D3D11 backend in overlay.cpp is untouched. These mirror the
// public API's backend-specific half; the Win32 input/WndProc and device plumbing are
// self-contained in overlay_dx12.cpp so the Legacy path stays byte-identical.
namespace gage::game::Overlay::dx12 {
    bool Init(void* swapChain);
    void Render(void* swapChain);
    void Shutdown();
    bool Ready();
    bool WantsInput();
    bool Interactive();
    bool LoadPngTexture(const unsigned char* bytes, size_t n, void** outTexture, int* outW, int* outH);
    void FreeTexture(void* texture);
}
