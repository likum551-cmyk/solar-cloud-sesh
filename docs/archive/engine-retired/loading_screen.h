// GAGE-MP D3D11 loading overlay: renders branded splash directly onto the
// game's back buffer via the Present hook. No separate window needed.
#pragma once

namespace gage::game {
    // Called from Frame() each tick. Renders the overlay when the engine isn't
    // live yet (natives not ready / player not spawned). swapChain is the
    // IDXGISwapChain* captured from the Present detour (GetSwapChain()).
    // Automatically cleans up its D3D11 resources once dismissed.
    void RenderLoadingOverlay(void* swapChain, bool dismiss);
}
