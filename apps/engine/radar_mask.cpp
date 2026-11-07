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
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace gage::game::radarmask {

using namespace gage::sync::ringround;
namespace swap = gage::game::brand::swap;

namespace {

// The stock masks' FNV-1a 64 (over every mip, as logged by "radar mask census (0.5.6): ... stock FNV-1a 64 ..."), once a live census has read
// them back: a view carrying one of these AND passing the basic mask test (StockMask::basic: grey, bright centre, dark edges) is the mask even
// when the census is incomplete or ambiguous (PickMask). Only hashes go here, never pixels.
// 0.5.7, from GI's 0.5.6 census (gage.log 2026-11-07 08:42-08:43, Legacy, 3 runs agreeing):
//   radarmasksm: candidate 9 of 12, 512x256 BC1, 1 mip (the only 1-mip view of that size), centre 186, corners 0, edges 0, mirror error 32.3:
//                refused by the symmetry rule, which is why all three runs kept the 0.5.4 square;
//   radarmasklg: candidate 137 of 161, 512x512 BC1, 1 mip, centre 181, corners 0, edges 0, mirror error 0.7: found by shape in every run.
const std::vector<uint64_t> kPinned[2] = {{0xdca3116aa8bc65a7ULL}, {0xe4fd0e88300a76deULL}};

// The formats a stock mask can be in that we can rebuild (ring_round.h TexFmt); the census keeps no other view.
constexpr uint32_t kFormats[] = {
    DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC1_UNORM_SRGB, DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM,
    DXGI_FORMAT_BC3_UNORM_SRGB, DXGI_FORMAT_BC4_TYPELESS, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8_TYPELESS,
    DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_A8_UNORM,
};
static_assert(sizeof(kFormats) / sizeof(kFormats[0]) <= (size_t)swap::kCensusMaxFormats, "census format list too long");

std::atomic<bool> g_enabled{ false };
std::atomic<bool> g_active{ false };          // SetActive's last word (a replacement registered later starts in this state)
std::atomic<int>  g_phase{ (int)Phase::Idle };
std::atomic<bool> g_cancel{ false };
std::mutex        g_mu;                       // guards g_status, g_req*, g_pins
Status            g_status;
uint32_t          g_reqSize[2][2] = {};
bool              g_reqVerbose = true;
std::vector<uint64_t> g_pins[2];              // testing::SetPins; empty = kPinned
uint32_t          g_gen = 0;                  // render thread: the swap generation our handles belong to

// render thread only
struct Item {                                 // one census view on its way through the readback
    int kind = -1;
    int ordinal = 0;                          // 1-based within its kind ("candidate 17 of 64")
    ID3D11ShaderResourceView* srv = nullptr;  // the stock view (the census's reference)
    ID3D11Texture2D* tex = nullptr;
    ID3D11Texture2D* staging = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    TexFmt fmt = TexFmt::Unknown;
    ULONGLONG started = 0;
    uint64_t binds = 0;
};
struct Seen {                                 // what the readback found, per candidate (index = ordinal - 1)
    CensusEntry e;
    ID3D11ShaderResourceView* srv = nullptr;  // kept only for a mask-like view, until the pick
    D3D11_TEXTURE2D_DESC desc = {};
    TexFmt fmt = TexFmt::Unknown;
    StockMask sm;
};
std::deque<Item>  g_queue;
std::vector<Item> g_flight;
std::vector<Seen> g_seen[2];
int               g_total[2] = {};
bool              g_verbose = true;
ULONGLONG         g_censusStart = 0;
uint32_t          g_censusFrames = 0;
int               g_kindOfSize[swap::kCensusMaxSizes] = {-1, -1, -1, -1};
bool              g_inRun[2] = {};            // the kinds this census looks for (asked, and not live already)

void ReleaseItem(Item& j) {
    if (j.staging) j.staging->Release();
    if (j.tex) j.tex->Release();
    if (j.srv) j.srv->Release();
    j = Item{};
}

void DropAll() {
    for (Item& j : g_queue) ReleaseItem(j);
    g_queue.clear();
    for (Item& j : g_flight) ReleaseItem(j);
    g_flight.clear();
    for (std::vector<Seen>& v : g_seen) {
        for (Seen& s : v)
            if (s.srv) { s.srv->Release(); s.srv = nullptr; }
        v.clear();
    }
    g_total[0] = g_total[1] = 0;
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

// A candidate that could not be read: its entry stays unread, so the census for its kind is incomplete (PickMask refuses to pick).
void Unread(Item& j, const char* why) {
    if (j.kind >= 0 && j.ordinal >= 1 && j.ordinal <= (int)g_seen[j.kind].size()) g_seen[j.kind][(size_t)j.ordinal - 1].e.read = false;
    GLOGW("radar mask census (0.5.6): %s candidate %d of %d (%ux%u, bound %llu times): NOT READ BACK (%s): the census is incomplete", kTexName[j.kind < 0 ? 0 : j.kind],
          j.ordinal, j.kind >= 0 ? g_total[j.kind] : 0, j.desc.Width, j.desc.Height, (unsigned long long)j.binds, why);
    ReleaseItem(j);
}

void TakeCensus() {
    swap::Census c = swap::CensusTake();
    for (int k = 0; k < 2; ++k) { g_total[k] = 0; g_seen[k].clear(); }
    for (swap::CensusCand& cand : c.cands) {
        const int kind = cand.size >= 0 && cand.size < swap::kCensusMaxSizes ? g_kindOfSize[cand.size] : -1;
        if (kind < 0) { if (cand.srv) cand.srv->Release(); continue; }
        Item it;
        it.kind = kind;
        it.ordinal = ++g_total[kind];
        it.srv = cand.srv;
        it.binds = cand.binds;
        ID3D11Resource* res = nullptr;
        it.srv->GetResource(&res);
        if (res) {
            res->QueryInterface(IID_ID3D11Texture2D, (void**)&it.tex);
            res->Release();
        }
        if (it.tex) {
            it.tex->GetDesc(&it.desc);
            it.fmt = FromDxgi(it.desc.Format);
        } else {
            it.desc.Width = cand.w;
            it.desc.Height = cand.h;
        }
        g_queue.push_back(it);
    }
    std::lock_guard<std::mutex> lk(g_mu);
    const double secs = (double)(GetTickCount64() - g_censusStart) / 1000.0;
    for (int k = 0; k < 2; ++k) {
        KindStatus& ks = g_status.k[k];
        if (!g_inRun[k]) continue;
        g_seen[k].assign((size_t)g_total[k], Seen{});
        int z = -1;
        for (int q = 0; q < swap::kCensusMaxSizes; ++q)
            if (g_kindOfSize[q] == k) z = q;
        ks.views = g_total[k];
        ks.read = 0;
        ks.shapes = 0;
        ks.notRecorded = (z >= 0 ? c.overflow[z] : 0) + c.lost;   // a bind it could not classify could have been this size
        ks.otherFormat = z >= 0 ? c.otherFormat[z] : 0;
        char b[200];
        std::snprintf(b, sizeof(b), "census: %d view(s) of %ux%u kept, %u not recorded, reading them back", ks.views, ks.w, ks.h, (unsigned)ks.notRecorded);
        ks.why = b;
        GLOGI("radar mask census (0.5.6): %s %ux%u: %d view(s) kept in %u frames / %.1f s (%llu binds seen), %u past the cap of %u, %u in formats we cannot "
              "rebuild (not kept), %u binds not classifiable (any size); reading them back",
              kTexName[k], ks.w, ks.h, ks.views, g_censusFrames, secs, (unsigned long long)c.binds, z >= 0 ? (unsigned)c.overflow[z] : 0u, (unsigned)c.cap,
              (unsigned)ks.otherFormat, (unsigned)c.lost);
    }
    g_status.lost = c.lost;
    g_status.censusBinds = c.binds;
    g_status.censusFrames = g_censusFrames;
    g_status.why = "census taken, reading back";
}

// 0 = still waiting for the GPU, 1 = done (the item released either way)
int FinishItem(Item& j) {
    ID3D11DeviceContext* ctx = swap::Context();
    if (!ctx) { Unread(j, "no device context"); return 1; }
    const int w = (int)j.desc.Width, h = (int)j.desc.Height, levels = (int)j.desc.MipLevels;
    std::vector<std::vector<uint8_t>> stock((size_t)levels);
    for (int l = 0; l < levels; ++l) {
        D3D11_MAPPED_SUBRESOURCE m = {};
        const HRESULT hr = ctx->Map(j.staging, (UINT)l, D3D11_MAP_READ, l == 0 ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            if (GetTickCount64() - j.started > kReadbackTimeoutMs) { Unread(j, "the staging copy stayed busy for 4 s"); return 1; }
            return 0;
        }
        if (FAILED(hr) || !m.pData) { Unread(j, "mapping the staging copy failed"); return 1; }
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
    Seen& s = g_seen[j.kind][(size_t)j.ordinal - 1];
    s.e.hash = hash;
    s.e.read = true;
    s.e.shape = sm.ok;
    s.e.basic = sm.basic;
    s.desc = j.desc;
    s.fmt = j.fmt;
    if (sm.basic) { s.srv = j.srv; j.srv = nullptr; }       // kept for the pick (basic: a pinned hash may pick it)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        KindStatus& ks = g_status.k[j.kind];
        ++ks.read;
        if (sm.ok) ++ks.shapes;
    }
    if (g_verbose || sm.basic)
        GLOGI("radar mask census (0.5.6): %s candidate %d of %d: %dx%d %s %d mip(s), bound %llu times, stock FNV-1a 64 %016llx: %s: %s", kTexName[j.kind], j.ordinal,
              g_total[j.kind], w, h, TexFmtName(j.fmt), levels, (unsigned long long)j.binds, (unsigned long long)hash,
              sm.ok ? "LOOKS LIKE A MASK" : (sm.basic ? "not a mask by shape, but grey / bright / dark-edged (a pinned hash may still pick it, 0.5.7)" : "not a mask"),
              sm.why.c_str());
    s.sm = std::move(sm);
    ReleaseItem(j);
    return 1;
}

void Pump() {
    ID3D11Device* dev = swap::Device();
    ID3D11DeviceContext* ctx = swap::Context();
    int starts = 0;
    while ((int)g_flight.size() < kMaxInFlight && starts < kStartsPerFrame && !g_queue.empty()) {
        Item it = g_queue.front();
        g_queue.pop_front();
        if (!it.tex) { Unread(it, "its texture could not be reached"); continue; }
        if (it.fmt == TexFmt::Unknown) { Unread(it, "a format we cannot read"); continue; }
        if (!dev || !ctx) { Unread(it, "no device"); continue; }
        D3D11_TEXTURE2D_DESC sd = it.desc;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &it.staging)) || !it.staging) { Unread(it, "no staging texture"); continue; }
        ctx->CopyResource(it.staging, it.tex);
        it.started = GetTickCount64();
        g_flight.push_back(it);
        ++starts;
    }
    int analysed = 0;
    for (size_t i = 0; i < g_flight.size() && analysed < kAnalysesPerFrame;) {
        if (FinishItem(g_flight[i]) == 0) { ++i; continue; }
        ++analysed;
        g_flight.erase(g_flight.begin() + (std::ptrdiff_t)i);
    }
}

// Our circle for the chosen stock view: same size / format / mips / view description, registered with the swap (inactive unless SetActive).
void Replace(int kind, Seen& s) {
    ID3D11Device* dev = swap::Device();
    const int w = (int)s.desc.Width, h = (int)s.desc.Height, levels = (int)s.desc.MipLevels;
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_status.k[kind].why += "; NOT replaced: " + why;
        GLOGW("radar mask census (0.5.6): %s NOT replaced: %s", kTexName[kind], why.c_str());
    };
    if (!dev) { fail("no device"); return; }
    std::vector<std::vector<Rgba>> mips;
    BuildRoundMask(w, h, levels, s.sm.inside, MaskOutside(s.sm), mips);
    std::vector<std::vector<uint8_t>> enc((size_t)levels);
    std::vector<D3D11_SUBRESOURCE_DATA> sd((size_t)levels);
    for (int l = 0; l < levels; ++l) {
        const int lw = LevelDim(w, l), lh = LevelDim(h, l);
        if (!EncodeLevel(s.fmt, mips[(size_t)l], lw, lh, enc[(size_t)l])) { fail("encoding our mask failed"); return; }
        sd[(size_t)l].pSysMem = enc[(size_t)l].data();
        sd[(size_t)l].SysMemPitch = (UINT)RowBytes(s.fmt, lw);
        sd[(size_t)l].SysMemSlicePitch = 0;
    }
    D3D11_TEXTURE2D_DESC rd = s.desc;
    rd.Usage = D3D11_USAGE_IMMUTABLE;
    rd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    rd.CPUAccessFlags = 0;
    rd.MiscFlags = 0;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&rd, sd.data(), &tex);
    if (FAILED(hr) || !tex) {
        char b[80];
        std::snprintf(b, sizeof(b), "CreateTexture2D for our mask failed (0x%08lx)", (unsigned long)hr);
        fail(b);
        return;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
    s.srv->GetDesc(&vd);
    ID3D11ShaderResourceView* ours = nullptr;
    hr = dev->CreateShaderResourceView(tex, &vd, &ours);
    tex->Release();                                             // the view keeps the texture alive
    if (FAILED(hr) || !ours) { fail("CreateShaderResourceView for our mask failed"); return; }
    const int handle = swap::Replace(s.srv, ours, g_active.load());
    ours->Release();                                            // swap holds its own reference
    if (handle < 0) { fail("the swap refused it (already replaced, or its table is full)"); return; }
    std::lock_guard<std::mutex> lk(g_mu);
    KindStatus& ks = g_status.k[kind];
    ks.handle = handle;
    ks.replaced = true;
    char b[240];
    std::snprintf(b, sizeof(b), "; REPLACED: %dx%d %s %d mip(s) (stock centre value %d, corners %d) -> our circle (registered; handed out only with the grown map)",
                  w, h, TexFmtName(s.fmt), levels, s.sm.insideValue, s.sm.outsideValue);
    ks.why += b;
    g_status.generation = swap::Generation();
    GLOGI("radar mask census (0.5.6): %s replaced by our circle (stock FNV-1a 64 %016llx; registered inactive: the bind hook hands it out while the engine's gate "
          "has the map grown)", kTexName[kind], (unsigned long long)ks.stockHash);
}

void Decide() {
    for (int k = 0; k < 2; ++k) {
        std::vector<uint64_t> pins;
        bool probed = false;
        uint32_t notRecorded = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            probed = g_inRun[k];
            notRecorded = g_status.k[k].notRecorded;
            pins = g_pins[k].empty() ? kPinned[k] : g_pins[k];
        }
        if (!probed) continue;
        std::vector<CensusEntry> entries;
        entries.reserve(g_seen[k].size());
        for (const Seen& s : g_seen[k]) entries.push_back(s.e);
        const CensusPick p = PickMask(entries, notRecorded, pins.empty() ? nullptr : pins.data(), (int)pins.size());
        {
            std::lock_guard<std::mutex> lk(g_mu);
            KindStatus& ks = g_status.k[k];
            ks.picked = true;
            ks.verdict = p.verdict;
            ks.views = p.views;
            ks.read = p.read;
            ks.shapes = p.shapes;
            ks.found = p.Ok();
            ks.why = PickText(p);
            if (p.Ok()) {
                const Seen& s = g_seen[k][(size_t)p.index];
                ks.stockHash = s.e.hash;
                ks.mips = s.desc.MipLevels;
                ks.fmt = s.fmt;
                char b[120];
                std::snprintf(b, sizeof(b), " -> candidate %d (stock FNV-1a 64 %016llx)", p.index + 1, (unsigned long long)s.e.hash);
                ks.why += b;
            }
        }
        if (p.Ok()) GLOGI("radar mask census (0.5.6): %s: %s -> candidate %d", kTexName[k], PickText(p).c_str(), p.index + 1);
        else GLOGW("radar mask census (0.5.6): %s: %s: nothing replaced", kTexName[k], PickText(p).c_str());
        if (p.Ok()) Replace(k, g_seen[k][(size_t)p.index]);
    }
    DropAll();
    std::lock_guard<std::mutex> lk(g_mu);
    g_status.why = "census decided";
}

void RenderTickImpl() {
    // the generation first: after a teardown (hooks gone) every reference to the old device's views is dropped at once
    const uint32_t gen = swap::Generation();
    if (gen != g_gen) {
        DropAll();
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_gen != 0) {
            for (KindStatus& ks : g_status.k) { ks.replaced = false; ks.handle = -1; ks.found = false; }
            // a census in progress belonged to the old device (Teardown dropped its views): it failed; a request not armed yet stands
            const Phase p = (Phase)g_phase.load();
            if (p == Phase::Recording || p == Phase::Reading) {
                g_status.why = "the D3D11 device changed during the census";
                g_phase.store((int)Phase::Failed);
            } else if (p != Phase::Requested) {
                g_status.why = "the D3D11 device changed: census again";
            }
        }
        g_gen = gen;
        g_status.generation = gen;
    }
    if (!swap::Installed()) return;
    if (g_cancel.exchange(false)) {
        DropAll();
        swap::Census c = swap::CensusTake();                     // an armed census: release what it holds
        for (swap::CensusCand& cand : c.cands)
            if (cand.srv) cand.srv->Release();
        std::lock_guard<std::mutex> lk(g_mu);
        g_status.why = "cancelled";
        g_phase.store((int)Phase::Idle);
        return;
    }
    const Phase p = (Phase)g_phase.load();
    if (p == Phase::Requested) {
        swap::CensusSize sizes[2];
        int n = 0;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (int& z : g_kindOfSize) z = -1;
            for (int k = 0; k < 2; ++k) {
                g_inRun[k] = g_status.k[k].probed && !g_status.k[k].replaced && g_reqSize[k][0] && g_reqSize[k][1];
                if (g_inRun[k]) {
                    g_kindOfSize[n] = k;
                    sizes[n++] = { g_reqSize[k][0], g_reqSize[k][1] };
                }
            }
            g_verbose = g_reqVerbose;
        }
        const bool ok = n > 0 && swap::CensusArm(sizes, n, kFormats, (int)(sizeof(kFormats) / sizeof(kFormats[0])));
        g_censusStart = GetTickCount64();
        g_censusFrames = 0;
        g_phase.store((int)(ok ? Phase::Recording : Phase::Failed));
        std::lock_guard<std::mutex> lk(g_mu);
        g_status.why = ok ? "census recording" : "the census could not be armed";
    } else if (p == Phase::Recording) {
        ++g_censusFrames;
        if (!swap::CensusArmed()) {                              // torn down under us (Teardown clears it): nothing to read
            std::lock_guard<std::mutex> lk(g_mu);
            g_status.why = "the census was disarmed by a teardown";
            g_phase.store((int)Phase::Failed);
        } else if (g_censusFrames >= kCensusMinFrames && GetTickCount64() - g_censusStart >= kCensusMinMs) {
            TakeCensus();
            g_phase.store((int)Phase::Reading);
        }
    } else if (p == Phase::Reading) {
        Pump();
        if (g_queue.empty() && g_flight.empty()) {
            Decide();
            g_phase.store((int)Phase::Done);
        }
    }
}

}  // namespace

const char* PhaseName(Phase p) {
    switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Requested: return "requested";
    case Phase::Recording: return "recording";
    case Phase::Reading: return "reading back";
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

bool RequestCensus(uint32_t wSmall, uint32_t hSmall, uint32_t wBig, uint32_t hBig, bool verbose) {
    const Phase p = (Phase)g_phase.load();
    if (p != Phase::Idle && p != Phase::Done && p != Phase::Failed) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    g_reqSize[0][0] = wSmall; g_reqSize[0][1] = hSmall;
    g_reqSize[1][0] = wBig;   g_reqSize[1][1] = hBig;
    g_reqVerbose = verbose;
    int probed = 0;
    for (int k = 0; k < 2; ++k) {
        KindStatus& ks = g_status.k[k];
        if (ks.replaced && swap::Generation() == g_status.generation) continue;   // a live replacement stays
        ks = KindStatus{};
        ks.probed = g_reqSize[k][0] && g_reqSize[k][1];
        ks.w = g_reqSize[k][0];
        ks.h = g_reqSize[k][1];
        if (ks.probed) ++probed;
    }
    if (!probed) return false;
    g_status.why = "census requested";
    g_phase.store((int)Phase::Requested);
    return true;
}

void Cancel() { g_cancel.store(true); }

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
void SetPins(int kind, const std::vector<uint64_t>& hashes) {
    if (kind < 0 || kind > 1) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_pins[kind] = hashes;
}
}

}  // namespace gage::game::radarmask
