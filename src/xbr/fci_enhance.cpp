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
constexpr uint32_t kRsFogEnable = 92;   // D3DRS_FOGENABLE
constexpr uint32_t kRsFogColor = 138;   // D3DRS_FOGCOLOR
constexpr uint32_t kOpaqueDraws = 4;    // world draws before the scene counts as a 3D frame
// the D3D transform slots, by D3DTRANSFORMSTATETYPE (the State's transforms are indexed by these)
constexpr uint32_t kTsView = 2;         // D3DTS_VIEW
constexpr uint32_t kTsProjection = 3;   // D3DTS_PROJECTION

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

// Post-pass fog (RECOMP_FCI_XBR_FOG): height fog and a ground layer in place of the console's per-vertex
// fog; formulas in docs/rendering.md. "ring" gathers the sky's horizon colour per azimuth into a 64 x 1
// target kept across frames; "fog" composites in place with c.Feedback. p7.z: 1 the fog, 2 the debug.
// p0 = (texel_x, texel_y, tan_x, tan_y); p1 = (s0, a, H, h0); p2 = (sun_dir, sun_strength);
// p3 = (fog_col, w-units/metre); p4..p6 the view rows (world to view, translation in .w);
// p7 = (thickness, sky knee, mode, water_h); p8 = (view z sign, -, -, -); p9 = (layer density, layer top,
// soft, seconds); p10 = (noise, noise size m, sky ray m, -); p11 = (sun colour, sky tint);
// p12 = (march steps, march metres, -, -)
const char* kFogHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; float4 p2; float4 p3; float4 p4; float4 p5; float4 p6; float4 p7; float4 p8; float4 p9; float4 p10; float4 p11; float4 p12; };
Texture2D dep  : register(t0);   // the W-buffer depth (0..1 in .r)
Texture2D colr : register(t1);   // the scene colour
Texture2D ring : register(t2);   // the horizon colour per azimuth, 64 x 1
SamplerState sr : register(s0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
// the world position (metres) of a view-space point: R^T * (Pvm - t), the view rows are R
float3 World(float3 Pvm) {
    float3 dv = Pvm - float3(p4.w, p5.w, p6.w);
    return float3(dot(dv, p4.xyz), dot(dv, p5.xyz), dot(dv, p6.xyz));
}
bool Sky(float d) { return d <= 0.0 || d >= 0.9999; }
// the fog amount for an optical depth: the editor's thickness scales the density, so far enough is always
// fully fogged; p12.z 1: the game's meaning, thickness caps the opacity
float Amount(float tau) {
    tau = clamp(tau, 0.0, 50.0);
    return p12.z > 0.5 ? p7.x * (1.0 - exp(-tau)) : 1.0 - exp(-tau * p7.x);
}
// each bin: the visible sky a little above the horizon at its azimuth; none visible: keep the old value
float4 ringp(O i) : SV_Target {
    float phi = (i.uv.x - 0.5) * 6.2831853;
    float3 acc = 0;
    float n = 0;
    [unroll] for (int j = 0; j < 4; j++) {
        float e = 0.02 + 0.05 * j;
        float3 dw = float3(cos(phi) * sqrt(1.0 - e * e), sin(phi) * sqrt(1.0 - e * e), e);
        float3 v = float3(dot(p4.xyz, dw), dot(p5.xyz, dw), dot(p6.xyz, dw));
        if (v.z * p8.x <= 1e-4) continue;   // behind the camera
        float2 nd = v.xy * (p8.x / v.z) / p0.zw;
        if (abs(nd.x) > 0.98 || abs(nd.y) > 0.98) continue;
        float2 uv = float2(nd.x * 0.5 + 0.5, 0.5 - nd.y * 0.5);
        if (!Sky(dep.SampleLevel(sr, uv, 0).r)) continue;
        acc += colr.SampleLevel(sr, uv, 0).rgb;
        n += 1.0;
    }
    if (n < 0.5) discard;
    return float4(acc / n, 1);
}
float4 ringinit(O i) : SV_Target { return float4(p3.xyz, 0); }   // alpha: how much sky a bin has seen
// the fog's colour along a world ray: the game's fog colour tinted by how the sky's horizon at this azimuth
// (bins blurred, weighted by what they saw) differs from the horizon's mean; plus the sun's glow
float3 FogCol(float3 rd) {
    int i0 = (int)floor((atan2(rd.y, rd.x) / 6.2831853 + 0.5) * 64.0);
    float4 dc = 0, mc = 0;
    [unroll] for (int k = -8; k <= 8; k++) {
        float4 r = ring.Load(int3((i0 + k + 128) % 64, 0, 0));
        dc += float4(r.rgb * r.a, r.a) * (9.0 - abs(k)) * 0.25;
    }
    [unroll] for (int j = 0; j < 16; j++) {
        float4 r = ring.Load(int3(j * 4, 0, 0));
        mc += float4(r.rgb * r.a, r.a);
    }
    float3 tint = 1.0;
    if (dc.a > 2.0 && mc.a > 1.0)
        tint = clamp((dc.rgb / dc.a) / max(mc.rgb / mc.a, 0.02), 0.7, 1.4);
    // steeply down or up the azimuth means little and its bins fan out on screen: no tint there
    float3 col = p3.xyz * lerp(1.0, tint, p11.w * (1.0 - smoothstep(0.25, 0.7, abs(rd.z))));
    float mu = saturate(dot(rd, p2.xyz));
    return col + p11.xyz * p2.w * (0.6 * pow(mu, 8.0) + 0.4 * pow(mu, 64.0));
}
float H3(float3 q) { return frac(sin(dot(q, float3(127.1, 311.7, 74.7))) * 43758.5453); }
float VN3(float3 x) {
    float3 i = floor(x), f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(H3(i), H3(i + float3(1, 0, 0)), f.x), lerp(H3(i + float3(0, 1, 0)), H3(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(H3(i + float3(0, 0, 1)), H3(i + float3(1, 0, 1)), f.x), lerp(H3(i + float3(0, 1, 1)), H3(i + 1.0), f.x), f.y), f.z);
}
// the layer's density factor at p: drifting 3D noise, flattened vertically, mean about 1
float Mist(float3 p) {
    float3 q = p / float3(p10.y, p10.y, p10.y * 0.4) + p9.w * float3(0.031, 0.017, 0.004);
    float n = 0.65 * VN3(q) + 0.35 * VN3(q * 2.13 + 5.2);
    return lerp(1.0, 2.0 * saturate(0.5 + (n - 0.5) * 2.5), p10.x);
}
// the layer's density profile integrated over height: 1 below top - soft, a linear fade to 0 at top
float G(float h) {
    float b = p9.y - p9.z, a = clamp(h - b, 0.0, p9.z);
    return min(h, b) + a - a * a / (2.0 * p9.z);
}
float LayerMean(float3 a, float3 b) {   // the closed form with the mean density
    float dh = b.z - a.z;
    float avg = abs(dh) < 1e-3 ? saturate((p9.y - a.z) / p9.z) : (G(b.z) - G(a.z)) / dh;
    return p9.x * length(b - a) * avg;
}
// the ground layer's optical depth from a to b: marched through the noise for the near in-layer part
// (p12.y metres, p12.x steps, jittered per pixel), the closed form beyond
float Layer(float3 a, float3 b, float jit) {
    if (p9.x <= 0.0) return 0.0;
    if (p12.x < 0.5 || p10.x <= 0.0) return LayerMean(a, b);
    float3 ab = b - a;
    float L = max(length(ab), 1e-3);
    float t0 = 0.0, t1 = 1.0;
    if (abs(ab.z) > 1e-3) {
        float tt = (p9.y - a.z) / ab.z;
        if (ab.z > 0.0) t1 = min(1.0, tt); else t0 = max(0.0, tt);
    } else if (a.z >= p9.y) return 0.0;
    if (t1 <= t0) return 0.0;
    float tm = min(t1, t0 + p12.y / L);
    float3 rd = ab / L, pa = a + ab * t0;
    float tau = LayerMean(a + ab * tm, a + ab * t1);
    int n = (int)p12.x;
    float ds = (tm - t0) * L / n;
    [loop] for (int k = 0; k < n; k++) {
        float3 q = pa + rd * ((k + jit) * ds);
        tau += p9.x * saturate((p9.y - q.z) / p9.z) * Mist(q) * ds;
    }
    return tau;
}
float4 fog(O i) : SV_Target {
    float2 uv = i.uv;
    float d = dep.SampleLevel(sr, uv, 0).r;
    float4 c = colr.SampleLevel(sr, uv, 0);
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    float3 dir = float3(ndc * p0.zw, p8.x);   // the view-space ray, unnormalized; p8.x: forward is +z or -z
    float3 cam = World(float3(0, 0, 0));      // the camera, at the origin in view space
    float3 rd = normalize(World(dir) - cam);  // the world-space view ray
    float jit = frac(52.9829189 * frac(dot(i.p.xy, float2(0.06711056, 0.00583715))));
    if (p7.z > 1.5) {   // the debug
        if (Sky(d)) return float4(0, 0, 0, c.a);
        float3 Pw = World(dir * (d * 16777215.0 / p3.w));
        float band = frac(Pw.z * 0.5);
        float3 col = float3(band, 1.0 - band, 0.4);
        col = lerp(col, float3(1, 1, 1), saturate(1.0 - abs(Pw.z - p7.w) * 8.0));
        col = lerp(col, float3(0, 1, 1), p9.x > 0.0 ? saturate(1.0 - abs(Pw.z - p9.y) * 8.0) : 0.0);
        return float4(col, c.a);   // alpha untouched: later draws blend with it
    }
    float3 fc = FogCol(rd);
    float eu = exp(-(cam.z - p1.w) / p1.z);
    if (Sky(d)) {   // the sky: what a level ray gets at the sky ray's length, faded upward; then the layer
        float f = Amount(p1.x * p10.z * (p1.y + (1.0 - p1.y) * eu)) * (1.0 - smoothstep(0.0, p7.y, rd.z));
        float fl = Amount(Layer(cam, cam + rd * p10.z, jit));
        return float4(lerp(c.rgb, fc, 1.0 - (1.0 - f) * (1.0 - fl)), c.a);
    }
    float3 Pvm = dir * (d * 16777215.0 / p3.w);   // metres
    float3 Pw = World(Pvm);
    // the height fog's optical depth, integrated along the ray in closed form
    float k = (Pw.z - cam.z) / p1.z;
    float g = abs(k) < 1e-4 ? eu : eu * (1.0 - exp(-k)) / k;
    float tau = p1.x * length(Pvm) * (p1.y + (1.0 - p1.y) * g) + Layer(cam, Pw, jit);
    return float4(lerp(c.rgb, fc, Amount(tau)), c.a);
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
        fog_mode_ = int(fci_gfx::Number("RECOMP_FCI_XBR_FOG", 0));
        fog_mode_ = fog_mode_ < 0 ? 0 : (fog_mode_ > 2 ? 2 : fog_mode_);
        fog_floor_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_FLOOR", 0.35f);
        fog_height_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_HEIGHT", 6.0f);
        fog_base_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_BASE", -1.0f);   // -1: the water height
        fog_sky_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_SKY", 0.2f);
        fog_wpm_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_WPM", 39475.0f);
        fog_layer_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_LAYER", 5.0f);   // its top above h0; 0 off
        fog_layer_vis_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_LAYER_VIS", 40.0f);
        fog_layer_soft_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_LAYER_SOFT", 3.0f);
        fog_noise_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_NOISE", 0.8f);
        fog_noise_scale_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_NOISE_SCALE", 25.0f);
        fog_sun_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_SUN", 0.6f);
        fog_tint_ = std::clamp(fci_gfx::Number("RECOMP_FCI_XBR_FOG_TINT", 1.0f), 0.0f, 1.0f);
        fog_steps_ = std::clamp(std::floor(fci_gfx::Number("RECOMP_FCI_XBR_FOG_STEPS", 16)), 0.0f, 64.0f);
        fog_march_ = std::max(1.0f, fci_gfx::Number("RECOMP_FCI_XBR_FOG_MARCH", 150));
        fog_cap_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_CAP", 0) > 0.5f ? 1.0f : 0.0f;
        return remake;   // the shadow map is made again at its new size
    }

    void Reset(Context&) override
    {
        for (auto& r : ao_)
            r.Reset();
        post_.Reset();
        scene_rt_ = scene_ds_ = nullptr;
        done_ = fog_done_ = false;
        fog_captured_ = refl_captured_ = false;
        fog_gate_ = 0;
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

    void Decoded(const gpu::TextureDesc& d, const gpu::FormatInfo& fi, uint32_t, uint32_t level,
                 std::vector<uint8_t>& px, uint32_t pitch, uint8_t&) override
    { fci_glyphs::Patch(d, fi, level, px, pitch); }

    void Constants(float (*)[4], float* ext, const Target*, uint32_t) override
    {
        ext[0] = float(shadow_filter_);
        ext[1] = fog_gate_;   // the per-draw fog kill: the scene weight is 1 (no game fog) on gated draws
    }

    // The world is the guest-size target the W-buffered draws write to; after its opaque draws the
    // first non-world draw is the moment the passes run. The AO (ao_at_ 0) and the fog both run there,
    // on the world scene target, the fog after the AO; ao_at_ 1's AO instead waits for the first
    // blended surface. Each reads the scene's depth and colour; the frame's result is copied back in
    // place with c.Feedback. The scene tracking runs for both, so the fog works with the AO off.
    bool Bound(Context& c, const State& s, Target* rt, Target* ds, bool pretransformed) override
    {
        uint64_t frame = c.Presents();
        if (frame != frame_) {
            frame_ = frame;
            done_ = fog_done_ = false;
            opaque_ = 0;
            scene_rt_ = scene_ds_ = nullptr;
            fog_captured_ = refl_captured_ = false;
        }
        if (!rt)
            return false;
        uint32_t gw, gh;
        c.GuestSize(gw, gh);
        bool world = ds && !pretransformed && s.rs[kRsZEnable] == 2;   // W-buffered: FCI's world
        bool main = rt->gw == gw && rt->gh == gh && world && ds->w == rt->w && ds->h == rt->h;
        // kill the game fog on the draws our pass covers; scene draws after the pass keep the game's fog
        fog_gate_ = fog_mode_ && main && !fog_done_ && s.rs[kRsFogEnable] != 0 ? 1.0f : 0.0f;
        if (main) {
            bool zwrite = s.simple[kMZWriteEnable >> 2] != 0;
            bool alpha = s.simple[kMBlendEnable >> 2] && s.simple[kMSrcBlend >> 2] == kBlendSrcAlpha;
            if (zwrite) {   // opaque, alpha tested or blended foliage: all in the depth the pass reads
                opaque_++;
                scene_rt_ = rt;
                scene_ds_ = ds;
                // the view's projection, as the title also sets it for its fixed function draws
                const float* pm = s.transforms[kTsProjection].data();
                if (pm[0] > 0.1f && pm[5] > 0.1f) {
                    tan_x_ = 1.0f / pm[0];
                    tan_y_ = 1.0f / pm[5];
                }
                if (fog_mode_ && s.rs[kRsFogEnable] != 0 && !fog_captured_)
                    CaptureFog(s);
                if (fog_mode_)
                    CaptureSun(s);
                if (!logged_) {
                    logged_ = true;
                    LOGI("fci ao: scene %ux%u, projection %.3f %.3f %.3f %.3f, zscale %.0f", rt->w, rt->h,
                         pm[0], pm[5], pm[10], pm[14], s.zscale);
                }
                return false;
            }
            // ao_at_ 1: before the first blended surface that leaves depth alone (terrain layers, water,
            // particles)
            if (ao_mode_ && !done_ && ao_at_ == 1 && alpha && opaque_ >= kOpaqueDraws) {
                done_ = true;
                return Occlude(c, *scene_rt_, *scene_ds_);
            }
            return false;
        }
        // a non-main W-buffered fogged draw: the water reflection; its camera sets the water height
        if (fog_mode_ && !refl_captured_ && world && s.rs[kRsFogEnable] != 0)
            CaptureRefl(s);
        // the world is done: the first non-main draw after the opaque world; the AO (ao_at_ 0) and the
        // fog run here, on the world scene target, the fog after the AO
        if (opaque_ >= kOpaqueDraws && scene_rt_) {
            if (ao_mode_ && !done_ && ao_at_ != 1) {
                done_ = true;
                Occlude(c, *scene_rt_, *scene_ds_);
            }
            if (fog_mode_ && !fog_done_ && fog_captured_)
                return Fog(c, *scene_rt_, *scene_ds_);
        }
        return false;
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
        c.Pass(kAaHlsl, "aa", t.fmt, Rtv(2), t.w, t.h, src, sf, p, 8, -1);
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
        c.Pass(kAoHlsl, "ao", kAoFormat, Rtv(0), w, h, src, fmt, p, 8, -1);
        for (int axis = 0; axis < 2; axis++) {
            int from = axis, to = 1 - axis;
            c.Transition(ao_[from].Get(), ao_state_[from], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            c.Transition(ao_[to].Get(), ao_state_[to], D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12Resource* bs[3] = {ao_[from].Get(), nullptr, nullptr};
            DXGI_FORMAT bf[3] = {kAoFormat, kBgra, kBgra};
            float bp[8] = {axis == 0 ? 1.0f / w : 0.0f, axis == 1 ? 1.0f / h : 0.0f};
            c.Pass(kAoHlsl, "blur", kAoFormat, Rtv(to), w, h, bs, bf, bp, 8, -1);
        }
        c.Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* fb = c.Feedback(rt);
        if (!fb)
            return true;
        c.Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12Resource* as[3] = {fb, ao_[0].Get(), nullptr};
        DXGI_FORMAT af[3] = {rt.fmt, kAoFormat, kBgra};
        float ap[8] = {float(ao_mode_)};
        c.Pass(kAoHlsl, "apply", rt.fmt, c.View(rt), w, h, as, af, ap, 8, -1);
        c.Unbind();
        return true;
    }

    // the fog register, the colour and the main view, from the first gated (main, fogged) draw. The
    // register is the c[k] with {x < 0, y = 1, z > 0, w = 1}: the end in w-units is z, the thickness
    // -x * z
    void CaptureFog(const State& s)
    {
        for (const auto& v : s.vsc)
            if (v[0] < -1e-9f && std::abs(v[1] - 1.0f) < 1e-5f && v[2] > 1000.0f
                && std::abs(v[3] - 1.0f) < 1e-5f) {
                fog_end_w_ = v[2];
                fog_thick_ = -v[0] * v[2];
                break;
            }
        if (fog_end_w_ <= 0)
            return;   // no register: the pass does nothing this frame
        uint32_t argb = s.rs[kRsFogColor];
        fog_col_[0] = ((argb >> 16) & 255) / 255.0f;
        fog_col_[1] = ((argb >> 8) & 255) / 255.0f;
        fog_col_[2] = (argb & 255) / 255.0f;
        const float* vm = s.transforms[kTsView].data();
        // D3D's row vectors: rows 0..2 rotate, row 3 translates; the shader takes the rows with T in .w
        for (int r = 0; r < 3; r++) {
            for (int cc = 0; cc < 3; cc++)
                view_[r * 4 + cc] = vm[r * 4 + cc];
            view_[r * 4 + 3] = vm[12 + r];
        }
        Camera(vm, cam_main_);
        Projection(s, vm);
        fog_captured_ = true;
    }

    // The sun, from a lit draw: Evolution's vertex programs light with max(dot(c186, N), c165.x) * c189, so
    // c186 is toward the sun (unit, w 1) and c189 its colour (w 1). Other programs use c186 otherwise.
    void CaptureSun(const State& s)
    {
        const auto& d = s.vsc[186];
        const auto& k = s.vsc[189];
        float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (std::abs(n - 1.0f) > 0.01f || d[3] != 1.0f || k[3] != 1.0f)
            return;
        for (int i = 0; i < 3; i++) {
            if (!(k[i] >= 0.0f && k[i] <= 4.0f))
                return;
        }
        for (int i = 0; i < 3; i++) {
            sun_dir_[i] = d[i];
            sun_col_[i] = k[i];
        }
        sun_seen_ = true;
    }

    // the reflection camera, from a fogged W-buffered draw on the other target (the water reflection)
    void CaptureRefl(const State& s)
    {
        Camera(s.transforms[kTsView].data(), cam_refl_);
        refl_captured_ = true;
    }

    // The projection the vertex programs use: Evolution's view-projection columns in c167-c170 (hardware
    // order) times the view's inverse. The fixed function projection is a stand-in there (1, 1, 1, 0).
    // Gives the tangents, the W units per metre and which way view space faces; else the defaults.
    void Projection(const State& s, const float* vm)
    {
        const auto& c = s.vsc;
        float m[3][4];   // view-space columns x, y, w of the clip transform: P = V^-1 M, rows 0..2 of V^-1
        for (int col = 0, reg = 0; col < 3; col++, reg = col == 2 ? 3 : col) {
            const auto& r = c[167 + reg];
            for (int i = 0; i < 3; i++)   // V^-1's rotation is R^T: P[i][col] = sum_j R[j][i] M[j][col]
                m[col][i] = vm[i] * r[0] + vm[4 + i] * r[1] + vm[8 + i] * r[2];
            m[col][3] = r[3];
        }
        float px = m[0][0], py = m[1][1], pw = m[2][2];   // x from view x, y from view y, w from view z
        bool ok = std::abs(px) > 1 && std::abs(py) > 1 && std::abs(pw) > 1 && std::abs(m[0][1]) < 1e-3f * std::abs(px)
                  && std::abs(m[1][0]) < 1e-3f * std::abs(py) && std::abs(m[2][0]) < 1e-3f * std::abs(pw);
        if (!ok) {
            fog_tan_x_ = tan_x_, fog_tan_y_ = tan_y_, fog_zsign_ = 1, fog_wpm_frame_ = fog_wpm_;
            return;
        }
        fog_tan_x_ = std::abs(pw / px);
        fog_tan_y_ = std::abs(pw / py);
        fog_zsign_ = pw < 0 ? -1.0f : 1.0f;   // w = pw * view z: forward is -z when pw < 0
        fog_wpm_frame_ = std::abs(pw);
    }

    // a view's camera in the world: -T R^T, T the translation row, R the rotation rows
    static void Camera(const float* vm, float out[3])
    {
        for (int r = 0; r < 3; r++)
            out[r] = -(vm[12] * vm[r * 4] + vm[13] * vm[r * 4 + 1] + vm[14] * vm[r * 4 + 2]);
    }

    // the post-pass fog: one full-screen pass over the scene, composited in place with c.Feedback
    bool Fog(Context& c, Target& rt, Target& ds)
    {
        // the reference height: the option, else the water (the two cameras' mean height), else the
        // main camera's height
        float hw = refl_captured_ ? (cam_main_[2] + cam_refl_[2]) * 0.5f : cam_main_[2];
        float h0 = fog_base_ >= 0 ? fog_base_ : hw;
        // the base density: 3 optical depths over the game's fog end, in metres
        float end_m = fog_end_w_ / fog_wpm_frame_;
        float s0 = end_m > 0 ? 3.0f / end_m : 0.0f;
        // the ground layer (Far Cry 1's FogVolume as a layer over h0): 3 optical depths over its visibility
        float layer_s = fog_layer_ > 0 && fog_layer_vis_ > 0 ? 3.0f / fog_layer_vis_ : 0.0f;
        float soft = std::clamp(fog_layer_soft_, 0.1f, std::max(0.1f, fog_layer_));
        float scale = std::max(1.0f, fog_noise_scale_);
        float secs = float(GetTickCount64() % 3600000) * 0.001f;   // the noise drift; wraps hourly
        float sun = sun_seen_ ? fog_sun_ : 0.0f;
        float p[52] = {1.0f / rt.w,      1.0f / rt.h, fog_tan_x_,  fog_tan_y_,      s0,          fog_floor_,
                       fog_height_,      h0,          sun_dir_[0], sun_dir_[1], sun_dir_[2], sun,
                       fog_col_[0],      fog_col_[1], fog_col_[2], fog_wpm_frame_,    view_[0],    view_[1],
                       view_[2],         view_[3],    view_[4],    view_[5],    view_[6],    view_[7],
                       view_[8],         view_[9],    view_[10],   view_[11],   fog_thick_,  fog_sky_,
                       float(fog_mode_), hw,          fog_zsign_,  0,           0,           0,
                       layer_s,          h0 + fog_layer_, soft,    secs,        fog_noise_,  scale,
                       fog_far_,         0,           sun_col_[0], sun_col_[1], sun_col_[2], fog_tint_,
                       fog_steps_,       fog_march_,  fog_cap_,    0};
        c.Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* fb = c.Feedback(rt);
        fog_done_ = true;
        bool fresh = !ring_;
        if (!fb || !EnsureTexture(c, ring_, ring_state_, 64, 1, kBgra, 3))
            return true;
        // the sky's horizon colours: seeded with the fog colour, then a running mean of what is visible
        ID3D12Resource* fs[3] = {ds.res.Get(), fb, nullptr};
        DXGI_FORMAT ff[3] = {kDepthRead, rt.fmt, kBgra};
        c.Transition(ring_.Get(), ring_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (fresh)
            c.Pass(kFogHlsl, "ringinit", kBgra, Rtv(3), 64, 1, fs, ff, p, 52, -1);
        c.Pass(kFogHlsl, "ringp", kBgra, Rtv(3), 64, 1, fs, ff, p, 52, 0.1f);
        c.Transition(ring_.Get(), ring_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        c.Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        fs[2] = ring_.Get();
        c.Pass(kFogHlsl, "fog", rt.fmt, c.View(rt), rt.w, rt.h, fs, ff, p, 52, -1);
        c.Unbind();
        if (!fog_logged_) {
            fog_logged_ = true;
            LOGI("fci fog: end_w %.0f, thickness %.3f, colour %02X%02X%02X, h0 %.1f, camera %.1f, "
                 "water %.1f, sun %d (%.3f %.3f %.3f) colour %.3f %.3f %.3f",
                 fog_end_w_, fog_thick_, int(fog_col_[0] * 255), int(fog_col_[1] * 255),
                 int(fog_col_[2] * 255), h0, cam_main_[2], hw, int(sun_seen_), sun_dir_[0], sun_dir_[1],
                 sun_dir_[2], sun_col_[0], sun_col_[1], sun_col_[2]);
        }
        return true;
    }

    uint32_t gen_ = 0;
    int ao_mode_ = 0, ao_at_ = 0, aa_mode_ = 0, shadow_filter_ = 0;
    int fog_mode_ = 0;           // 0 off, 1 the fog, 2 the debug
    float fog_floor_ = 0.35f;    // a: the density floor
    float fog_height_ = 6.0f;    // H: the height scale, metres
    float fog_base_ = -1.0f;     // h0: the reference height, metres; -1 the water height
    float fog_sky_ = 0.2f;       // the sky fog's smoothstep knee
    float fog_wpm_ = 39475.0f;   // s: w-units per metre, when the projection is not found
    float fog_layer_ = 5.0f, fog_layer_vis_ = 40.0f, fog_layer_soft_ = 3.0f;   // the ground layer, metres
    float fog_noise_ = 0.8f, fog_noise_scale_ = 25.0f, fog_far_ = 500.0f;     // its noise; the sky ray's length
    float fog_wpm_frame_ = 39475.0f, fog_tan_x_ = 0.89f, fog_tan_y_ = 0.67f, fog_zsign_ = 1;   // this frame's
    float shadow_scale_ = 0;     // 0: the render scale
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
    float fog_gate_ = 0;           // the per-draw ext[1]: kill the game fog on this draw
    bool fog_captured_ = false;    // the fog register, colour and main view are set
    bool refl_captured_ = false;   // the reflection camera is set
    bool fog_done_ = false;        // the fog ran this frame
    bool fog_logged_ = false;
    float fog_end_w_ = 0;                     // the fog register's end, w-units
    float fog_thick_ = 0;                     // the fog register's thickness
    float fog_col_[3] = {0, 0, 0};            // the game's fog colour
    float view_[12] = {};                     // the main view rows 0..2 (world to view, metres)
    float cam_main_[3] = {0, 0, 0};           // the main camera, world metres
    float cam_refl_[3] = {0, 0, 0};           // the reflection camera, world metres
    float sun_dir_[3] = {0, 0, 1};            // toward the sun, world (Evolution's c186)
    float sun_col_[3] = {0, 0, 0};            // the sun's colour (c189)
    bool sun_seen_ = false;                   // a sun was found in some frame
    float fog_sun_ = 0.6f, fog_tint_ = 1.0f;  // the glow's strength; how much the fog takes the sky's colour
    float fog_steps_ = 16, fog_march_ = 150;  // the layer's march: steps and metres (0 steps: closed form)
    float fog_cap_ = 0;                       // 1: thickness caps the opacity, as the game
    ComPtr<ID3D12Resource> ring_;             // the horizon colour per azimuth, 64 x 1, kept across frames
    D3D12_RESOURCE_STATES ring_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    static constexpr UINT kRtvSlots = 4;      // the occlusion and its blur, the anti-aliased picture, the fog ring
};

// before the backend is made (d3d8.cpp makes it at CreateDevice): g_extension is constant initialised
const bool g_registered = [] {
    gpu::SetExtension(std::make_unique<FciExtension>());
    return true;
}();
}   // namespace
