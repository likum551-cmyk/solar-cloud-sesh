// brand_apparel: the GAGE-MP logo on the default character's top, visible to every GAGE client.
//
// How (route 1, run-time texture replacement, one layer below FiveM's ADD_REPLACE_TEXTURE):
//   FiveM's ADD_REPLACE_TEXTURE (extra-natives-five/TextureReplacementNatives.cpp + rage-graphics-five/RenderHooks.cpp
//   SetTextureHook) swaps the grcTexture* the game binds for a replacement at draw time. We do the same one level
//   down, on the D3D11 shader-resource-view the game binds with ID3D11DeviceContext::PSSetShaderResources, so no
//   game signature and no grcTexture layout is needed (the only inputs are the D3D11 vtable and the swap chain the
//   engine already holds):
//     1. hook PSSetShaderResources and CreateShaderResourceView through the vtables of the game's own device;
//     2. every SRV the game binds is classified once (cheap): a 2D BC1/BC3 texture of the stock tee's size is a candidate;
//     3. a candidate is copied to a staging texture and read back asynchronously (DO_NOT_WAIT, no stall); its small
//        mips are hashed (FNV-1a 64) and compared with the fingerprint of the stock "Sky Blue Tee" texture
//        (mp_m_freemode_01 jbib_diff_008_k_uni, derived offline from the player's own game files; the hash is the
//        only thing of it in the repo);
//     4. on a match the GAGE decals (apps/engine/brand_apparel_data.inc, our artwork baked into the tee's UV space
//        by tools/brand_apparel/make_decals.mjs) are blended into the 4x4 blocks they touch, every other block is
//        copied bit-exact, a new immutable texture + SRV with the same description is created, and from then on the
//        bind hook hands the game's draw calls the branded SRV instead of the stock one.
//   Every GAGE client does this, and the look is synced as plain component/drawable/texture indices (PlayerLook /
//   PeerLook -> World::ApplyPedLook), so a remote ped wearing top 11/8/10 draws the same branded texture.
//
// Measured: a D3D11 context/device object carries its own vtable in its own heap block (a deferred context has
// another one each), so the two patched slots belong to exactly the immediate context and device the engine holds;
// brand_apparel keeps a reference on both while hooked and restores the slots before releasing them. Draws recorded on
// a deferred context are not covered (the "binds" counter in the status line shows 0 if the game works that way).
// Nothing here touches the game's files or memory except those two vtable slots (restored by Shutdown). Setting:
// HKCU\Software\GAGE-MP\brand_apparel ("0" turns it off, anything else or unset = on; the launcher copies the
// brand_apparel key of gage_mp.cfg there like peer_drive). Every outcome is logged with the prefix "brand apparel:".
//
// Threading: Poll() runs on the render thread (once per Present, from engine.cpp UiFrame). The two hooks run on
// whatever thread the game calls the D3D11 context/device from. NoteDressed() runs on the script thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace gage::game::brand {

    // The stock item the branding rides on (must match the top in engine.cpp's kPlayerOutfit).
    constexpr int kTopComponent = 11;
    constexpr int kTopDrawable  = 8;
    constexpr int kTopTexture   = 10;

    // True unless HKCU\Software\GAGE-MP\brand_apparel is "0". Read once.
    bool Enabled();

    // Render thread, once per Present, with the game's IDXGISwapChain*. Installs the hooks on the first call that
    // carries a D3D11 swap chain, then pumps the readback jobs. Cheap when idle; safe to call every frame.
    void Poll(void* swapChain);

    // Script thread: DressPlayer has put the branded top on a ped (`when` is the DressPlayer label). Logs what is known.
    void NoteDressed(const char* when);

    // Restore the two vtable slots and drop every D3D object. Safe to call twice and when never installed.
    void Shutdown();

    struct Stats {
        bool     enabled = false;
        bool     installed = false;      // both vtable slots patched
        uint64_t binds = 0;              // PSSetShaderResources calls seen
        uint32_t srvSeen = 0;            // distinct SRVs classified
        uint32_t candidates = 0;         // right size/format, queued for the fingerprint
        uint32_t fingerprinted = 0;      // read back and hashed
        uint32_t applied = 0;            // stock texture instances replaced (live)
    };
    Stats GetStats();

    // ---- test seam (tests/brand_apparel_test.cpp, tools): not used by the engine ---------------------------------
    namespace testing {
        // FNV-1a 64 over the bytes of every mip whose width is <= 64, smallest-index (largest) first. mips[l] holds
        // the tightly packed blocks of level l.
        uint64_t TailHash(int width, int levels, const std::vector<std::vector<uint8_t>>& mips);

        void EncodeBC1Block(const float rgb[16][3], uint8_t out[8]);            // 0..255 floats, opaque 4-colour mode
        void DecodeBC1Block(const uint8_t in[8], float rgb[16][3], bool* threeColour = nullptr);

        struct Decal { int x, y, w, h; std::vector<uint8_t> a8; uint8_t rgb[3]; };   // base-space (baseSize) rect + alpha
        // Blend `decals` (authored at baseSize px) into a BC1 (bytesPerBlock 8) or BC3 (16) chain of a texture whose
        // top level is `width` px square. Untouched blocks stay bit-exact. Returns the number of blocks re-encoded
        // (-1 on bad arguments). Levels narrower than baseSize/16 are left alone.
        int CompositeChain(int width, int levels, int bytesPerBlock, int baseSize, const std::vector<Decal>& decals,
                           std::vector<std::vector<uint8_t>>& mips);

        void Reset();                                                           // forget every target, slot and job
        bool AddTarget(const char* name, uint64_t tailHash, bool bc3, int baseSize, int levels,
                       const std::vector<Decal>& decals);                       // register an extra fingerprint
        bool InstallOn(ID3D11Device* dev, ID3D11DeviceContext* immediateCtx);   // hook this device's vtables
        void Pump(ID3D11DeviceContext* immediateCtx);                           // the per-frame work of Poll()
    }

}  // namespace gage::game::brand
