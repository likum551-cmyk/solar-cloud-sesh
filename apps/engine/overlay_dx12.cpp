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
// v1 draws the full HUD (vitals, speedometer, chat, loader layout) with ImGui's own font;
// custom brand textures (the corner mark, the loader's real GTA V stills) are not yet
// uploaded on DX12, so the UI draws its designed fallbacks — see LoadPngTexture below.
#include "overlay.h"
#include "overlay_internal.h"
#include "common/logger.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
#include <cstdint>

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
    ID3D12DescriptorHeap*      srvHeap = nullptr;   // shader-visible: ImGui font at slot 0
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

    detail::UiReset();
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

    // SRV heap: shader-visible, ImGui's font SRV at slot 0 (slots 1.. reserved for future brand textures).
    D3D12_DESCRIPTOR_HEAP_DESC sh = {};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 64;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&sh, IID_ID3D12DescriptorHeap, (void**)&g.srvHeap))) {
        GLOGW("overlay[dx12]: CreateDescriptorHeap(SRV) failed"); Teardown(); return false;
    }

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
        g.ready.store(true);
    }
    // v1: no custom brand textures on DX12 yet; the UI draws its designed fallbacks.
    detail::UiTextures tx;      // all null
    detail::UiSetTextures(tx);
    GLOGI("overlay[dx12]: ready (sc %p dev %p queue %p hwnd %p %ux%u scale %.2f fmt %u) — brand textures: fallback (v1)",
          (void*)sc, (void*)dev, (void*)g.queue, (void*)g.hwnd, g.initW, g.initH, g.uiScale, (unsigned)g.rtvFormat);
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

bool LoadPngTexture(const unsigned char* /*bytes*/, size_t /*n*/, void** outTexture, int* outW, int* outH)
{
    // v1: brand-texture upload is not yet wired on DX12 (needs a WIC decode + a DX12 default-heap
    // texture uploaded through the captured queue, with its SRV in g.srvHeap slots 1..). Until then
    // the UI gets null handles and draws its designed fallbacks. Returning false is the contract.
    if (outTexture) *outTexture = nullptr;
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
        GLOGW("overlay[dx12]: LoadPngTexture is not implemented yet (v1); the UI uses its designed fallbacks");
    return false;
}

void FreeTexture(void* /*texture*/)
{
    // No DX12 user textures exist yet (see LoadPngTexture); nothing to free.
}

} // namespace gage::game::Overlay::dx12
