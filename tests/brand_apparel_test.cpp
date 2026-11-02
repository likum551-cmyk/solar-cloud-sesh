// brand_apparel_test: offline proofs for apps/engine/brand_apparel.{h,cpp} (the GAGE-MP logo on the default tee).
//
//   brand_apparel_test.exe [real_k.bin [out_dir [log_file]]]
//
//   real_k.bin = the 8 raw mips (174760 bytes, BC1, 512 px) of the stock texture jbib_diff_008_k_uni, extracted
//                from the player's own game files by the scratch tool (never committed). Without it the tests that
//                need the real bytes are reported as SKIPPED (and the exit code is still 0 only if nothing failed).
//   out_dir    = where the composited chain is dumped (composited_k.bin, for the Node preview render).
//
// Runs on a WARP (software) D3D11 device: no GPU, no game. It exercises the same two vtable hooks the engine
// installs, against a device it created itself.
#include "../apps/engine/brand_apparel.h"

#include "common/logger.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace gage::game::brand;
namespace T = gage::game::brand::testing;

static int g_fail = 0, g_pass = 0, g_skip = 0;
#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        if (cond) { ++g_pass; }                                                          \
        else { ++g_fail; std::printf("FAIL  %s:%d  %s  -- ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

using Mips = std::vector<std::vector<uint8_t>>;

static int BlocksAcross(int w) { return w < 4 ? 1 : (w + 3) / 4; }
static int MipW(int w, int l) { int v = w >> l; return v < 1 ? 1 : v; }

// ---- an independent FNV-1a 64 (the test must not call the code under test to decide the expected value) ---------
static uint64_t RefTailHash(int width, int levels, const Mips& m) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int l = 0; l < levels; ++l) {
        if (MipW(width, l) > 64) continue;
        for (uint8_t b : m[(size_t)l]) { h ^= b; h *= 0x100000001b3ULL; }
    }
    return h;
}

static Mips SplitMips(const std::vector<uint8_t>& raw, int width, int levels, int bpb) {
    Mips m((size_t)levels);
    size_t off = 0;
    for (int l = 0; l < levels; ++l) {
        const int bw = BlocksAcross(MipW(width, l));
        const size_t n = (size_t)bw * bw * bpb;
        m[(size_t)l].assign(raw.begin() + (std::ptrdiff_t)off, raw.begin() + (std::ptrdiff_t)(off + n));
        off += n;
    }
    return m;
}

static double Psnr(double mse) { return mse <= 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse); }

// Build a chain of BC1/BC3 mips from a function pixel(level, x, y) -> rgb, using the encoder under test.
template <class F>
static Mips BuildChain(int width, int levels, int bpb, F px) {
    Mips m((size_t)levels);
    for (int l = 0; l < levels; ++l) {
        const int w = MipW(width, l), bw = BlocksAcross(w);
        m[(size_t)l].assign((size_t)bw * bw * bpb, 0);
        for (int by = 0; by < bw; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                float p[16][3];
                for (int i = 0; i < 16; ++i) {
                    const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    float c[3];
                    px(l, x < w ? x : w - 1, y < w ? y : w - 1, c);
                    p[i][0] = c[0]; p[i][1] = c[1]; p[i][2] = c[2];
                }
                uint8_t* dst = m[(size_t)l].data() + ((size_t)by * bw + bx) * bpb;
                if (bpb == 16) { for (int k = 0; k < 8; ++k) dst[k] = (uint8_t)(0xA0 + k * 3 + bx + by); dst += 8; }
                T::EncodeBC1Block(p, dst);
            }
    }
    return m;
}

static void DecodePx(const Mips& m, int width, int level, int bpb, int x, int y, float out[3]) {
    const int w = MipW(width, level), bw = BlocksAcross(w);
    const uint8_t* blk = m[(size_t)level].data() + ((size_t)(y / 4) * bw + (x / 4)) * bpb;
    float p[16][3];
    T::DecodeBC1Block(bpb == 16 ? blk + 8 : blk, p);
    const int i = (y % 4) * 4 + (x % 4);
    out[0] = p[i][0]; out[1] = p[i][1]; out[2] = p[i][2];
}

// ---------------------------------------------------------------------------------------------------------------
static void TestHash() {
    std::printf("-- tail hash\n");
    Mips m = BuildChain(512, 8, 8, [](int l, int x, int y, float* c) { c[0] = (float)((x * 7 + l * 13) & 255); c[1] = (float)((y * 5) & 255); c[2] = 90; });
    const uint64_t a = T::TailHash(512, 8, m);
    CHECK(a == RefTailHash(512, 8, m), "got %016llx", (unsigned long long)a);
    Mips m0 = m;
    m0[0][100] ^= 0xFF;     // 512 px level: not part of the fingerprint (survives a texture-quality drop of the top mip)
    m0[1][100] ^= 0xFF;     // 256 px
    m0[2][100] ^= 0xFF;     // 128 px
    CHECK(T::TailHash(512, 8, m0) == a, "levels above 64 px leaked into the hash");
    Mips m3 = m;
    m3[3][5] ^= 0x01;       // 64 px: part of it
    CHECK(T::TailHash(512, 8, m3) != a, "64 px level not hashed");
    Mips m7 = m;
    m7[7][0] ^= 0x80;
    CHECK(T::TailHash(512, 8, m7) != a, "4 px level not hashed");
    // the same chain with the 512 px top level dropped (texture quality "high" streams 256 as the top) hashes alike
    Mips dropped(m.begin() + 1, m.end());
    CHECK(T::TailHash(256, 7, dropped) == a, "a dropped top mip changes the fingerprint");
}

static void TestEncoder() {
    std::printf("-- BC1 encoder\n");
    // flat block
    {
        float p[16][3], q[16][3];
        for (int i = 0; i < 16; ++i) { p[i][0] = 100; p[i][1] = 150; p[i][2] = 200; }
        uint8_t b[8];
        T::EncodeBC1Block(p, b);
        T::DecodeBC1Block(b, q);
        double mse = 0;
        for (int i = 0; i < 16; ++i) for (int c = 0; c < 3; ++c) mse += (p[i][c] - q[i][c]) * (p[i][c] - q[i][c]);
        mse /= 48;
        CHECK(Psnr(mse) > 38.0, "flat block PSNR %.1f", Psnr(mse));
    }
    // two-colour block (a hard edge, like the logo's rim)
    {
        float p[16][3], q[16][3];
        for (int i = 0; i < 16; ++i) { const bool hi = (i & 3) >= 2; p[i][0] = hi ? 242 : 40; p[i][1] = hi ? 241 : 90; p[i][2] = hi ? 238 : 160; }
        uint8_t b[8];
        T::EncodeBC1Block(p, b);
        T::DecodeBC1Block(b, q);
        double mse = 0;
        for (int i = 0; i < 16; ++i) for (int c = 0; c < 3; ++c) mse += (p[i][c] - q[i][c]) * (p[i][c] - q[i][c]);
        mse /= 48;
        CHECK(Psnr(mse) > 36.0, "edge block PSNR %.1f", Psnr(mse));
    }
    // gradients and noise over many pseudo-random blocks; the mode must always be 4-colour or flat
    uint32_t s = 12345;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) & 0xffff; };
    double worstGrad = 99, sumGrad = 0;
    int nGrad = 0, badMode = 0;
    for (int n = 0; n < 2000; ++n) {
        float base[3] = { (float)(rnd() % 256), (float)(rnd() % 256), (float)(rnd() % 256) };
        float dir[3] = { (float)((int)(rnd() % 61) - 30), (float)((int)(rnd() % 61) - 30), (float)((int)(rnd() % 61) - 30) };
        float p[16][3], q[16][3];
        for (int i = 0; i < 16; ++i) for (int c = 0; c < 3; ++c) {
            float v = base[c] + dir[c] * ((i & 3) - 1.5f) * 0.5f + dir[c] * ((i >> 2) - 1.5f) * 0.25f + ((int)(rnd() % 5) - 2);
            p[i][c] = v < 0 ? 0 : (v > 255 ? 255 : v);
        }
        uint8_t b[8];
        T::EncodeBC1Block(p, b);
        bool three = false;
        T::DecodeBC1Block(b, q, &three);
        const uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8)), c1 = (uint16_t)(b[2] | (b[3] << 8));
        if (three && c0 != c1) ++badMode;       // 3-colour mode (c0 <= c1) is only acceptable as the flat c0 == c1 case
        double mse = 0;
        for (int i = 0; i < 16; ++i) for (int c = 0; c < 3; ++c) mse += (p[i][c] - q[i][c]) * (p[i][c] - q[i][c]);
        mse /= 48;
        const double ps = Psnr(mse);
        sumGrad += ps; ++nGrad;
        if (ps < worstGrad) worstGrad = ps;
    }
    std::printf("   gradient blocks: mean PSNR %.1f dB, worst %.1f dB, 3-colour-mode blocks %d\n", sumGrad / nGrad, worstGrad, badMode);
    CHECK(sumGrad / nGrad > 36.0, "mean PSNR %.1f", sumGrad / nGrad);
    CHECK(worstGrad > 28.0, "worst PSNR %.1f", worstGrad);
    CHECK(badMode == 0, "%d blocks came out in the 3-colour mode", badMode);
}

static bool BlockTouched(int bx, int by, int k, const T::Decal& d) {
    const int rx = d.x >> k, ry = d.y >> k, rw = d.w >> k, rh = d.h >> k;
    if (rw <= 0 || rh <= 0) return false;
    return bx >= rx / 4 && bx <= (rx + rw - 1) / 4 && by >= ry / 4 && by <= (ry + rh - 1) / 4;
}

static void TestComposite() {
    std::printf("-- compositor (synthetic chains)\n");
    auto bg = [](int l, int x, int y, float* c) { c[0] = 40.f + (float)((x + l * 3) % 90); c[1] = 70.f + (float)((y * 2) % 80); c[2] = 130.f; };
    for (int bpb : { 8, 16 }) {
        Mips before = BuildChain(512, 8, bpb, bg);
        Mips m = before;
        T::Decal d{ 64, 64, 64, 64, std::vector<uint8_t>(64 * 64, 255), { 255, 255, 255 } };
        const int n = T::CompositeChain(512, 8, bpb, 512, { d }, m);
        // expected: rect 64 -> blocks 16x16 at k=0, 8x8 at k=1, 4x4 at k=2, 2x2 at k=3, 1 at k=4 ; k>=5 untouched
        CHECK(n == 256 + 64 + 16 + 4 + 1, "re-encoded %d blocks (bpb %d)", n, bpb);
        int diffOutside = 0, sameInside = 0;
        for (int l = 0; l < 8; ++l) {
            const int w = MipW(512, l), bw = BlocksAcross(w);
            const int k = l;
            for (int by = 0; by < bw; ++by) for (int bx = 0; bx < bw; ++bx) {
                const bool touched = k < 5 && BlockTouched(bx, by, k, d);
                const bool same = std::memcmp(&before[(size_t)l][((size_t)by * bw + bx) * bpb], &m[(size_t)l][((size_t)by * bw + bx) * bpb], (size_t)bpb) == 0;
                if (!touched && !same) ++diffOutside;
                if (touched && same) ++sameInside;
            }
        }
        CHECK(diffOutside == 0, "%d blocks outside the decal changed (bpb %d)", diffOutside, bpb);
        CHECK(sameInside == 0, "%d blocks inside the decal stayed (bpb %d)", sameInside, bpb);
        float c[3];
        DecodePx(m, 512, 0, bpb, 96, 96, c);
        CHECK(c[0] > 245 && c[1] > 245 && c[2] > 245, "decal interior (%g,%g,%g)", c[0], c[1], c[2]);
        DecodePx(m, 512, 2, bpb, 24, 24, c);       // 128 px level: rect is 16..32
        CHECK(c[0] > 245 && c[1] > 245 && c[2] > 245, "mip2 interior (%g,%g,%g)", c[0], c[1], c[2]);
        if (bpb == 16)
            for (int by = 16; by < 32; ++by) for (int bx = 16; bx < 32; ++bx)
                CHECK(std::memcmp(&before[0][((size_t)by * 128 + bx) * 16], &m[0][((size_t)by * 128 + bx) * 16], 8) == 0, "BC3 alpha bytes changed at %d,%d", bx, by);
    }
    // half alpha: the blend lands between the stock colour and the ink
    {
        Mips m = BuildChain(512, 8, 8, [](int, int, int, float* c) { c[0] = 40; c[1] = 80; c[2] = 120; });
        float s[3];
        DecodePx(m, 512, 0, 8, 100, 100, s);
        T::Decal d{ 64, 64, 64, 64, std::vector<uint8_t>(64 * 64, 128), { 255, 255, 255 } };
        T::CompositeChain(512, 8, 8, 512, { d }, m);
        float c[3];
        DecodePx(m, 512, 0, 8, 100, 100, c);
        const float a = 128.f / 255.f;
        for (int k = 0; k < 3; ++k) CHECK(std::fabs(c[k] - (s[k] * (1 - a) + 255 * a)) < 9.f, "half alpha ch %d: %g vs expected %g", k, c[k], s[k] * (1 - a) + 255 * a);
    }
    // a 256 px variant of the texture (streaming drops the top mip): the decal lands at the same place on the cloth
    {
        Mips m = BuildChain(256, 7, 8, bg);
        T::Decal d{ 64, 64, 64, 64, std::vector<uint8_t>(64 * 64, 255), { 255, 255, 255 } };
        const int n = T::CompositeChain(256, 7, 8, 512, { d }, m);
        CHECK(n == 64 + 16 + 4 + 1, "256 px variant: %d blocks", n);
        float c[3];
        DecodePx(m, 256, 0, 8, 48, 48, c);         // rect is 32..64 at this size
        CHECK(c[0] > 245 && c[1] > 245 && c[2] > 245, "256 px interior (%g,%g,%g)", c[0], c[1], c[2]);
        DecodePx(m, 256, 0, 8, 20, 20, c);
        CHECK(!(c[0] > 245 && c[1] > 245 && c[2] > 245), "256 px outside is white");
    }
    // refusals
    {
        Mips m = BuildChain(512, 8, 8, bg);
        T::Decal d{ 64, 64, 64, 64, std::vector<uint8_t>(64 * 64, 255), { 255, 255, 255 } };
        CHECK(T::CompositeChain(1024, 8, 8, 512, { d }, m) == -1, "wider-than-base accepted");
        CHECK(T::CompositeChain(512, 8, 12, 512, { d }, m) == -1, "bad block size accepted");
        Mips few(2);
        CHECK(T::CompositeChain(512, 8, 8, 512, { d }, few) == -1, "short chain accepted");
    }
}

// ---- D3D11 (WARP) helpers --------------------------------------------------------------------------------------
struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
};

static bool MakeGpu(Gpu& g) {
    D3D_FEATURE_LEVEL fl;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx);
    if (FAILED(hr)) { std::printf("   D3D11CreateDevice(WARP) failed 0x%08lx\n", (unsigned long)hr); return false; }
    std::printf("   WARP device %p, immediate context %p, feature level 0x%x\n", (void*)g.dev, (void*)g.ctx, (unsigned)fl);
    return true;
}

static ID3D11ShaderResourceView* MakeTex(Gpu& g, int width, const Mips& mips, bool bc3, ID3D11Texture2D** texOut = nullptr) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = (UINT)width;
    d.MipLevels = (UINT)mips.size();
    d.ArraySize = 1;
    d.Format = bc3 ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_BC1_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> sd(mips.size());
    for (size_t l = 0; l < mips.size(); ++l) {
        sd[l].pSysMem = mips[l].data();
        sd[l].SysMemPitch = (UINT)(BlocksAcross(MipW(width, (int)l)) * (bc3 ? 16 : 8));
    }
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g.dev->CreateTexture2D(&d, sd.data(), &tex)) || !tex) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(g.dev->CreateShaderResourceView(tex, nullptr, &srv))) { tex->Release(); return nullptr; }
    if (texOut) *texOut = tex; else tex->Release();
    return srv;
}

static bool ReadSrv(Gpu& g, ID3D11ShaderResourceView* srv, int bpb, Mips& out) {
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    ID3D11Texture2D* tex = nullptr;
    if (!res || FAILED(res->QueryInterface(IID_ID3D11Texture2D, (void**)&tex))) { if (res) res->Release(); return false; }
    res->Release();
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &st))) { tex->Release(); return false; }
    g.ctx->CopyResource(st, tex);
    out.assign(d.MipLevels, {});
    for (UINT l = 0; l < d.MipLevels; ++l) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g.ctx->Map(st, l, D3D11_MAP_READ, 0, &m))) { st->Release(); tex->Release(); return false; }
        const int bw = BlocksAcross(MipW((int)d.Width, (int)l));
        out[l].resize((size_t)bw * bw * bpb);
        for (int r = 0; r < bw; ++r) std::memcpy(out[l].data() + (size_t)r * bw * bpb, (const uint8_t*)m.pData + (size_t)r * m.RowPitch, (size_t)bw * bpb);
        g.ctx->Unmap(st, l);
    }
    st->Release();
    tex->Release();
    return true;
}

// Bind `srv` at slot `slot` through the (hooked) context and report which SRV the context actually holds.
static ID3D11ShaderResourceView* BindAndGet(Gpu& g, ID3D11ShaderResourceView* srv, UINT slot = 0) {
    ID3D11ShaderResourceView* arr[1] = { srv };
    g.ctx->PSSetShaderResources(slot, 1, arr);
    ID3D11ShaderResourceView* got[1] = { nullptr };
    g.ctx->PSGetShaderResources(slot, 1, got);
    return got[0];            // AddRef'd by Get; the caller releases
}

static bool PumpUntilApplied(Gpu& g, ID3D11ShaderResourceView* bindMe, uint32_t want, int ms) {
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < (ULONGLONG)ms) {
        ID3D11ShaderResourceView* r = BindAndGet(g, bindMe);
        if (r) r->Release();
        T::Pump(g.ctx);
        if (GetStats().applied >= want) return true;
        Sleep(5);
    }
    return false;
}

static bool Inside(int x, int y, int w, int h, int bx, int by) {      // 4x4 block (bx,by) overlaps rect?
    return bx * 4 < x + w && bx * 4 + 4 > x && by * 4 < y + h && by * 4 + 4 > y;
}

// ---------------------------------------------------------------------------------------------------------------
static void TestEndToEndSynthetic() {
    std::printf("-- hook end to end (WARP, synthetic target)\n");
    Gpu g;
    if (!MakeGpu(g)) { ++g_fail; std::printf("FAIL  no D3D11 device\n"); return; }
    T::Reset();
    Mips target = BuildChain(512, 8, 8, [](int l, int x, int y, float* c) { c[0] = 30.f + (float)((x * 3 + l) % 100); c[1] = 90.f + (float)((y * 5) % 60); c[2] = 160; });
    Mips other = BuildChain(512, 8, 8, [](int l, int x, int y, float* c) { c[0] = 200.f - (float)((x * 2 + l) % 80); c[1] = (float)((y * 3) % 200); c[2] = 40; });
    const uint64_t hash = RefTailHash(512, 8, target);
    T::Decal d{ 96, 96, 64, 32, std::vector<uint8_t>(64 * 32, 255), { 250, 250, 250 } };
    CHECK(T::AddTarget("synthetic tee", hash, false, 512, 8, { d }), "AddTarget");
    ID3D11ShaderResourceView* sTarget = MakeTex(g, 512, target, false);
    ID3D11ShaderResourceView* sOther = MakeTex(g, 512, other, false);
    CHECK(sTarget && sOther, "texture creation");
    if (!sTarget || !sOther) return;

    // Where does the vtable we patch live? (informational, it is why the patch is per object and why the engine
    // keeps its own reference on the device and context while hooked.) A module image address would mean a table
    // shared by every context; a heap address means it belongs to this one object.
    void** ivt = *reinterpret_cast<void***>(g.ctx);
    {
        HMODULE mod = nullptr;
        const BOOL inModule = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ivt, &mod);
        std::printf("   info: the immediate context's vtable %p is %s\n", (void*)ivt, inModule ? "inside a loaded module (shared)" : "not in any module (per-object heap block)");
    }
    void* origSlot = ivt[8];

    CHECK(T::InstallOn(g.dev, g.ctx), "InstallOn");
    CHECK(ivt[8] != origSlot, "PSSetShaderResources slot not patched");
    CHECK(GetStats().installed, "stats.installed");

    // before the fingerprint is known, the game gets what it bound
    ID3D11ShaderResourceView* r0 = BindAndGet(g, sTarget);
    CHECK(r0 == sTarget, "first bind already swapped");
    if (r0) r0->Release();

    CHECK(PumpUntilApplied(g, sTarget, 1, 8000), "target was not replaced within 8 s (stats: candidates %u fingerprinted %u)", GetStats().candidates, GetStats().fingerprinted);
    // after: bound SRV differs for the target, equals itself for the other texture
    ID3D11ShaderResourceView* r1 = BindAndGet(g, sTarget);
    CHECK(r1 && r1 != sTarget, "target still bound as the stock srv");
    ID3D11ShaderResourceView* r2 = BindAndGet(g, sOther);
    CHECK(r2 == sOther, "unrelated texture was swapped");
    if (r2) r2->Release();
    PumpUntilApplied(g, sOther, 1, 300);
    ID3D11ShaderResourceView* r3 = BindAndGet(g, sOther);
    CHECK(r3 == sOther, "unrelated texture swapped after classification");
    if (r3) r3->Release();

    // multi-slot bind: the caller's array stays untouched, slot 1 is replaced, slots 0/2 are as given
    {
        ID3D11ShaderResourceView* arr[3] = { sOther, sTarget, nullptr };
        g.ctx->PSSetShaderResources(0, 3, arr);
        CHECK(arr[0] == sOther && arr[1] == sTarget && arr[2] == nullptr, "caller array was modified");
        ID3D11ShaderResourceView* got[3] = {};
        g.ctx->PSGetShaderResources(0, 3, got);
        CHECK(got[0] == sOther && got[1] == r1 && got[2] == nullptr, "multi-slot bind: %p %p %p (want %p %p null)", (void*)got[0], (void*)got[1], (void*)got[2], (void*)sOther, (void*)r1);
        for (auto* p : got) if (p) p->Release();
    }

    // contents of the replacement: only blocks under the decal changed, the interior is the ink colour
    Mips rep;
    CHECK(r1 && ReadSrv(g, r1, 8, rep), "read back replacement");
    if (r1 && !rep.empty()) {
        int outside = 0, inside = 0;
        for (int l = 0; l < 8; ++l) {
            const int bw = BlocksAcross(MipW(512, l));
            for (int by = 0; by < bw; ++by) for (int bx = 0; bx < bw; ++bx) {
                const bool diff = std::memcmp(&target[(size_t)l][((size_t)by * bw + bx) * 8], &rep[(size_t)l][((size_t)by * bw + bx) * 8], 8) != 0;
                const bool in = l < 5 && Inside(d.x >> l, d.y >> l, d.w >> l, d.h >> l, bx, by);
                if (diff && !in) ++outside;
                if (diff && in) ++inside;
            }
        }
        CHECK(outside == 0, "%d blocks changed outside the decal", outside);
        // 128 (k=0: 16x8 blocks) + 32 + 8 + 2 + 2 (k=1..4) for the 64x32 rect at (96,96)
        CHECK(inside == 172, "%d blocks changed under the decal, expected 172", inside);
        float c[3];
        DecodePx(rep, 512, 0, 8, 120, 110, c);
        CHECK(c[0] > 240 && c[1] > 240 && c[2] > 240, "replacement interior (%g,%g,%g)", c[0], c[1], c[2]);
    }
    if (r1) r1->Release();

    // a mutable texture created EMPTY and filled later (streaming): the first look sees zeros, the retry catches it
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = d.Height = 512; d.MipLevels = 8; d.ArraySize = 1; d.Format = DXGI_FORMAT_BC1_UNORM;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* tex = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        CHECK(SUCCEEDED(g.dev->CreateTexture2D(&d, nullptr, &tex)) && tex, "create empty mutable texture");
        if (tex && SUCCEEDED(g.dev->CreateShaderResourceView(tex, nullptr, &srv)) && srv) {
            const uint32_t before = GetStats().applied;
            ID3D11ShaderResourceView* x = BindAndGet(g, srv);
            CHECK(x == srv, "empty texture swapped");
            if (x) x->Release();
            for (int i = 0; i < 40; ++i) { T::Pump(g.ctx); Sleep(5); }          // first look: zeros, not a target
            CHECK(GetStats().applied == before, "empty texture was replaced");
            for (size_t l = 0; l < target.size(); ++l)
                g.ctx->UpdateSubresource(tex, (UINT)l, nullptr, target[l].data(), (UINT)(BlocksAcross(MipW(512, (int)l)) * 8), 0);
            const bool ok = PumpUntilApplied(g, srv, before + 1, 8000);
            CHECK(ok, "texture filled after creation was never replaced (retry path)");
            ID3D11ShaderResourceView* y = BindAndGet(g, srv);
            CHECK(y && y != srv, "filled texture still stock");
            if (y) y->Release();
        }
        if (srv) srv->Release();
        if (tex) tex->Release();
    }

    // a view re-created at the old address would be re-classified (the CreateShaderResourceView hook): just exercise it
    {
        ID3D11ShaderResourceView* extra = MakeTex(g, 512, other, false);
        CHECK(extra != nullptr, "CreateShaderResourceView through the hook");
        if (extra) extra->Release();
    }

    T::Reset();     // = Shutdown + clear
    CHECK(ivt[8] == origSlot, "vtable slot not restored by Shutdown");
    CHECK(!GetStats().installed, "stats.installed after shutdown");
    // after Shutdown the context binds what it is given
    ID3D11ShaderResourceView* r4 = BindAndGet(g, sTarget);
    CHECK(r4 == sTarget, "swap still active after Shutdown");
    if (r4) r4->Release();
    sTarget->Release();
    sOther->Release();
    g.ctx->Release();
    g.dev->Release();
}

static void TestRealData(const std::string& realPath, const std::string& outDir) {
    std::printf("-- real stock texture bytes (%s)\n", realPath.c_str());
    std::vector<uint8_t> raw;
    {
        FILE* f = std::fopen(realPath.c_str(), "rb");
        if (!f) { std::printf("FAIL  cannot open %s\n", realPath.c_str()); ++g_fail; return; }
        std::fseek(f, 0, SEEK_END);
        raw.resize((size_t)std::ftell(f));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(raw.data(), 1, raw.size(), f) != raw.size()) { std::fclose(f); ++g_fail; std::printf("FAIL  short read\n"); return; }
        std::fclose(f);
    }
    CHECK(raw.size() == 174760, "size %zu", raw.size());
    if (raw.size() != 174760) return;
    Mips m = SplitMips(raw, 512, 8, 8);
    const uint64_t h = T::TailHash(512, 8, m);
    std::printf("   tail hash %016llx\n", (unsigned long long)h);
    CHECK(h == 0x67964881c72ba99dULL, "fingerprint %016llx", (unsigned long long)h);

    // how lossy is a decode -> encode round trip of the stock top mip (= the cost of re-encoding a touched block)?
    {
        const int bw = 128;
        double sum = 0, worst = 99;
        int n = 0;
        for (int by = 0; by < bw; ++by) for (int bx = 0; bx < bw; ++bx) {
            float p[16][3], q[16][3];
            uint8_t b[8];
            T::DecodeBC1Block(&m[0][((size_t)by * bw + bx) * 8], p);
            T::EncodeBC1Block(p, b);
            T::DecodeBC1Block(b, q);
            double mse = 0;
            for (int i = 0; i < 16; ++i) for (int c = 0; c < 3; ++c) mse += (p[i][c] - q[i][c]) * (p[i][c] - q[i][c]);
            const double ps = Psnr(mse / 48);
            sum += ps; ++n;
            if (ps < worst) worst = ps;
        }
        std::printf("   stock mip0 decode->encode round trip: mean PSNR %.1f dB, worst block %.1f dB (%d blocks)\n", sum / n, worst, n);
        CHECK(sum / n > 40.0, "mean %.1f", sum / n);
        CHECK(worst > 24.0, "worst %.1f", worst);
    }

    // the real thing: the production target table (no Reset: it would empty it), hooks on a WARP device, the stock
    // bytes as the texture, bound the way the game binds it
    Gpu g;
    if (!MakeGpu(g)) { ++g_fail; std::printf("FAIL  no D3D11 device\n"); return; }
    ID3D11ShaderResourceView* sA = MakeTex(g, 512, m, false);
    ID3D11ShaderResourceView* sB = MakeTex(g, 512, m, false);        // a second instance of the same tee (another player)
    Mips variant = m;
    variant[6][3] ^= 0x10;                                             // a different variant colour: other tail bytes
    ID3D11ShaderResourceView* sV = MakeTex(g, 512, variant, false);
    CHECK(sA && sB && sV, "texture creation");
    if (!sA || !sB || !sV) return;
    CHECK(T::InstallOn(g.dev, g.ctx), "InstallOn");

    CHECK(PumpUntilApplied(g, sA, 1, 15000), "stock tee not replaced within 15 s (candidates %u, fingerprinted %u)", GetStats().candidates, GetStats().fingerprinted);
    ID3D11ShaderResourceView* rA = BindAndGet(g, sA);
    CHECK(rA && rA != sA, "bound srv for the stock tee is still the stock srv");

    // second instance: replaced on its own
    CHECK(PumpUntilApplied(g, sB, 2, 15000), "second instance not replaced (applied %u)", GetStats().applied);
    ID3D11ShaderResourceView* rB = BindAndGet(g, sB);
    CHECK(rB && rB != sB && rB != rA, "second instance: %p (stock %p, first replacement %p)", (void*)rB, (void*)sB, (void*)rA);

    // a different variant (other tail bytes) must NOT be touched
    PumpUntilApplied(g, sV, 99, 1500);
    ID3D11ShaderResourceView* rV = BindAndGet(g, sV);
    CHECK(rV == sV, "a texture with another fingerprint was swapped");
    CHECK(GetStats().applied == 2, "applied %u", GetStats().applied);
    if (rV) rV->Release();

    Mips rep;
    CHECK(rA && ReadSrv(g, rA, 8, rep), "read back the replacement");
    if (rA && rep.size() == 8) {
        // Decal rects of apps/engine/brand_apparel_data.inc (base 512 px). If make_decals.mjs is re-run with other
        // artwork, update these two lines; the test then guards "nothing outside the decals changes".
        const int R[2][4] = { { 64, 240, 96, 112 }, { 288, 256, 160, 48 } };
        int changed[8] = {}, outside = 0;
        for (int l = 0; l < 8; ++l) {
            const int bw = BlocksAcross(MipW(512, l));
            for (int by = 0; by < bw; ++by) for (int bx = 0; bx < bw; ++bx) {
                if (std::memcmp(&m[(size_t)l][((size_t)by * bw + bx) * 8], &rep[(size_t)l][((size_t)by * bw + bx) * 8], 8) == 0) continue;
                ++changed[l];
                bool in = false;
                for (const auto& r : R) in = in || Inside(r[0] >> l, r[1] >> l, r[2] >> l, r[3] >> l, bx, by);
                if (!in) ++outside;
            }
        }
        std::printf("   changed blocks per mip:");
        for (int l = 0; l < 8; ++l) std::printf(" %d", changed[l]);
        std::printf("   (outside the decals: %d)\n", outside);
        CHECK(outside == 0, "%d changed blocks outside the decal rects", outside);
        CHECK(changed[0] > 150 && changed[0] < 800, "mip0 changed blocks %d", changed[0]);
        CHECK(changed[5] == 0 && changed[6] == 0 && changed[7] == 0, "tiny mips were touched");
        CHECK(changed[4] > 0, "the 32 px mip got no logo");
        // the replacement's own fingerprint differs from the stock one (so it is never mistaken for it again)
        CHECK(T::TailHash(512, 8, rep) != 0x67964881c72ba99dULL, "replacement has the stock fingerprint");
        // brand ink is present in the front mark: the brightest decoded pixel in the front rect is near the ink colour
        float best = 0;
        for (int y = 240; y < 352; ++y) for (int x = 64; x < 160; ++x) {
            float c[3];
            DecodePx(rep, 512, 0, 8, x, y, c);
            if (c[0] > best) best = c[0];
        }
        CHECK(best > 225.f, "brightest front pixel %g", best);

        std::string path = outDir + "/composited_k.bin";
        FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
            for (const auto& lv : rep) std::fwrite(lv.data(), 1, lv.size(), f);
            std::fclose(f);
            std::printf("   wrote %s\n", path.c_str());
        } else { ++g_fail; std::printf("FAIL  cannot write %s\n", path.c_str()); }
    }
    if (rA) rA->Release();
    if (rB) rB->Release();
    Shutdown();
    sA->Release(); sB->Release(); sV->Release();
    g.ctx->Release();
    g.dev->Release();
}

int main(int argc, char** argv) {
    const std::string real = argc > 1 ? argv[1] : "";
    const std::string outDir = argc > 2 ? argv[2] : ".";
    if (argc > 3) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, nullptr, 0);
        std::wstring w((size_t)(n > 0 ? n - 1 : 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, argv[3], -1, w.data(), n);
        gage::log::init(w);
    }
    TestHash();
    TestEncoder();
    TestComposite();
    if (!real.empty()) TestRealData(real, outDir); else { ++g_skip; std::printf("-- real stock texture: SKIPPED (no real_k.bin argument)\n"); }
    TestEndToEndSynthetic();      // after the real-data run: Reset() empties the production target table
    std::printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
