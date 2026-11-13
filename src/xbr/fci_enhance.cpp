// Rendering enhancements on the D3D12 layer (gpu/ext.h) and the graphics options (fci_gfx.h): ambient
// occlusion, edge anti-aliasing, shadow filtering and scaling.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "core/log.h"
#include "fci_gfx.h"
#include "fci_glyphs.h"
#include "gpu/ext.h"

// ---- options ---------------------------------------------------------------------------------
namespace {
std::mutex g_opt_mu;
std::atomic<uint32_t> g_opt_gen{1};
}   // namespace

namespace fci_gfx {
uint32_t Generation() { return g_opt_gen.load(std::memory_order_acquire); }

std::string Value(const char* name)
{
    std::lock_guard lock(g_opt_mu);
    const char* e = std::getenv(name);
    return e ? e : "";
}

float Number(const char* name, float fallback)
{
    std::string v = Value(name);
    char* end = nullptr;
    float f = v.empty() ? fallback : std::strtof(v.c_str(), &end);
    return end && end != v.c_str() ? f : fallback;
}
}   // namespace fci_gfx

extern "C" int fci_gfx_set(const char* key_value)
{
    const char* eq = std::strchr(key_value, '=');
    if (std::strncmp(key_value, "RECOMP_FCI_XBR_", 15) != 0 || !eq)
        return 0;
    {
        std::lock_guard lock(g_opt_mu);
        if (_putenv(key_value) != 0)
            return 0;
    }
    g_opt_gen.fetch_add(1, std::memory_order_acq_rel);
    std::fprintf(stderr, "[FCI] graphics option %s\n", key_value);
    return 1;
}

// ---- the extension -----------------------------------------------------------------------------
namespace {
using gpu::Context;
using gpu::State;
using gpu::Target;
using Microsoft::WRL::ComPtr;

constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kDepthRead = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;   // d3d12.cpp's host depth, as read
constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R16G16_FLOAT;                // occlusion, depth
// NV2A methods the trigger reads (gpu/state.h names them; SimpleState is d3d12.cpp's)
constexpr uint32_t kMBlendEnable = 0x304, kMSrcBlend = 0x344, kMZWriteEnable = 0x35C;
constexpr uint32_t kBlendSrcAlpha = 0x302;
constexpr uint32_t kRsZEnable = 143;
constexpr uint32_t kOpaqueDraws = 4;   // world draws before the scene counts as a 3D frame

const char* kAoHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; };
Texture2D src0 : register(t0);
Texture2D src1 : register(t1);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
// ao: p0 = texel size, tan of the half field of view (x, y); p1 = radius (depth units), strength, fade from, to
float Dep(float2 uv) { return src0.SampleLevel(ps, uv, 0).r; }
float3 Pos(float2 uv, float d) { return float3((uv * float2(2, -2) + float2(-1, 1)) * p0.zw * d, d); }
float2 ao(O i) : SV_Target {
    float2 uv = i.uv;
    float d = Dep(uv);
    if (d <= 0 || d >= 0.9999) return float2(1, d);
    float3 P = Pos(uv, d);
    float2 px = p0.xy;
    float dl = Dep(uv - float2(px.x, 0)), dr = Dep(uv + float2(px.x, 0));
    float du = Dep(uv - float2(0, px.y)), dd = Dep(uv + float2(0, px.y));
    // the neighbours on the same surface: the nearer one in depth on each axis
    float3 tx = abs(dr - d) < abs(d - dl) ? Pos(uv + float2(px.x, 0), dr) - P : P - Pos(uv - float2(px.x, 0), dl);
    float3 ty = abs(dd - d) < abs(d - du) ? Pos(uv + float2(0, px.y), dd) - P : P - Pos(uv - float2(0, px.y), du);
    float3 n = normalize(cross(tx, ty));
    if (dot(n, P) > 0) n = -n;
    float2 ruv = 0.5 * p1.x / (d * p0.zw);
    float shrink = min(1.0, 0.12 / max(ruv.y, 1e-6));  // near the camera: at most an eighth of the screen,
    ruv *= shrink;                                     // and the radius in the world with it
    float r = p1.x * shrink;
    float noise = frac(52.9829189 * frac(dot(i.p.xy, float2(0.06711056, 0.00583715))));
    float occ = 0;
    const int N = 16;
    [unroll] for (int k = 0; k < N; k++) {
        float t = (k + noise) / N;
        float a = 2.39996323 * k + noise * 6.2831853;
        float2 suv = uv + float2(cos(a), sin(a)) * ruv * t;
        float3 v = (Pos(suv, Dep(suv)) - P) / r;
        float vv = dot(v, v);
        occ += saturate(dot(n, v) * rsqrt(vv + 1e-6) - 0.1) * saturate(1 - vv);
    }
    float A = saturate(1 - p1.y * occ * (2.0 / N));
    return float2(lerp(A, 1, saturate((d - p1.z) / max(p1.w - p1.z, 1e-6))), d);
}
// blur: p0.xy = one texel along the axis
float2 blur(O i) : SV_Target {
    float2 c = src0.SampleLevel(ps, i.uv, 0).rg;
    float sum = c.r, sw = 1;
    [unroll] for (int k = -4; k <= 4; k++) {
        if (k == 0) continue;
        float2 s = src0.SampleLevel(ps, i.uv + p0.xy * k, 0).rg;
        float w = exp(-k * k / 12.0) * saturate(1 - abs(s.g - c.g) / (0.03 * c.g + 1e-6));
        sum += s.r * w;
        sw += w;
    }
    return float2(sum / sw, c.g);
}
// apply: p0.x = 2 shows the occlusion alone, 3 only on the right half (a same frame comparison)
float4 apply(O i) : SV_Target {
    float4 c = src0.SampleLevel(ps, i.uv, 0);
    float a = src1.SampleLevel(ps, i.uv, 0).r;
    if (p0.x > 2.5) return i.uv.x < 0.5 ? c : abs(i.uv.x - 0.5) < 0.001 ? float4(1, 0, 0, c.a) : float4(c.rgb * a, c.a);
    if (p0.x > 1.5) return float4(a, a, a, c.a);
    return float4(c.rgb * a, c.a);
}
)";

// Edge anti-aliasing (RECOMP_FCI_XBR_AA), after FXAA: blend across luma edges by the distance to the
// edge's nearer end. p0 = texel size; p1.x = 2 leaves the left half alone (a side-by-side comparison).
const char* kAaHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; };
Texture2D src0 : register(t0);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 C(float2 uv) { return src0.SampleLevel(ls, uv, 0).rgb; }
float L(float2 uv) { return dot(C(uv), float3(0.299, 0.587, 0.114)); }
float4 aa(O i) : SV_Target {
    float2 uv = i.uv, px = p0.xy;
    float4 cm = src0.SampleLevel(ps, uv, 0);
    if (p1.x > 1.5 && uv.x < 0.5) return cm;
    float lm = dot(cm.rgb, float3(0.299, 0.587, 0.114));
    float ln = L(uv + float2(0, -px.y)), lsth = L(uv + float2(0, px.y));
    float le = L(uv + float2(px.x, 0)), lw = L(uv + float2(-px.x, 0));
    float hi = max(lm, max(max(ln, lsth), max(le, lw))), lo = min(lm, min(min(ln, lsth), min(le, lw)));
    float range = hi - lo;
    if (range < max(0.0312, hi * 0.125)) return cm;
    float lnw = L(uv - px), lse = L(uv + px);
    float lne = L(uv + float2(px.x, -px.y)), lsw = L(uv + float2(-px.x, px.y));
    float eh = abs(lnw + lne - 2 * ln) + 2 * abs(lw + le - 2 * lm) + abs(lsw + lse - 2 * lsth);
    float ev = abs(lnw + lsw - 2 * lw) + 2 * abs(ln + lsth - 2 * lm) + abs(lne + lse - 2 * le);
    bool horz = eh >= ev;
    float l1 = horz ? ln : lw, l2 = horz ? lsth : le;
    float g1 = abs(l1 - lm), g2 = abs(l2 - lm);
    float stepv = horz ? px.y : px.x;
    float ledge, grad;
    if (g1 >= g2) { stepv = -stepv; ledge = 0.5 * (l1 + lm); grad = g1; }
    else { ledge = 0.5 * (l2 + lm); grad = g2; }
    float2 e = uv + (horz ? float2(0, stepv * 0.5) : float2(stepv * 0.5, 0));
    float2 dir = horz ? float2(px.x, 0) : float2(0, px.y);
    float2 a = e - dir, b = e + dir;
    float da = L(a) - ledge, db = L(b) - ledge;
    bool ea = abs(da) >= grad * 0.25, eb = abs(db) >= grad * 0.25;
    const float steps[10] = {1, 1, 1, 1.5, 2, 2, 2, 4, 8, 8};
    [unroll] for (int k = 0; k < 10; k++) {
        if (ea && eb) break;
        if (!ea) { a -= dir * steps[k]; da = L(a) - ledge; ea = abs(da) >= grad * 0.25; }
        if (!eb) { b += dir * steps[k]; db = L(b) - ledge; eb = abs(db) >= grad * 0.25; }
    }
    float dist_a = horz ? uv.x - a.x : uv.y - a.y, dist_b = horz ? b.x - uv.x : b.y - uv.y;
    bool near_a = dist_a < dist_b;
    float off = 0.5 - min(dist_a, dist_b) / (dist_a + dist_b);
    if (((near_a ? da : db) < 0) == (lm < ledge)) off = 0;  // the nearer end is not where this edge ends
    float avg = (2 * (ln + lsth + le + lw) + lnw + lne + lsw + lse) / 12;
    float sub = saturate(abs(avg - lm) / range);
    sub = (-2 * sub + 3) * sub * sub;
    off = max(off, sub * sub * 0.75);
    float2 f = uv + (horz ? float2(0, off * stepv) : float2(off * stepv, 0));
    return float4(C(f), cm.a);
}
)";

// Shadows: RECOMP_FCI_XBR_SHADOW_FILTER=1 takes a 4 x 4 grid of the console's four-texel compares for softer
// edges; ext.x carries the setting, so it changes without new shaders.
struct FciShaderHooks final : gpu::ShaderHooks {
    std::string Shadow() const override
    {
        return R"(float4 shadow(Texture2D<float> t, SamplerComparisonState s, float2 uv, float z, int fmt, float2 px, float2 size) {
    float mx = fmt == 2 ? 65535.0 : 16777215.0;
    float r = clamp(z, 0, mx) / mx;
    if (ext.x < 0.5) return t.SampleCmpLevelZero(s, uv, r).xxxx;
    float2 st = 1.0 / size;
    float sum = 0;
    [unroll] for (int y = 0; y < 4; y++)
        [unroll] for (int x = 0; x < 4; x++) sum += t.SampleCmpLevelZero(s, uv + (float2(x, y) - 1.5) * st, r);
    return (sum / 16).xxxx;
}
)";
    }
};

class FciExtension final : public gpu::Extension {
  public:
    bool Settings() override
    {
        fci_glyphs::Poll();
        uint32_t gen = fci_gfx::Generation();
        if (gen == gen_)
            return false;
        gen_ = gen;
        float shadow = fci_gfx::Number("RECOMP_FCI_XBR_SHADOW_SCALE", 0);
        shadow = shadow >= 0.5f && shadow <= 8 ? shadow : 0;
        bool remake = shadow != shadow_scale_;
        shadow_scale_ = shadow;
        shadow_filter_ = int(fci_gfx::Number("RECOMP_FCI_XBR_SHADOW_FILTER", 0));
        ao_mode_ = int(fci_gfx::Number("RECOMP_FCI_XBR_AO", 0));
        ao_strength_ = fci_gfx::Number("RECOMP_FCI_XBR_AO_STRENGTH", 1.2f);
        ao_radius_ = fci_gfx::Number("RECOMP_FCI_XBR_AO_RADIUS", 1.5f);
        ao_at_ = int(fci_gfx::Number("RECOMP_FCI_XBR_AO_AT", 0));
        aa_mode_ = int(fci_gfx::Number("RECOMP_FCI_XBR_AA", 0));
        return remake;   // the shadow map is made again at its new size
    }

    void Reset(Context&) override
    {
        for (auto& r : ao_)
            r.Reset();
        post_.Reset();
        scene_rt_ = scene_ds_ = nullptr;
    }

    // the sun shadow map: a square depth target drawn with a colour target of its size (1024 x 1024 in
    // Training); it takes the larger render scale so it stays square, other targets keep the render scale
    void MakeTarget(const gpu::Surface&, bool, uint32_t gw, uint32_t gh, float& sx, float& sy,
                    DXGI_FORMAT&) override
    {
        if (gw != gh || gw < kShadowMin)
            return;
        sx = sy = shadow_scale_ > 0 ? shadow_scale_ : std::max(sx, sy);
    }

    const gpu::ShaderHooks* Shaders() const override { return &hooks_; }

    void Decoded(const gpu::TextureDesc& d, const gpu::FormatInfo& fi, uint32_t, uint32_t level, std::vector<uint8_t>& px,
                 uint32_t pitch, uint8_t&) override
    { fci_glyphs::Patch(d, fi, level, px, pitch); }

    void Constants(float (*)[4], float* ext, const Target*, uint32_t) override
    { ext[0] = float(shadow_filter_); }

    bool Bound(Context& c, const State& s, Target* rt, Target* ds, bool pretransformed) override
    {
        uint64_t frame = c.Presents();
        if (frame != frame_) {
            frame_ = frame;
            done_ = false;
            opaque_ = 0;
            scene_rt_ = scene_ds_ = nullptr;
        }
        if (!ao_mode_ || done_ || !rt)
            return false;
        uint32_t gw, gh;
        c.GuestSize(gw, gh);
        bool world = ds && !pretransformed && s.rs[kRsZEnable] == 2;   // W-buffered: FCI's world
        if (rt->gw == gw && rt->gh == gh && world && ds->w == rt->w && ds->h == rt->h) {
            bool zwrite = s.simple[kMZWriteEnable >> 2] != 0;
            bool alpha = s.simple[kMBlendEnable >> 2] && s.simple[kMSrcBlend >> 2] == kBlendSrcAlpha;
            if (zwrite) {   // opaque, alpha tested or blended foliage: all in the depth the pass reads
                opaque_++;
                scene_rt_ = rt;
                scene_ds_ = ds;
                // the view's projection, as the title also sets it for its fixed function draws
                const float* pm = s.transforms[3].data();
                if (pm[0] > 0.1f && pm[5] > 0.1f) {
                    tan_x_ = 1.0f / pm[0];
                    tan_y_ = 1.0f / pm[5];
                }
                if (!logged_) {
                    logged_ = true;
                    LOGI("fci ao: scene %ux%u, projection %.3f %.3f %.3f %.3f, zscale %.0f", rt->w, rt->h,
                         pm[0], pm[5], pm[10], pm[14], s.zscale);
                }
                return false;
            }
            // ao_at_ 1: before the first blended surface that leaves depth alone (terrain layers, water,
            // particles); 0: after them, when the world is done
            if (!(ao_at_ == 1 && alpha))
                return false;
        }
        if (opaque_ < kOpaqueDraws || !scene_rt_)
            return false;
        done_ = true;
        return Occlude(c, *scene_rt_, *scene_ds_);
    }

    ID3D12Resource* Present(Context& c, Target& t, DXGI_FORMAT& fmt) override
    {
        if (!aa_mode_ || t.depth || !EnsureTexture(c, post_, post_state_, t.w, t.h, t.fmt, 2))
            return nullptr;
        c.Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        c.Transition(post_.Get(), post_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12Resource* src[3] = {t.res.Get(), nullptr, nullptr};
        DXGI_FORMAT sf[3] = {t.fmt, kBgra, kBgra};
        float p[8] = {1.0f / t.w, 1.0f / t.h, 0, 0, float(aa_mode_)};
        c.Pass(kAaHlsl, "aa", t.fmt, Rtv(2), t.w, t.h, src, sf, p, -1);
        c.Unbind();
        Target shown;   // F3 / RECOMP_FCI_XBR_CAPTURE: the picture as shown, at full size
        shown.res = post_;
        shown.w = shown.gw = t.w;
        shown.h = shown.gh = t.h;
        shown.fmt = t.fmt;
        shown.state = post_state_;
        c.Capture(shown, "shown", 1);
        c.Transition(post_.Get(), shown.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        post_state_ = shown.state;
        fmt = t.fmt;
        return post_.Get();
    }

  private:
    // a w x h render target of format f with its view at Rtv(slot); made again when the size changes
    bool EnsureTexture(Context& c, ComPtr<ID3D12Resource>& r, D3D12_RESOURCE_STATES& st, uint32_t w,
                       uint32_t h, DXGI_FORMAT f, int slot)
    {
        if (r) {
            D3D12_RESOURCE_DESC d = r->GetDesc();
            if (d.Width == w && d.Height == h && d.Format == f)
                return true;
            c.Release(r);
            r.Reset();
        }
        if (!EnsureHeap(c))
            return false;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = f;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        st = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(c.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr,
                                                       IID_PPV_ARGS(&r))))
            return false;
        c.Device()->CreateRenderTargetView(r.Get(), nullptr, Rtv(slot));
        return true;
    }

    bool EnsureHeap(Context& c)
    {
        if (rtv_heap_)
            return true;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvSlots};
        if (FAILED(c.Device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_))))
            return false;
        rtv_inc_ = c.Device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        return true;
    }

    bool EnsureAo(Context& c, uint32_t w, uint32_t h)
    {
        return EnsureTexture(c, ao_[0], ao_state_[0], w, h, kAoFormat, 0)
               && EnsureTexture(c, ao_[1], ao_state_[1], w, h, kAoFormat, 1);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(int i) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(i) * rtv_inc_;
        return h;
    }

    // occlusion into ao_[0], blurred across and back, then the scene colour times it; true: rebind
    bool Occlude(Context& c, Target& rt, Target& ds)
    {
        if (!EnsureAo(c, rt.w, rt.h))
            return false;
        uint32_t w = rt.w, h = rt.h;
        // depth units per metre: Training's 39,475 w per metre over the 24 bit range
        constexpr float kDepthPerMetre = 39475.0f / 16777215.0f;
        float radius = ao_radius_ * kDepthPerMetre;
        c.Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        c.Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12Resource* src[3] = {ds.res.Get(), nullptr, nullptr};
        DXGI_FORMAT fmt[3] = {kDepthRead, kBgra, kBgra};
        // fades out from 80 to 200 metres, into FCI's fog
        float p[8] = {1.0f / w,
                      1.0f / h,
                      tan_x_,
                      tan_y_,
                      radius,
                      ao_strength_,
                      80 * kDepthPerMetre,
                      200 * kDepthPerMetre};
        c.Pass(kAoHlsl, "ao", kAoFormat, Rtv(0), w, h, src, fmt, p, -1);
        for (int axis = 0; axis < 2; axis++) {
            int from = axis, to = 1 - axis;
            c.Transition(ao_[from].Get(), ao_state_[from], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            c.Transition(ao_[to].Get(), ao_state_[to], D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12Resource* bs[3] = {ao_[from].Get(), nullptr, nullptr};
            DXGI_FORMAT bf[3] = {kAoFormat, kBgra, kBgra};
            float bp[8] = {axis == 0 ? 1.0f / w : 0.0f, axis == 1 ? 1.0f / h : 0.0f};
            c.Pass(kAoHlsl, "blur", kAoFormat, Rtv(to), w, h, bs, bf, bp, -1);
        }
        c.Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* fb = c.Feedback(rt);
        if (!fb)
            return true;
        c.Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12Resource* as[3] = {fb, ao_[0].Get(), nullptr};
        DXGI_FORMAT af[3] = {rt.fmt, kAoFormat, kBgra};
        float ap[8] = {float(ao_mode_)};
        c.Pass(kAoHlsl, "apply", rt.fmt, c.View(rt), w, h, as, af, ap, -1);
        c.Unbind();
        return true;
    }

    uint32_t gen_ = 0;
    int ao_mode_ = 0, ao_at_ = 0, aa_mode_ = 0, shadow_filter_ = 0;
    float shadow_scale_ = 0;   // 0: the render scale
    static constexpr uint32_t kShadowMin = 512;
    FciShaderHooks hooks_;
    float ao_strength_ = 1, ao_radius_ = 1;
    float tan_x_ = 0.89f, tan_y_ = 0.67f;   // FCI's on foot view (Training: projection 1.122, 1.497)
    uint64_t frame_ = ~0ull;
    bool done_ = false, logged_ = false;
    uint32_t opaque_ = 0;
    Target* scene_rt_ = nullptr;
    Target* scene_ds_ = nullptr;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    UINT rtv_inc_ = 0;
    ComPtr<ID3D12Resource> ao_[2];
    D3D12_RESOURCE_STATES ao_state_[2]{};
    ComPtr<ID3D12Resource> post_;   // the anti-aliased picture
    D3D12_RESOURCE_STATES post_state_{};
    static constexpr UINT kRtvSlots = 3;   // the occlusion and its blur, the anti-aliased picture
};

// before the backend is made (d3d8.cpp makes it at CreateDevice): g_extension is constant initialised
const bool g_registered = [] {
    gpu::SetExtension(std::make_unique<FciExtension>());
    return true;
}();
}   // namespace
