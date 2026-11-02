// GAGE-MP D3D11 loading overlay — renders directly onto the game's back buffer.
//
// Runs inside the Present detour (before the actual Present swaps). Draws via
// ClearRenderTargetView (branded dark background) + UpdateSubresource (text
// bitmaps blitted onto the back buffer). No shaders, no D2D1, no GDI+,
// no extra DLLs — pure D3D11 COM calls through vtable indices.
//
// The bitmap font is a 5×7 pixel grid per character, scaled up. The spinner
// is three dots with pulsing alpha.

#include "loading_screen.h"
#include "common/logger.h"

#include <windows.h>
#include <cstring>
#include <cstdint>
#include <cmath>

using namespace gage;

namespace {

// ---------------------------------------------------------------------------
// COM vtable call helper (no headers needed)
// ---------------------------------------------------------------------------
template<typename R, typename... Args>
R comCall(void* obj, int vt, Args... args) {
    using Fn = R(__stdcall*)(void*, Args...);
    return ((Fn)(*(void***)obj)[vt])(obj, args...);
}

void comRelease(void* obj) { if (obj) comCall<unsigned long>(obj, 2); }

// D3D11 struct (same layout as the SDK)
struct D3D11_BOX { uint32_t left, top, front, right, bottom, back; };

// DXGI_SWAP_CHAIN_DESC: we only read the first 8 bytes (Width + Height)
// and the Format at offset 28. Total struct is 72 bytes on x64.
struct SwapDesc {
    uint32_t Width, Height;             // +0, +4
    uint32_t RefreshNum, RefreshDen;    // +8, +12  (DXGI_RATIONAL)
    uint32_t Format;                    // +16 (DXGI_MODE_DESC::Format)
    uint32_t ScanlineOrdering;          // +20
    uint32_t Scaling;                   // +24
    uint32_t SampleCount, SampleQual;   // +28, +32
    uint32_t BufferUsage;               // +36
    uint32_t BufferCount;               // +40
    uint64_t OutputWindow;              // +44 (HWND, 8 bytes on x64)
    uint32_t Windowed;                  // +52
    uint32_t SwapEffect;                // +56
    uint32_t Flags;                     // +60
};

// IID_ID3D11Texture2D {6f15aaf2-d208-4e89-9ab4-489535d34f9c}
struct COMGUID { uint32_t a; uint16_t b, c; uint8_t d[8]; };
const COMGUID kIIDTexture2D = { 0x6f15aaf2, 0xd208, 0x4e89,
    {0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c} };

// ---------------------------------------------------------------------------
// 5×7 bitmap font — uppercase letters + a few symbols
// ---------------------------------------------------------------------------
// Each glyph: 7 rows of 5 bits. Row 0 = top. Bit 4 = leftmost column.
// Stored as uint32_t: bits[0..4] = row0, [5..9] = row1, ..., [30..34] = row6.
static uint32_t charGlyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    switch (ch) {
    case 'A': return 0x22FBE880 | 0x04400000; // 01110 10001 10001 11111 10001 10001 10001
    // Encoded more readably: each 5-bit row from top to bottom.
    // To keep this compact, we define them as hex of the 35-bit pattern.
    default: break;
    }
    // Full glyph table:
    static const uint32_t glyphs[128] = {
    //         row0    row1    row2    row3    row4    row5    row6
    // Pack: (r0<<30)|(r1<<25)|(r2<<20)|(r3<<15)|(r4<<10)|(r5<<5)|r6
    // Each row is 5 bits wide: e.g., 0b10001 = 0x11, 0b01110 = 0x0E
    // (no ['A'] = ... designators: g++ rejects array designators in C++)
    };
    // This packing is error-prone at compile time. Use a runtime init instead.
    // But we can't have a static init in -fno-exceptions easily. So let's use
    // a simpler row-based representation.
    (void)glyphs;
    return 0; // placeholder — we'll use the row-array approach below
}

// Row-based glyph data: 7 uint8_t per character, each uint8_t has 5 bits (MSB=left).
// Only A-Z, 0-9, and a few punctuation marks.
static const uint8_t kFont[][7] = {
    // A
    { 0x04, 0x0A, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    // B
    { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
    // C
    { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
    // D
    { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E },
    // E
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },
    // F
    { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
    // G
    { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E },
    // H
    { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
    // I
    { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
    // J
    { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },
    // K
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },
    // L
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
    // M
    { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },
    // N
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },
    // O
    { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    // P
    { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },
    // Q (unused but complete)
    { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },
    // R
    { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
    // S
    { 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E },
    // T
    { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
    // U
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
    // V
    { 0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04 },
    // W
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },
    // X
    { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
    // Y
    { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
    // Z
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
};

static const uint8_t kFontDot[7] = { 0,0,0,0,0,0, 0x04 }; // '.'
static const uint8_t kFontBlank[7] = { 0,0,0,0,0,0,0 };   // ' '

const uint8_t* getGlyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 'A' && ch <= 'Z') return kFont[ch - 'A'];
    if (ch == '.') return kFontDot;
    return kFontBlank; // space and unknown
}

// Paint a string into a pixel buffer (BGRA format, bottom byte = B).
void paintText(uint32_t* buf, int bufW, int bufH,
               const char* text, int cx, int cy, int scale, uint32_t color)
{
    int len = 0;
    for (const char* p = text; *p; ++p) ++len;
    int totalW = len > 0 ? (len * 6 - 1) * scale : 0;
    int startX = cx - totalW / 2;
    int startY = cy - (7 * scale) / 2;

    int x0 = startX;
    for (const char* p = text; *p; ++p) {
        const uint8_t* glyph = getGlyph(*p);
        for (int row = 0; row < 7; ++row) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 5; ++col) {
                if (!(bits & (0x10 >> col))) continue;
                for (int sy = 0; sy < scale; ++sy) {
                    for (int sx = 0; sx < scale; ++sx) {
                        int px = x0 + col * scale + sx;
                        int py = startY + row * scale + sy;
                        if (px >= 0 && px < bufW && py >= 0 && py < bufH)
                            buf[py * bufW + px] = color;
                    }
                }
            }
        }
        x0 += 6 * scale;
    }
}

// Paint a filled circle.
void paintDot(uint32_t* buf, int bufW, int bufH,
              int cx, int cy, int radius, uint32_t color)
{
    int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx*dx + dy*dy > r2) continue;
            int px = cx + dx, py = cy + dy;
            if (px >= 0 && px < bufW && py >= 0 && py < bufH)
                buf[py * bufW + px] = color;
        }
    }
}

// ---------------------------------------------------------------------------
// Overlay state
// ---------------------------------------------------------------------------
struct OverlayState {
    bool        inited = false;
    bool        dismissed = false;
    int         frame = 0;
    void*       rtv = nullptr;       // ID3D11RenderTargetView*
    void*       staging = nullptr;   // ID3D11Texture2D* (STAGING)
    void*       lastSC = nullptr;
    uint32_t    width = 0, height = 0;
    uint32_t    format = 0;
    uint32_t*   pixels = nullptr;    // CPU pixel buffer (W*H*4)
};
OverlayState g_ov;

void Cleanup() {
    comRelease(g_ov.rtv);    g_ov.rtv = nullptr;
    comRelease(g_ov.staging); g_ov.staging = nullptr;
    if (g_ov.pixels) { VirtualFree(g_ov.pixels, 0, MEM_RELEASE); g_ov.pixels = nullptr; }
    g_ov.inited = false;
    g_ov.lastSC = nullptr;
}

bool InitD3D(void* sc) {
    Cleanup();

    // IDXGISwapChain::GetDesc — vtable[12]
    SwapDesc desc = {};
    if (comCall<long>(sc, 12, &desc) < 0) return false;
    g_ov.width = desc.Width;
    g_ov.height = desc.Height;
    g_ov.format = desc.Format;

    // GetBuffer(0, IID_ID3D11Texture2D, &bb)
    void* bb = nullptr;
    if (comCall<long>(sc, 9, 0u, &kIIDTexture2D, &bb) < 0 || !bb) return false;

    // bb → GetDevice → device
    void* dev = nullptr;
    comCall<void>(bb, 3, &dev);
    if (!dev) { comRelease(bb); return false; }

    // CreateRenderTargetView(bb, nullptr, &rtv) — ID3D11Device vtable[9]
    void* rtv = nullptr;
    if (comCall<long>(dev, 9, bb, nullptr, &rtv) < 0 || !rtv) {
        comRelease(bb); comRelease(dev); return false;
    }
    g_ov.rtv = rtv;

    // Allocate CPU pixel buffer
    size_t bufSize = (size_t)g_ov.width * g_ov.height * 4;
    g_ov.pixels = (uint32_t*)VirtualAlloc(nullptr, bufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    comRelease(bb);
    comRelease(dev);

    g_ov.lastSC = sc;
    g_ov.inited = true;
    GLOGI("loading_screen: D3D11 overlay init (%ux%u fmt=%u)", g_ov.width, g_ov.height, g_ov.format);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public: called from Frame() every present while loading
// ---------------------------------------------------------------------------
void gage::game::RenderLoadingOverlay(void* swapChain, bool dismiss) {
    if (!swapChain) return;
    if (dismiss && g_ov.inited) {
        Cleanup();
        g_ov.dismissed = true;
        GLOGI("loading_screen: dismissed");
        return;
    }
    if (g_ov.dismissed) return;

    if (!g_ov.inited || g_ov.lastSC != swapChain) {
        if (!InitD3D(swapChain)) return;
    }

    g_ov.frame++;
    const int W = (int)g_ov.width;
    const int H = (int)g_ov.height;
    if (!g_ov.pixels || W < 100 || H < 100) return;

    // ---- Paint the CPU buffer ------------------------------------------------
    // Background: #0E0F12 (BGRA)
    const uint32_t bgColor = 0xFF120F0E; // ABGR in memory = BGRA pixel
    for (int i = 0; i < W * H; ++i)
        g_ov.pixels[i] = bgColor;

    // "SOLAR RP" — large, white, centered vertically at ~40% (this legacy painter's 5x7 font is caps only)
    int titleScale = H / 60; if (titleScale < 4) titleScale = 4;
    paintText(g_ov.pixels, W, H, "SOLAR RP", W/2, H * 38 / 100, titleScale, 0xFFFFFFFF);

    // "MULTIPLAYER" — smaller, accent blue (#3B82F6 → BGRA: F6823B)
    int subScale = H / 120; if (subScale < 2) subScale = 2;
    paintText(g_ov.pixels, W, H, "MULTIPLAYER", W/2, H * 52 / 100, subScale, 0xFFF6823B);

    // Accent line
    int lineY = H * 58 / 100;
    int lineHalf = W / 10;
    uint32_t lineColor = 0x80F6823B; // semi-transparent blue
    for (int x = W/2 - lineHalf; x < W/2 + lineHalf; ++x) {
        if (x >= 0 && x < W && lineY >= 0 && lineY < H)
            g_ov.pixels[lineY * W + x] = lineColor;
    }

    // 3 pulsing dots
    int dotY = H * 65 / 100;
    int dotR = H / 120; if (dotR < 3) dotR = 3;
    int dotSpacing = H / 25;
    for (int i = 0; i < 3; ++i) {
        float phase = (float)(g_ov.frame % 90) / 90.0f * 6.2832f - (float)i * 1.2f;
        float pulse = (sinf(phase) + 1.0f) / 2.0f;
        uint8_t alpha = (uint8_t)(80 + 175 * pulse);
        uint32_t dotColor = ((uint32_t)alpha << 24) | 0xF6823B; // accent blue
        int dx = W/2 + (i - 1) * dotSpacing;
        paintDot(g_ov.pixels, W, H, dx, dotY, dotR, dotColor);
    }

    // "LOADING..." — small white text
    int statusScale = H / 180; if (statusScale < 1) statusScale = 1;
    paintText(g_ov.pixels, W, H, "LOADING...", W/2, H * 75 / 100, statusScale, 0xCCCCCCCC);

    // ---- Upload to GPU -------------------------------------------------------
    // Get back buffer + context
    void* bb = nullptr;
    if (comCall<long>(swapChain, 9, 0u, &kIIDTexture2D, &bb) < 0 || !bb) return;
    void* dev = nullptr;
    comCall<void>(bb, 3, &dev);
    if (!dev) { comRelease(bb); return; }
    void* ctx = nullptr;
    comCall<void>(dev, 40, &ctx); // GetImmediateContext
    comRelease(dev);
    if (!ctx) { comRelease(bb); return; }

    // UpdateSubresource(bb, 0, nullptr, pixels, rowPitch, 0) — vtable[48]
    comCall<void>(ctx, 48, bb, 0u, nullptr, g_ov.pixels, (uint32_t)(W * 4), 0u);

    comRelease(ctx);
    comRelease(bb);
}
