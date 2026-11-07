// radar_mask.cpp: see radar_mask.h. Built with -fno-exceptions like the rest of the engine.
#include "radar_mask.h"

#include "brand_apparel.h"
#include "common/logger.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace gage::game::radarmask {

using namespace gage::sync::ringround;
namespace swap = gage::game::brand::swap;

namespace {

std::atomic<bool> g_enabled{ false };
std::atomic<bool> g_active{ false };          // SetActive's last word (a replacement registered later starts in this state)
std::atomic<int>  g_phase{ (int)Phase::Idle };
std::mutex        g_mu;                    // guards g_status, g_req*
Status            g_status;
swap::ProbeSize   g_reqSizes[2] = {};
std::vector<int>  g_reqWindows;
uint32_t          g_gen = 0;               // render thread: the swap generation our handles belong to

struct Readback {
    int kind = -1;
    ID3D11ShaderResourceView* srv = nullptr;   // the stock view (AddRef'd)
    ID3D11Texture2D* tex = nullptr;            // its texture (AddRef'd)
    ID3D11Texture2D* staging = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    TexFmt fmt = TexFmt::Unknown;
    ULONGLONG started = 0;
};
std::vector<Readback> g_jobs;              // render thread only

void ReleaseJob(Readback& j) {
    if (j.staging) j.staging->Release();
    if (j.tex) j.tex->Release();
    if (j.srv) j.srv->Release();
    j = Readback{};
}

TexFmt FromDxgi(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: return TexFmt::BC1;
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: return TexFmt::BC3;
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: return TexFmt::BC4;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return TexFmt::RGBA8;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return TexFmt::BGRA8;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: return TexFmt::R8;
    case DXGI_FORMAT_A8_UNORM: return TexFmt::A8;
    default: return TexFmt::Unknown;
    }
}

void SetKindWhy(int kind, const std::string& why) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status.k[kind].why = why;
}

// Pick each kind's view: bound in ALL of its windows and in NONE of the others (another kind's or an empty one).
void Evaluate(std::vector<swap::ProbeCand>& cands, const std::vector<int>& windows, uint32_t overflow) {
    std::lock_guard<std::mutex> lk(g_mu);
    bool anyFound = false;
    for (int k = 0; k < 2; ++k) {
        KindStatus& ks = g_status.k[k];
        if (!ks.probed) continue;
        uint32_t mine = 0, others = 0;
        for (size_t i = 0; i < windows.size() && i < 32; ++i) (windows[i] == k ? mine : others) |= 1u << i;
        int sized = 0, winners = 0, everywhere = 0;
        swap::ProbeCand* win = nullptr;
        uint32_t bestHits = 0;
        for (swap::ProbeCand& c : cands) {
            if (c.w != ks.w || c.h != ks.h) continue;
            ++sized;
            const uint32_t hits = (uint32_t)__builtin_popcount(c.seen & mine);
            if (hits > bestHits) bestHits = hits;
            if ((c.seen & mine) == mine && (c.seen & others) == 0) { ++winners; win = &c; }
            if ((c.seen & (mine | others)) == (mine | others)) ++everywhere;
        }
        char b[420];
        if (!mine) {
            // no window of its own: nothing can prove a view is this mask (every candidate would "match" an empty window set)
            ks.why = "no window was spent on it";
            continue;
        }
        if (winners == 1) std::snprintf(b, sizeof(b), "found: the one %ux%u view bound in all %d of its windows and in none of the other %d (%d views of that size seen)",
                                             ks.w, ks.h, __builtin_popcount(mine), __builtin_popcount(others), sized);
        else if (winners > 1) std::snprintf(b, sizeof(b), "%d views of %ux%u matched its windows; not guessing which is the mask", winners, ks.w, ks.h);
        else std::snprintf(b, sizeof(b), "no %ux%u view was bound in exactly its windows (%d of that size seen, the best in %u of %d, %d bound in EVERY window: "
                           "does the game bind it with the radar hidden, or did DRAW_SPRITE draw nothing?)%s",
                           ks.w, ks.h, sized, bestHits, __builtin_popcount(mine), everywhere, overflow ? " (candidate table overflowed)" : "");
        ks.why = b;
        if (winners != 1) continue;
        ks.found = true;
        anyFound = true;
        // the readback: a staging copy now, mapped on a later frame
        Readback j;
        j.kind = k;
        j.srv = win->srv;
        win->srv = nullptr;                                     // the reference moves into the job
        ID3D11Resource* res = nullptr;
        j.srv->GetResource(&res);
        if (res) {
            res->QueryInterface(IID_ID3D11Texture2D, (void**)&j.tex);
            res->Release();
        }
        ID3D11Device* dev = swap::Device();
        ID3D11DeviceContext* ctx = swap::Context();
        if (!j.tex || !dev || !ctx) { ks.why += "; its texture could not be read"; ReleaseJob(j); continue; }
        j.tex->GetDesc(&j.desc);
        j.fmt = FromDxgi(j.desc.Format);
        ks.mips = j.desc.MipLevels;
        ks.fmt = j.fmt;
        if (j.fmt == TexFmt::Unknown) {
            std::snprintf(b, sizeof(b), "; its format (DXGI %d) is not one we can rebuild", (int)j.desc.Format);
            ks.why += b;
            ReleaseJob(j);
            continue;
        }
        D3D11_TEXTURE2D_DESC sd = j.desc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &j.staging)) || !j.staging) { ks.why += "; no staging texture"; ReleaseJob(j); continue; }
        ctx->CopyResource(j.staging, j.tex);
        j.started = GetTickCount64();
        g_jobs.push_back(j);
    }
    for (swap::ProbeCand& c : cands)
        if (c.srv) { c.srv->Release(); c.srv = nullptr; }
    g_status.why = anyFound ? "probe done" : "probe found no mask";
}

// 0 = still waiting for the GPU, 1 = done (job released either way)
int FinishReadback(Readback& j) {
    ID3D11Device* dev = swap::Device();
    ID3D11DeviceContext* ctx = swap::Context();
    if (!dev || !ctx) { ReleaseJob(j); return 1; }
    const int w = (int)j.desc.Width, h = (int)j.desc.Height, levels = (int)j.desc.MipLevels;
    std::vector<std::vector<uint8_t>> stock((size_t)levels);
    for (int l = 0; l < levels; ++l) {
        D3D11_MAPPED_SUBRESOURCE m = {};
        const HRESULT hr = ctx->Map(j.staging, (UINT)l, D3D11_MAP_READ, l == 0 ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (GetTickCount64() - j.started > 4000) { SetKindWhy(j.kind, "the readback stayed busy for 4 s"); ReleaseJob(j); return 1; }
            return 0;
        }
        if (FAILED(hr) || !m.pData) { SetKindWhy(j.kind, "mapping the staging copy failed"); ReleaseJob(j); return 1; }
        const int lw = LevelDim(w, l), lh = LevelDim(h, l);
        const size_t rb = RowBytes(j.fmt, lw);
        const int rows = Rows(j.fmt, lh);
        stock[(size_t)l].resize(rb * (size_t)rows);
        for (int r = 0; r < rows; ++r) std::memcpy(stock[(size_t)l].data() + (size_t)r * rb, (const uint8_t*)m.pData + (size_t)r * m.RowPitch, rb);
        ctx->Unmap(j.staging, (UINT)l);
    }
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto& lv : stock) hash = Fnv64(lv.data(), lv.size(), hash);
    std::vector<Rgba> px;
    StockMask sm;
    if (!DecodeLevel(j.fmt, stock[0].data(), stock[0].size(), w, h, px)) sm.why = "level 0 could not be decoded";
    else sm = AnalyzeStockMask(j.fmt, px, w, h);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_status.k[j.kind].stockHash = hash;
    }
    GLOGI("radar mask (0.5.5): %s read back: %dx%d %s, %d mip(s), stock FNV-1a 64 %016llx; %s: %s", kTexName[j.kind], w, h, TexFmtName(j.fmt), levels,
          (unsigned long long)hash, sm.ok ? "looks like a mask" : "NOT a mask, left alone", sm.why.c_str());
    if (!sm.ok) { SetKindWhy(j.kind, "the found view does not look like a mask: " + sm.why); ReleaseJob(j); return 1; }

    // our circle, the same chain
    std::vector<std::vector<Rgba>> mips;
    BuildRoundMask(w, h, levels, sm.inside, MaskOutside(sm), mips);
    std::vector<std::vector<uint8_t>> enc((size_t)levels);
    std::vector<D3D11_SUBRESOURCE_DATA> sd((size_t)levels);
    for (int l = 0; l < levels; ++l) {
        const int lw = LevelDim(w, l), lh = LevelDim(h, l);
        if (!EncodeLevel(j.fmt, mips[(size_t)l], lw, lh, enc[(size_t)l])) { SetKindWhy(j.kind, "encoding our mask failed"); ReleaseJob(j); return 1; }
        sd[(size_t)l].pSysMem = enc[(size_t)l].data();
        sd[(size_t)l].SysMemPitch = (UINT)RowBytes(j.fmt, lw);
        sd[(size_t)l].SysMemSlicePitch = 0;
    }
    D3D11_TEXTURE2D_DESC rd = j.desc;
    rd.Usage = D3D11_USAGE_IMMUTABLE;
    rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    rd.CPUAccessFlags = 0;
    rd.MiscFlags = 0;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&rd, sd.data(), &tex);
    if (FAILED(hr) || !tex) {
        char b[120];
        std::snprintf(b, sizeof(b), "CreateTexture2D for our mask failed (0x%08lx)", (unsigned long)hr);
        SetKindWhy(j.kind, b);
        ReleaseJob(j);
        return 1;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
    j.srv->GetDesc(&vd);
    ID3D11ShaderResourceView* ours = nullptr;
    hr = dev->CreateShaderResourceView(tex, &vd, &ours);
    tex->Release();                                             // the view keeps the texture alive
    if (FAILED(hr) || !ours) { SetKindWhy(j.kind, "CreateShaderResourceView for our mask failed"); ReleaseJob(j); return 1; }
    const int handle = swap::Replace(j.srv, ours, g_active.load());
    ours->Release();                                            // swap holds its own reference
    {
        std::lock_guard<std::mutex> lk(g_mu);
        KindStatus& ks = g_status.k[j.kind];
        ks.handle = handle;
        ks.replaced = handle >= 0;
        char b[400];
        std::snprintf(b, sizeof(b), "%s: %dx%d %s %d mip(s) (stock centre value %d, corners %d) -> our circle (inside = the stock centre texel, outside black, "
                      "a %.0f %% ramp inside the edge)%s",
                      handle >= 0 ? "REPLACED" : "the swap refused it", w, h, TexFmtName(j.fmt), levels, sm.insideValue, sm.outsideValue, (double)(kFeather * 100.0f),
                      handle >= 0 ? "" : " (already replaced, or the table is full)");
        ks.why = b;
        g_status.generation = swap::Generation();
    }
    GLOGI("radar mask (0.5.5): %s %s", kTexName[j.kind],
          handle >= 0 ? "replaced by our circle (registered; the bind hook hands it out while the engine's gate has the map grown)" : "NOT replaced (swap refused)");
    ReleaseJob(j);
    return 1;
}

void RenderTickImpl() {
    // the generation first: after a teardown (hooks gone) the jobs' references to the old device's views are dropped at once
    const uint32_t gen = swap::Generation();
    if (gen != g_gen) {
        // the hooks were (re)installed: every handle from before is dead
        for (Readback& j : g_jobs) ReleaseJob(j);
        g_jobs.clear();
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_gen != 0) {
            for (KindStatus& ks : g_status.k) { ks.replaced = false; ks.handle = -1; ks.found = false; }
            g_status.why = "the D3D11 device changed: probe again";
            g_phase.store((int)Phase::Idle);
        }
        g_gen = gen;
        g_status.generation = gen;
    }
    if (!swap::Installed()) return;
    const Phase p = (Phase)g_phase.load();
    if (p == Phase::ArmRequested) {
        swap::ProbeSize sizes[2];
        int n = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (int k = 0; k < 2; ++k)
                if (g_reqSizes[k].w && g_reqSizes[k].h) sizes[n++] = g_reqSizes[k];
        }
        const bool ok = n > 0 && swap::ProbeArm(sizes, n);
        g_phase.store((int)(ok ? Phase::Armed : Phase::Failed));
        if (!ok) { std::lock_guard<std::mutex> lk(g_mu); g_status.why = "the probe recorder could not be armed"; }
    } else if (p == Phase::FinishRequested) {
        uint32_t overflow = 0;
        std::vector<swap::ProbeCand> cands = swap::ProbeTake(&overflow);
        std::vector<int> windows;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            windows = g_reqWindows;
        }
        Evaluate(cands, windows, overflow);
        g_phase.store((int)Phase::Done);
    }
    for (size_t i = 0; i < g_jobs.size();) {
        if (FinishReadback(g_jobs[i]) == 0) { ++i; continue; }
        g_jobs.erase(g_jobs.begin() + (std::ptrdiff_t)i);
    }
}

}  // namespace

const char* PhaseName(Phase p) {
    switch (p) {
    case Phase::Idle: return "idle";
    case Phase::ArmRequested: return "arming";
    case Phase::Armed: return "probing";
    case Phase::FinishRequested: return "evaluating";
    case Phase::Done: return "done";
    case Phase::Failed: return "failed";
    }
    return "?";
}

void Enable(bool on) {
    g_enabled.store(on);
    if (on) swap::Want();
}

int SetActive(bool on) {
    g_active.store(on);
    int changed = 0;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_status.generation != swap::Generation()) return 0;
    for (const KindStatus& ks : g_status.k)
        if (ks.replaced && ks.handle >= 0 && swap::SetActive(ks.handle, g_status.generation, on)) ++changed;
    return changed;
}

bool RequestArm(uint32_t wSmall, uint32_t hSmall, uint32_t wBig, uint32_t hBig) {
    const Phase p = (Phase)g_phase.load();
    if (p != Phase::Idle && p != Phase::Done && p != Phase::Failed) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    g_reqSizes[0] = { wSmall, hSmall };
    g_reqSizes[1] = { wBig, hBig };
    for (int k = 0; k < 2; ++k) {
        KindStatus& ks = g_status.k[k];
        if (ks.replaced && swap::Generation() == g_status.generation) continue;   // a live replacement stays
        ks = KindStatus{};
        ks.probed = g_reqSizes[k].w && g_reqSizes[k].h;
        ks.w = g_reqSizes[k].w;
        ks.h = g_reqSizes[k].h;
    }
    g_status.why = "probe requested";
    g_phase.store((int)Phase::ArmRequested);
    return true;
}

bool RequestFinish(const int* kindOfWindow, int n) {
    if ((Phase)g_phase.load() != Phase::Armed || !kindOfWindow || n <= 0 || n > 32) return false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_reqWindows.assign(kindOfWindow, kindOfWindow + n);
    }
    swap::ProbeSetWindow(0);
    g_phase.store((int)Phase::FinishRequested);
    return true;
}

void Reset() {
    swap::ProbeSetWindow(0);
    g_phase.store((int)Phase::Idle);
}

Status GetStatus() {
    std::lock_guard<std::mutex> lk(g_mu);
    Status s = g_status;
    s.phase = (Phase)g_phase.load();
    s.hooks = swap::Installed();
    s.notD3D11 = swap::NotD3D11();
    return s;
}

bool Live(int kind) {
    if (kind < 0 || kind > 1) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    // the swap layer's own word: the stock view's slot really says "replaced by this handle" in the current install
    return g_status.k[kind].replaced && g_status.k[kind].handle >= 0 && swap::Holds(g_status.k[kind].handle, g_status.generation);
}

void Poll(void* swapChain) {
    if (!swapChain || !g_enabled.load()) return;
    RenderTickImpl();
}

namespace testing {
void RenderTick() { RenderTickImpl(); }
}

}  // namespace gage::game::radarmask
