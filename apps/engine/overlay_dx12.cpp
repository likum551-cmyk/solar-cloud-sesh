// GAGE-MP in-game overlay — Direct3D 12 backend, for GTA V Enhanced (GTA5_Enhanced.exe).
//
// Parallel to overlay.cpp's D3D11 backend (GTA V Legacy). The public Overlay::Init /
// Render / Shutdown / Ready / WantsInput / LoadPngTexture / FreeTexture in overlay.cpp
// dispatch here when the game's swap chain is Direct3D 12 (its GetDevice does not return
// an ID3D11Device). The D3D11 path is untouched; this file is self-contained.
//
// The in-game case differs from a standalone DX12 app in three ways, all handled here:
//   1. We do not own the device, queue or swap chain. The device comes from a back-buffer
//      resource (a DX12 swap chain's GetDevice does not return the device). The command
//      queue is captured by patching ID3D12CommandQueue::ExecuteCommandLists (vtable slot
//      10) via a throwaway device: the hook records the first DIRECT queue the game submits
//      on. Deferred init — install the hook, let the game's queue be captured over a frame
//      or two, and only THEN initialise ImGui (whose one-shot font-upload queue must not be
//      the one we capture). This ordering is proven (scratch dx12_overlay_test.cpp: PASS).
//   2. We draw ON TOP of the game's finished frame: barrier PRESENT->RENDER_TARGET, render
//      ImGui's draw data (no clear), barrier back to PRESENT, submit our list on the captured
//      queue — then return so the game runs the original Present itself. We never Present.
//   3. We fetch the back buffer fresh every frame and release it before returning, so we
//      hold no outstanding reference when the game calls ResizeBuffers between frames.
//
// Threading mirrors overlay.cpp: Init / Render / Shutdown run on the render thread (the
// Present detour); WantsInput() may be called from any thread; the WndProc subclass runs on
// the window thread, and one critical section guards the ImGui input state the two threads
// share. The ExecuteCommandLists hook runs on the game's render thread and touches only
// static state, so it is installed once and left in place for the DLL's lifetime.
//
// Textures (the brand marks, the lockup and the loader's GTA V stills) go through LoadPngTexture
// below: a WIC decode to 32bppRGBA, a default-heap texture filled through an upload buffer on a
// one-shot command list submitted to the captured queue, and one SRV per texture in g.srvHeap
// slots 1..63 (slot 0 is ImGui's font). The texture's ImTextureID is its GPU descriptor handle,
// which is what imgui_impl_dx12 passes to SetGraphicsRootDescriptorTable. Any failure is logged
// and the UI draws its designed fallback for that image.
#include "overlay.h"
#include "overlay_internal.h"
#include "common/logger.h"
#include "common/registry.h"
#include "sync/ped_preview.h"   // 0.5.8: the probe's Verdict

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

// The backend header keeps this declaration in an '#if 0' block on purpose (as in overlay.cpp).
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// GageImGuiAssertFailed (overlay_imconfig.h's IM_ASSERT) is defined once in overlay.cpp.

namespace gage::game::Overlay::dx12 {

namespace {

// ---------------------------------------------------------------------------
// Small helpers (local copies: overlay.cpp's are file-local)
// ---------------------------------------------------------------------------
template <typename T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Typeless back-buffer formats cannot make a view directly; resolve to a concrete one.
DXGI_FORMAT RtvFormatFor(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:                                return f;
    }
}

float UiScaleForHeight(float h)
{
    if (h <= 1.0f) return 1.0f;
    float s = h / 1080.0f;
    if (s < 0.75f) s = 0.75f;
    if (s > 3.0f)  s = 3.0f;
    return s;
}

struct CritSec {
    CRITICAL_SECTION cs;
    CritSec() { InitializeCriticalSection(&cs); }   // never deleted: lives as long as the DLL
};
CritSec g_cs;
struct Lock {
    Lock() { EnterCriticalSection(&g_cs.cs); }      // recursive IME re-entry is not possible here, but matches overlay.cpp
    ~Lock() { LeaveCriticalSection(&g_cs.cs); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};

constexpr int       kMaxInitTries  = 5;
constexpr ULONGLONG kInitRetryMs   = 2000;
constexpr ULONGLONG kOpenKeyCharMs = 150;        // same window as overlay.cpp (T opens chat)
constexpr UINT      kMaxBackBuffers = 8;

// ---------------------------------------------------------------------------
// ExecuteCommandLists capture (process-wide ID3D12CommandQueue vtable patch, slot 10)
// ---------------------------------------------------------------------------
typedef void (STDMETHODCALLTYPE* ExecCmdLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
ExecCmdLists_t      g_origExec = nullptr;
ID3D12CommandQueue* g_capturedQueue = nullptr;   // the game's DIRECT (graphics) queue; not owned

void STDMETHODCALLTYPE HookExecuteCommandLists(ID3D12CommandQueue* self, UINT n, ID3D12CommandList* const* lists)
{
    if (!g_capturedQueue) {
        const D3D12_COMMAND_QUEUE_DESC d = self->GetDesc();   // the overlay renders on the DIRECT queue only
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            g_capturedQueue = self;
    }
    g_origExec(self, n, lists);
}

// Reach the shared ID3D12CommandQueue vtable through a throwaway device + queue and patch
// slot 10. Idempotent. Left installed for the DLL's lifetime (the hook touches only statics),
// so a device/swap-chain teardown never has to unpatch a vtable the game thread may be in.
bool InstallExecHook()
{
    ID3D12Device* dummyDev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, (void**)&dummyDev)) || !dummyDev) {
        GLOGW("overlay[dx12]: D3D12CreateDevice (dummy) failed: cannot reach the command-queue vtable");
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* dummyQ = nullptr;
    dummyDev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, (void**)&dummyQ);
    bool ok = false;
    if (dummyQ) {
        void** vtbl = *reinterpret_cast<void***>(dummyQ);
        if (reinterpret_cast<void*>(vtbl[10]) == reinterpret_cast<void*>(&HookExecuteCommandLists)) {
            ok = true;     // already patched (re-init after a teardown)
        } else {
            g_origExec = reinterpret_cast<ExecCmdLists_t>(vtbl[10]);
            DWORD old = 0;
            if (VirtualProtect(&vtbl[10], sizeof(void*), PAGE_READWRITE, &old)) {
                vtbl[10] = reinterpret_cast<void*>(&HookExecuteCommandLists);
                VirtualProtect(&vtbl[10], sizeof(void*), old, &old);
                ok = true;
            } else {
                GLOGW("overlay[dx12]: VirtualProtect on the command-queue vtable failed");
            }
        }
        dummyQ->Release();
    } else {
        GLOGW("overlay[dx12]: dummy CreateCommandQueue failed");
    }
    dummyDev->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
struct FrameCtx {
    ID3D12CommandAllocator* alloc = nullptr;
    UINT64                  fenceValue = 0;
};

constexpr UINT kSrvSlots = 64;     // the SRV heap's size: slot 0 = ImGui's font, 1..63 = user textures

// A texture LoadPngTexture handed out: its resource and its GPU descriptor handle (the ImTextureID).
struct UserTex {
    ID3D12Resource* res = nullptr;
    UINT64          gpuPtr = 0;
};

struct State {
    std::atomic<bool> ready{false};
    std::atomic<bool> disabled{false};
    std::atomic<int>  inRender{0};
    std::atomic<bool> wantMouse{false};
    std::atomic<bool> wantKeyboard{false};
    std::atomic<ULONGLONG> openKeyArmedAt{0};

    IDXGISwapChain*       sc = nullptr;        // identity only, not AddRef'd
    ID3D12Device*         dev = nullptr;       // owned reference
    ID3D12CommandQueue*   queue = nullptr;     // the captured game queue, NOT owned (do not Release)
    HWND                  hwnd = nullptr;
    ImGuiContext*         imctx = nullptr;
    bool                  win32Init = false;
    bool                  dx12Init = false;

    WNDPROC origProc = nullptr;
    bool    hooked = false;
    HWND    hookedHwnd = nullptr;

    // DX12 render objects
    ID3D12DescriptorHeap*      rtvHeap = nullptr;
    UINT                       rtvStep = 0;
    ID3D12DescriptorHeap*      srvHeap = nullptr;   // shader-visible: ImGui font at slot 0, user textures at 1..63
    UINT                       srvStep = 0;         // CBV/SRV/UAV descriptor increment
    UserTex                    tex[kSrvSlots];      // user textures by SRV slot ([0] is the font and stays empty)
    ID3D12GraphicsCommandList* cmd = nullptr;       // one list, reset onto the current frame's allocator
    ID3D12Fence*               fence = nullptr;
    HANDLE                     fenceEvent = nullptr;
    UINT64                     fenceCounter = 0;
    FrameCtx                   frame[kMaxBackBuffers];
    UINT                       bufferCount = 0;
    DXGI_FORMAT                rtvFormat = DXGI_FORMAT_UNKNOWN;
    UINT                       initW = 0, initH = 0;

    float    uiScale = 1.0f;
    uint32_t frameNo = 0;
    int      getBufferFailLogged = 0;

    bool hookInstalled = false;

    // init back-off
    IDXGISwapChain* failSc = nullptr;
    int             failCount = 0;
    ULONGLONG       nextTry = 0;
};
State g;

struct RenderGuard {
    RenderGuard() { g.inRender.fetch_add(1); }
    ~RenderGuard() { g.inRender.fetch_sub(1); }
};

void WaitFence(UINT64 v)
{
    if (g.fence && g.fenceEvent && v && g.fence->GetCompletedValue() < v) {
        if (SUCCEEDED(g.fence->SetEventOnCompletion(v, g.fenceEvent)))
            WaitForSingleObject(g.fenceEvent, 1000);
    }
}

// As WaitFence, but with the timeout chosen by the caller and the answer returned: true when the GPU has
// passed v. A texture upload frees its staging memory only on true.
bool WaitFenceFor(UINT64 v, DWORD ms)
{
    if (!g.fence || !g.fenceEvent || !v)
        return false;
    if (g.fence->GetCompletedValue() >= v)
        return true;
    if (FAILED(g.fence->SetEventOnCompletion(v, g.fenceEvent)))
        return false;
    WaitForSingleObject(g.fenceEvent, ms);
    return g.fence->GetCompletedValue() >= v;
}

// Releases every user texture. The caller has already waited for the GPU to finish with them
// (WaitFence(g.fenceCounter)); their SRV slots become free again.
void ReleaseAllTextures()
{
    for (UINT i = 0; i < kSrvSlots; ++i) {
        SafeRelease(g.tex[i].res);
        g.tex[i].gpuPtr = 0;
    }
}

// ---------------------------------------------------------------------------
// WndProc subclass (same contract as overlay.cpp; duplicated so Legacy stays untouched)
// ---------------------------------------------------------------------------
bool BlockedFromGame(UINT msg)
{
    if (g.wantMouse.load()) {
        switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
            return true;     // button-ups are never blocked: nothing sticks in the game
        default: break;
        }
    }
    if (g.wantKeyboard.load()) {
        switch (msg) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
        case WM_CHAR: case WM_SYSCHAR: case WM_UNICHAR:
        case WM_IME_CHAR:
            return true;     // key-ups are never blocked
        default: break;
        }
    }
    return false;
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Alt+F4 / close and the quit card first, exactly as on D3D11 (overlay.cpp)
    if (gage::game::Overlay::detail::QuitGate(msg, wp, lp, g.ready.load()))
        return 0;
    if (g.ready.load()) {
        bool openKeyChar = false;
        if (msg == WM_KEYDOWN && wp == 'T' && !(lp & (1 << 30)) && !g.wantKeyboard.load()) {
            g.openKeyArmedAt.store(GetTickCount64());
        } else if (msg == WM_CHAR) {
            const ULONGLONG at = g.openKeyArmedAt.exchange(0);
            openKeyChar = at != 0 && GetTickCount64() - at < kOpenKeyCharMs;
        }
        {
            Lock lk;     // the same input state is read by ImGui::NewFrame on the render thread
            if (g.ready.load() && !openKeyChar) {
                const LRESULT r = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
                if (r)
                    return r;
            }
        }
        if (BlockedFromGame(msg))
            return 0;
    }
    WNDPROC orig = g.origProc;
    if (!orig) {
        Lock lk;
        orig = g.origProc;
    }
    return orig ? CallWindowProcW(orig, hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

bool InstallWndProc(HWND hwnd)
{
    Lock lk;
    const LONG_PTR cur = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (cur == (LONG_PTR)OverlayWndProc) {
        g.hooked = true;
        g.hookedHwnd = hwnd;
        return g.origProc != nullptr;
    }
    SetLastError(0);
    const LONG_PTR prev = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc);
    if (!prev) {
        GLOGW("overlay[dx12]: SetWindowLongPtr(GWLP_WNDPROC) failed (error %lu): no input for the overlay",
              GetLastError());
        return false;
    }
    g.origProc = (WNDPROC)prev;
    g.hooked = true;
    g.hookedHwnd = hwnd;
    return true;
}

void RemoveWndProc()
{
    Lock lk;
    if (!g.hooked)
        return;
    const LONG_PTR cur = GetWindowLongPtrW(g.hookedHwnd, GWLP_WNDPROC);
    if (cur == (LONG_PTR)OverlayWndProc) {
        SetWindowLongPtrW(g.hookedHwnd, GWLP_WNDPROC, (LONG_PTR)g.origProc);
        g.hooked = false;
        g.hookedHwnd = nullptr;
        g.origProc = nullptr;
    } else {
        GLOGW("overlay[dx12]: window procedure was subclassed after the overlay; left in place (forwarding only)");
    }
}

// ---------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------
// Frees everything this backend owns (also partial state of a failed init). Waits for the
// GPU to drain our submissions first. Does NOT release the captured queue (not owned) and
// does NOT unpatch the ExecuteCommandLists hook (kept for the DLL's lifetime).
void Teardown()
{
    g.ready.store(false);
    {
        Lock lk;
        g.wantMouse.store(false);
        g.wantKeyboard.store(false);
    }
    WaitFence(g.fenceCounter);     // our last submission must finish before its resources go

    detail::UiReset();                 // forgets the texture handles before the textures go
    ReleaseAllTextures();
    if (g.imctx)
        ImGui::SetCurrentContext(g.imctx);
    if (g.dx12Init)  { ImGui_ImplDX12_Shutdown();  g.dx12Init = false; }
    if (g.win32Init) { ImGui_ImplWin32_Shutdown(); g.win32Init = false; }
    if (g.imctx)     { ImGui::DestroyContext(g.imctx); g.imctx = nullptr; }

    RemoveWndProc();

    if (g.fenceEvent) { CloseHandle(g.fenceEvent); g.fenceEvent = nullptr; }
    SafeRelease(g.fence);
    SafeRelease(g.cmd);
    for (UINT i = 0; i < kMaxBackBuffers; ++i) {
        SafeRelease(g.frame[i].alloc);
        g.frame[i].fenceValue = 0;
    }
    SafeRelease(g.srvHeap);
    SafeRelease(g.rtvHeap);
    SafeRelease(g.dev);

    g.sc = nullptr;
    g.queue = nullptr;
    g.hwnd = nullptr;
    g.frameNo = 0;
    g.bufferCount = 0;
    g.rtvStep = 0;
    g.srvStep = 0;
    g.fenceCounter = 0;
    g.rtvFormat = DXGI_FORMAT_UNKNOWN;
    g.initW = g.initH = 0;
    g.getBufferFailLogged = 0;
}

bool InitNew(IDXGISwapChain* sc)
{
    // The device: from a back-buffer resource. A DX12 swap chain's GetDevice does not
    // return an ID3D12Device, but every resource it hands out can name its device.
    ID3D12Resource* probe = nullptr;
    HRESULT hr = sc->GetBuffer(0, IID_ID3D12Resource, (void**)&probe);
    if (FAILED(hr) || !probe) {
        GLOGW("overlay[dx12]: GetBuffer(0) for the device probe failed (0x%08lx)", (unsigned long)hr);
        return false;
    }
    ID3D12Device* dev = nullptr;
    hr = probe->GetDevice(IID_ID3D12Device, (void**)&dev);
    probe->Release();
    if (FAILED(hr) || !dev) {
        GLOGW("overlay[dx12]: back buffer has no ID3D12Device (0x%08lx)", (unsigned long)hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc = {};
    hr = sc->GetDesc(&desc);
    if (FAILED(hr) || !desc.OutputWindow || !IsWindow(desc.OutputWindow)) {
        GLOGW("overlay[dx12]: swap chain has no valid OutputWindow (GetDesc 0x%08lx)", (unsigned long)hr);
        dev->Release();
        return false;
    }
    UINT bc = desc.BufferCount;
    if (bc < 2) bc = 2;
    if (bc > kMaxBackBuffers) bc = kMaxBackBuffers;

    g.sc = sc;
    g.dev = dev;                       // owned, released by Teardown
    g.queue = g_capturedQueue;         // the game's DIRECT queue (not owned)
    g.hwnd = desc.OutputWindow;
    g.bufferCount = bc;
    g.rtvFormat = RtvFormatFor(desc.BufferDesc.Format);
    g.initW = desc.BufferDesc.Width;
    g.initH = desc.BufferDesc.Height;

    float hpx = (float)desc.BufferDesc.Height;
    if (hpx < 1.0f) {
        RECT rc = {};
        GetClientRect(g.hwnd, &rc);
        hpx = (float)(rc.bottom - rc.top);
    }
    g.uiScale = UiScaleForHeight(hpx);

    // RTV heap: one view per back buffer (recreated per frame onto the current index).
    D3D12_DESCRIPTOR_HEAP_DESC rh = {};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = bc;
    if (FAILED(dev->CreateDescriptorHeap(&rh, IID_ID3D12DescriptorHeap, (void**)&g.rtvHeap))) {
        GLOGW("overlay[dx12]: CreateDescriptorHeap(RTV) failed"); Teardown(); return false;
    }
    g.rtvStep = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // SRV heap: shader-visible, ImGui's font SRV at slot 0, user textures (LoadPngTexture) at slots 1..63.
    D3D12_DESCRIPTOR_HEAP_DESC sh = {};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = kSrvSlots;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&sh, IID_ID3D12DescriptorHeap, (void**)&g.srvHeap))) {
        GLOGW("overlay[dx12]: CreateDescriptorHeap(SRV) failed"); Teardown(); return false;
    }
    g.srvStep = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (UINT i = 0; i < bc; ++i) {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator,
                                               (void**)&g.frame[i].alloc))) {
            GLOGW("overlay[dx12]: CreateCommandAllocator[%u] failed", i); Teardown(); return false;
        }
    }
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.frame[0].alloc, nullptr,
                                      IID_ID3D12GraphicsCommandList, (void**)&g.cmd))) {
        GLOGW("overlay[dx12]: CreateCommandList failed"); Teardown(); return false;
    }
    g.cmd->Close();
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_ID3D12Fence, (void**)&g.fence))) {
        GLOGW("overlay[dx12]: CreateFence failed"); Teardown(); return false;
    }
    g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g.fenceEvent) {
        GLOGW("overlay[dx12]: CreateEvent failed"); Teardown(); return false;
    }

    IMGUI_CHECKVERSION();
    g.imctx = ImGui::CreateContext();
    if (!g.imctx) { GLOGW("overlay[dx12]: ImGui::CreateContext failed"); Teardown(); return false; }
    ImGui::SetCurrentContext(g.imctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // the game owns the OS cursor
    detail::UiInit(g.uiScale);                                // fonts + GAGE style before the backend builds the atlas

    if (!ImGui_ImplWin32_Init(g.hwnd)) { GLOGW("overlay[dx12]: ImGui_ImplWin32_Init failed"); Teardown(); return false; }
    g.win32Init = true;

    const D3D12_CPU_DESCRIPTOR_HANDLE fontCpu = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE fontGpu = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(dev, (int)bc, g.rtvFormat, g.srvHeap, fontCpu, fontGpu)) {
        GLOGW("overlay[dx12]: ImGui_ImplDX12_Init failed"); Teardown(); return false;
    }
    g.dx12Init = true;

    if (!InstallWndProc(g.hwnd)) { Teardown(); return false; }

    {
        Lock lk;
        g.ready.store(true);   // LoadPngTexture needs a ready overlay
    }
    // The marks, the lockup and the loader's pictures: the same list as the D3D11 backend (overlay.cpp). An image
    // that fails is logged and left null; the UI draws its designed fallback for it.
    // 0.4.24 switch: dx12_textures=0 (HKCU\Software\GAGE-MP) = no uploads, the pre-0.4.24 fallbacks (live-unverified on Enhanced).
    detail::UiTextures tx;
    const bool texOn = gage::reg::Get(L"dx12_textures") != L"0";
    const int loaded = texOn ? detail::LoadUiTextures(tx) : 0;
    if (!texOn) GLOGI("overlay[dx12]: dx12_textures=0, no image uploads (typeset fallbacks)");
    detail::UiSetTextures(tx);
    GLOGI("overlay[dx12]: ready (sc %p dev %p queue %p hwnd %p %ux%u scale %.2f fmt %u, %d/%d brand textures, brand art %s)",
          (void*)sc, (void*)dev, (void*)g.queue, (void*)g.hwnd, g.initW, g.initH, g.uiScale, (unsigned)g.rtvFormat, loaded,
          detail::kUseBrandArt ? 4 + detail::kLoaderScenes : detail::kLoaderScenes,
          detail::kUseBrandArt ? "on" : "off (typeset fallbacks)");
    return true;
}

bool SameTarget(IDXGISwapChain* sc)
{
    // Identity is enough on DX12: the device cannot change without a new swap chain, and
    // asking a DX12 swap chain for its device means fetching a back buffer every frame.
    return g.sc == sc;
}

bool Acquire(IDXGISwapChain* sc)
{
    if (g.ready.load()) {
        if (SameTarget(sc))
            return true;
        GLOGI("overlay[dx12]: swap chain changed (%p -> %p), re-initialising", (void*)g.sc, (void*)sc);
        Teardown();
    }
    // Deferred init: the ExecuteCommandLists hook must be installed and the game's DIRECT
    // queue captured before we can submit. Both take a frame or two to come true.
    if (!g.hookInstalled) {
        g.hookInstalled = InstallExecHook();
        if (g.hookInstalled)
            GLOGI("overlay[dx12]: ExecuteCommandLists hook installed; waiting for the game's command queue");
        return false;      // let the game run a frame so the hook can capture its queue
    }
    if (!g_capturedQueue)
        return false;      // the game has not submitted on a DIRECT queue yet; try next frame

    const ULONGLONG now = GetTickCount64();
    if (g.failSc == sc) {
        if (g.failCount >= kMaxInitTries || now < g.nextTry)
            return false;
    } else {
        g.failSc = sc;
        g.failCount = 0;
        g.nextTry = 0;
    }
    if (InitNew(sc)) {
        g.failSc = nullptr;
        g.failCount = 0;
        return true;
    }
    ++g.failCount;
    g.nextTry = now + kInitRetryMs;
    if (g.failCount >= kMaxInitTries)
        GLOGW("overlay[dx12]: giving up on swap chain %p after %d failed attempts", (void*)sc, g.failCount);
    return false;
}

// Rebuild from scratch when the swap chain was resized or its format/buffer count changed.
// Returns true when a rebuild was started (caller should skip this frame).
bool RebuildIfResized(IDXGISwapChain* sc)
{
    DXGI_SWAP_CHAIN_DESC d = {};
    if (FAILED(sc->GetDesc(&d)))
        return false;
    UINT newBc = d.BufferCount;
    if (newBc < 2 || newBc > kMaxBackBuffers) newBc = g.bufferCount;   // ignore an implausible count
    const bool changed = d.BufferDesc.Width != g.initW ||
                         d.BufferDesc.Height != g.initH ||
                         RtvFormatFor(d.BufferDesc.Format) != g.rtvFormat ||
                         newBc != g.bufferCount;
    if (!changed)
        return false;
    GLOGI("overlay[dx12]: swap chain changed (%ux%u -> %ux%u), rebuilding", g.initW, g.initH,
          d.BufferDesc.Width, d.BufferDesc.Height);
    Teardown();     // Acquire() re-inits with the new description on the next frame
    return true;
}

void RenderFrame(IDXGISwapChain* scBase)
{
    if (!g.ready.load() || !SameTarget(scBase)) {
        if (g.disabled.load() || !Acquire(scBase))
            return;
    }
    if (RebuildIfResized(scBase))
        return;

    // GetCurrentBackBufferIndex lives on IDXGISwapChain3.
    IDXGISwapChain3* sc = nullptr;
    if (FAILED(scBase->QueryInterface(IID_IDXGISwapChain3, (void**)&sc)) || !sc) {
        if (g.getBufferFailLogged++ < 3)
            GLOGW("overlay[dx12]: the swap chain is not an IDXGISwapChain3");
        return;
    }

    // ImGui frame first (needs the input lock, as in overlay.cpp).
    ImGui::SetCurrentContext(g.imctx);
    {
        Lock lk;     // WndProc input handling shares ImGui's input queue with NewFrame
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const ImGuiIO& io = ImGui::GetIO();
        g.wantMouse.store(io.WantCaptureMouse);
        g.wantKeyboard.store(io.WantCaptureKeyboard);
    }
    ++g.frameNo;
    if (const int probe = detail::TakeProbeRequest())
        GLOGI("overlay: radar probe %d skipped on D3D12 (the probe reads a D3D11 back buffer only)", probe);
    {
        // 0.5.8 inv_ped_mode: the CHARACTER window probe reads a D3D11 back buffer only; answer Unavailable (the slot stays unverified)
        uint32_t seq = 0;
        int slot = 0;
        if (detail::TakePedProbeRequest(&seq, &slot)) {
            PedProbe p;
            p.seq = seq;
            p.verdict = (int)gage::sync::pedpreview::Verdict::Unavailable;
            detail::PutPedProbe(p);
        }
    }
    detail::UiFrame();
    {
        // An open panel owns the keyboard and mouse even where ImGui has no widget under the pointer.
        const ImGuiIO& io = ImGui::GetIO();
        const bool interactive = detail::UiInteractive();
        g.wantMouse.store(io.WantCaptureMouse || interactive);
        g.wantKeyboard.store(io.WantCaptureKeyboard || interactive);
    }
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();

    const UINT idx = sc->GetCurrentBackBufferIndex();
    if (idx >= g.bufferCount) {     // a buffer-count change not yet rebuilt: skip this frame
        sc->Release();
        return;
    }
    FrameCtx& fc = g.frame[idx];
    WaitFence(fc.fenceValue);       // this index's previous submission is done: its allocator is free

    if (FAILED(fc.alloc->Reset()) || FAILED(g.cmd->Reset(fc.alloc, nullptr))) {
        if (g.getBufferFailLogged++ < 3)
            GLOGW("overlay[dx12]: command allocator/list reset failed");
        sc->Release();
        return;
    }

    // Fresh back buffer each frame: we must hold no reference when the game calls ResizeBuffers.
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(idx, IID_ID3D12Resource, (void**)&bb)) || !bb) {
        if (g.getBufferFailLogged++ < 3)
            GLOGW("overlay[dx12]: GetBuffer(%u) failed", idx);
        g.cmd->Close();
        sc->Release();
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)idx * g.rtvStep;
    D3D12_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = g.rtvFormat;
    rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g.dev->CreateRenderTargetView(bb, &rd, rtv);

    // Draw ON TOP of the game's finished frame: the back buffer is in PRESENT here.
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g.cmd->ResourceBarrier(1, &b);

    g.cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);   // no clear: the game's frame stays
    ID3D12DescriptorHeap* heaps[] = { g.srvHeap };
    g.cmd->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(dd, g.cmd);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    g.cmd->ResourceBarrier(1, &b);
    g.cmd->Close();

    ID3D12CommandList* lists[] = { g.cmd };
    g.queue->ExecuteCommandLists(1, lists);               // on the captured queue, before the game's Present
    fc.fenceValue = ++g.fenceCounter;
    g.queue->Signal(g.fence, fc.fenceValue);

    bb->Release();      // hold no reference past this frame (ResizeBuffers happens between frames)
    sc->Release();
}

// ---------------------------------------------------------------------------
// Textures: WIC decode, upload through the captured queue, one SRV per texture
// ---------------------------------------------------------------------------
// A straight-alpha texture is filtered bilinearly, so the RGB stored under fully transparent texels leaks
// into the anti-aliased edge (exporters write black there: a dark fringe around a white mark). Give every
// transparent texel the average colour of its already-coloured neighbours, `passes` rings deep. Alpha is
// untouched. Allocation failure only skips the fix. Local copy of overlay.cpp's, so the D3D11 file is untouched.
void BleedColorIntoTransparent(unsigned char* px, UINT w, UINT h, int passes)
{
    const size_t n = (size_t)w * h;
    unsigned char* known = new (std::nothrow) unsigned char[n];
    unsigned char* grown = new (std::nothrow) unsigned char[n];
    if (!known || !grown) {
        delete[] known;
        delete[] grown;
        return;
    }
    for (size_t i = 0; i < n; ++i)
        known[i] = px[i * 4 + 3] != 0;
    for (int pass = 0; pass < passes; ++pass) {
        std::memcpy(grown, known, n);
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                const size_t i = (size_t)y * w + x;
                if (known[i])
                    continue;
                unsigned sr = 0, sg = 0, sb = 0, cnt = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = (int)x + dx, ny = (int)y + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= (int)w || ny >= (int)h)
                            continue;
                        const size_t j = (size_t)ny * w + (size_t)nx;
                        if (!known[j])
                            continue;
                        sr += px[j * 4 + 0]; sg += px[j * 4 + 1]; sb += px[j * 4 + 2];
                        ++cnt;
                    }
                }
                if (cnt) {
                    px[i * 4 + 0] = (unsigned char)(sr / cnt);
                    px[i * 4 + 1] = (unsigned char)(sg / cnt);
                    px[i * 4 + 2] = (unsigned char)(sb / cnt);
                    grown[i] = 1;
                }
            }
        }
        unsigned char* t = known; known = grown; grown = t;
    }
    delete[] known;
    delete[] grown;
}

// Decode a PNG/JPEG held in memory to straight-alpha 32bppRGBA. On success *outPx is a new[] block of
// w * h * 4 bytes the caller frees; on failure *outPx is null and *what names the step that failed.
bool DecodeToRgba(const unsigned char* bytes, size_t n, unsigned char** outPx, UINT* outW, UINT* outH,
                  const char** what, HRESULT* outHr)
{
    // WIC needs COM on this thread. Balance only what we initialised; RPC_E_CHANGED_MODE
    // means the thread already has an STA, which is just as good.
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInit = SUCCEEDED(hrCo);

    IWICImagingFactory*     factory = nullptr;
    IWICStream*             stream = nullptr;
    IWICBitmapDecoder*      decoder = nullptr;
    IWICBitmapFrameDecode*  frame = nullptr;
    IWICFormatConverter*    conv = nullptr;
    unsigned char*          px = nullptr;
    UINT w = 0, h = 0;
    HRESULT hr = S_OK;

    do {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                              (void**)&factory);
        if (FAILED(hr)) { *what = "CoCreateInstance(WICImagingFactory)"; break; }
        hr = factory->CreateStream(&stream);
        if (FAILED(hr)) { *what = "CreateStream"; break; }
        hr = stream->InitializeFromMemory(const_cast<BYTE*>(bytes), (DWORD)n);
        if (FAILED(hr)) { *what = "InitializeFromMemory"; break; }
        hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
        if (FAILED(hr)) { *what = "CreateDecoderFromStream (not a decodable image)"; break; }
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) { *what = "GetFrame"; break; }
        hr = frame->GetSize(&w, &h);
        if (FAILED(hr)) { *what = "GetSize"; break; }
        if (w == 0 || h == 0 || w > 8192 || h > 8192) { *what = "image size out of range (1..8192)"; hr = E_INVALIDARG; break; }
        hr = factory->CreateFormatConverter(&conv);
        if (FAILED(hr)) { *what = "CreateFormatConverter"; break; }
        // Straight (non-premultiplied) alpha: ImGui's blend is SRC_ALPHA / INV_SRC_ALPHA.
        hr = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) { *what = "format conversion to 32bppRGBA"; break; }
        const size_t bytesTotal = (size_t)w * h * 4;
        px = new (std::nothrow) unsigned char[bytesTotal];
        if (!px) { *what = "out of memory"; hr = E_OUTOFMEMORY; break; }
        hr = conv->CopyPixels(nullptr, w * 4, (UINT)bytesTotal, px);
        if (FAILED(hr)) { *what = "CopyPixels"; break; }
    } while (false);

    SafeRelease(conv);
    SafeRelease(frame);
    SafeRelease(decoder);
    SafeRelease(stream);
    SafeRelease(factory);
    if (coInit)
        CoUninitialize();

    *outHr = hr;
    if (FAILED(hr)) {
        delete[] px;
        *outPx = nullptr;
        return false;
    }
    *outPx = px;
    *outW = w;
    *outH = h;
    return true;
}

// Create a default-heap RGBA8 texture and fill it from px (w * 4 bytes per row) through an upload buffer on a
// one-shot list submitted to the captured queue. Returns the texture in PIXEL_SHADER_RESOURCE state only after the
// fence has passed, so the upload buffer is never freed while the GPU can still read it. If the wait times out the
// staging objects and the texture are deliberately leaked (the GPU may still be using them) and the call fails.
bool UploadRgba(const unsigned char* px, UINT w, UINT h, ID3D12Resource** outTex, const char** what, HRESULT* outHr)
{
    ID3D12Resource*         tex = nullptr;
    ID3D12Resource*         upload = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    HRESULT hr = S_OK;
    bool leak = false;      // the GPU may still read these: do not release them
    bool ok = false;

    do {
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_ID3D12Resource, (void**)&tex);
        if (FAILED(hr)) { *what = "CreateCommittedResource(texture)"; break; }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        g.dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        if (total == 0 || rows != h || rowBytes < (UINT64)w * 4) { *what = "GetCopyableFootprints"; hr = E_FAIL; break; }

        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES up = {};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        hr = g.dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_ID3D12Resource, (void**)&upload);
        if (FAILED(hr)) { *what = "CreateCommittedResource(upload buffer)"; break; }
        unsigned char* mapped = nullptr;
        const D3D12_RANGE noRead = {0, 0};
        hr = upload->Map(0, &noRead, (void**)&mapped);
        if (FAILED(hr) || !mapped) { *what = "Map(upload buffer)"; if (SUCCEEDED(hr)) hr = E_FAIL; break; }
        for (UINT y = 0; y < h; ++y)
            std::memcpy(mapped + fp.Offset + (size_t)y * fp.Footprint.RowPitch, px + (size_t)y * w * 4, (size_t)w * 4);
        upload->Unmap(0, nullptr);

        hr = g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_ID3D12CommandAllocator, (void**)&alloc);
        if (FAILED(hr)) { *what = "CreateCommandAllocator(upload)"; break; }
        hr = g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_ID3D12GraphicsCommandList,
                                      (void**)&list);
        if (FAILED(hr)) { *what = "CreateCommandList(upload)"; break; }

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        hr = list->Close();
        if (FAILED(hr)) { *what = "Close(upload list)"; break; }

        ID3D12CommandList* lists[] = { list };
        g.queue->ExecuteCommandLists(1, lists);
        const UINT64 v = ++g.fenceCounter;
        hr = g.queue->Signal(g.fence, v);
        if (FAILED(hr)) { *what = "Signal(upload fence)"; leak = true; break; }
        if (!WaitFenceFor(v, 3000)) {
            *what = "the GPU did not finish the upload within 3 s";
            hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            leak = true;
            break;
        }
        ok = true;
    } while (false);

    if (leak) {
        // Submitted but not known to be finished: freeing now could free memory the GPU is reading.
        GLOGW("overlay[dx12]: LoadPngTexture: leaking one upload's resources (the GPU may still be using them)");
    } else {
        SafeRelease(list);
        SafeRelease(alloc);
        SafeRelease(upload);
    }
    if (!ok) {
        if (!leak)
            SafeRelease(tex);
        *outTex = nullptr;
        *outHr = hr;
        return false;
    }
    *outTex = tex;
    *outHr = S_OK;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Backend entry points (called from overlay.cpp's public API dispatch)
// ---------------------------------------------------------------------------
bool Init(void* swapChain)
{
    IDXGISwapChain* sc = static_cast<IDXGISwapChain*>(swapChain);
    if (!sc)
        return false;
    g.disabled.store(false);
    return Acquire(sc);
}

void Render(void* swapChain)
{
    IDXGISwapChain* sc = static_cast<IDXGISwapChain*>(swapChain);
    if (!sc)
        return;
    RenderGuard guard;
    RenderFrame(sc);
}

void Shutdown()
{
    g.disabled.store(true);
    g.ready.store(false);
    for (int i = 0; i < 500 && g.inRender.load() > 0; ++i)
        Sleep(1);
    if (g.inRender.load() > 0)
        GLOGW("overlay[dx12]: Shutdown: a Render() is still in flight after 500 ms, tearing down anyway");
    const bool had = g.sc != nullptr;
    Teardown();
    if (had)
        GLOGI("overlay[dx12]: shut down");
}

bool Ready()
{
    return g.ready.load();
}

bool WantsInput()
{
    return g.ready.load() && (g.wantMouse.load() || g.wantKeyboard.load());
}

bool Interactive()
{
    return g.ready.load() && detail::UiInteractive();
}

// Render thread only (see overlay.h). The returned handle is the texture's GPU descriptor handle in g.srvHeap,
// which is exactly what imgui_impl_dx12 reads back from ImDrawCmd::GetTexID(). It stays valid until FreeTexture
// or Shutdown / a rebuild (Teardown); the UI forgets it in the same step (UiReset).
bool LoadPngTexture(const unsigned char* bytes, size_t n, void** outTexture, int* outW, int* outH)
{
    if (outTexture) *outTexture = nullptr;
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!outTexture || !bytes || n == 0 || n > 0x7fffffffu) {
        GLOGW("overlay[dx12]: LoadPngTexture: bad arguments (bytes %p, size %zu)", (const void*)bytes, n);
        return false;
    }
    if (!g.ready.load() || !g.dev || !g.queue || !g.srvHeap || !g.fence || !g.fenceEvent || g.srvStep == 0) {
        GLOGW("overlay[dx12]: LoadPngTexture: the overlay is not initialised");
        return false;
    }
    UINT slot = 0;
    for (UINT i = 1; i < kSrvSlots; ++i) {
        if (!g.tex[i].res) { slot = i; break; }
    }
    if (slot == 0) {
        GLOGW("overlay[dx12]: LoadPngTexture: all %u SRV slots are in use", kSrvSlots - 1);
        return false;
    }

    unsigned char* px = nullptr;
    UINT w = 0, h = 0;
    const char* what = "unknown step";
    HRESULT hr = S_OK;
    if (!DecodeToRgba(bytes, n, &px, &w, &h, &what, &hr)) {
        GLOGW("overlay[dx12]: LoadPngTexture: %s failed (0x%08lx)", what, (unsigned long)hr);
        return false;
    }
    BleedColorIntoTransparent(px, w, h, 2);

    ID3D12Resource* tex = nullptr;
    const bool uploaded = UploadRgba(px, w, h, &tex, &what, &hr);
    delete[] px;
    if (!uploaded) {
        GLOGW("overlay[dx12]: LoadPngTexture: %s failed (0x%08lx)", what, (unsigned long)hr);
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MostDetailedMip = 0;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)slot * g.srvStep;
    gpu.ptr += (UINT64)slot * g.srvStep;
    g.dev->CreateShaderResourceView(tex, &sv, cpu);

    g.tex[slot].res = tex;       // the table owns the reference
    g.tex[slot].gpuPtr = gpu.ptr;
    *outTexture = reinterpret_cast<void*>(static_cast<uintptr_t>(gpu.ptr));
    if (outW) *outW = (int)w;
    if (outH) *outH = (int)h;
    return true;
}

void FreeTexture(void* texture)
{
    if (!texture)
        return;
    const UINT64 key = static_cast<UINT64>(reinterpret_cast<uintptr_t>(texture));
    for (UINT i = 1; i < kSrvSlots; ++i) {
        if (g.tex[i].res && g.tex[i].gpuPtr == key) {
            WaitFence(g.fenceCounter);     // a frame in flight may still sample it, and its descriptor slot is about to be reusable
            SafeRelease(g.tex[i].res);
            g.tex[i].gpuPtr = 0;
            return;
        }
    }
    GLOGW("overlay[dx12]: FreeTexture: %p is not a texture of this overlay", texture);
}

} // namespace gage::game::Overlay::dx12
