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
#include "fci_quicksave.h"
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
constexpr uint32_t kBlendZero = 0, kBlendOne = 1;   // D3DBLEND_*, as the guest sets them
constexpr uint32_t kBlendSrcColor = 0x300, kBlendInvSrcAlpha = 0x303, kBlendDestColor = 0x306;
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
// The host notice (quick save, the fog toggle): 5x7 glyphs from fci_quicksave over a dimmed box, drawn on the
// presented picture so it shows at any render scale. p[0]: the text's top left in pixels, pixels per font
// pixel, characters; p[1].xy the size; glyph i in p[2 + i / 2] (.xy or .zw: rows 0-3, rows 4-6, 5 bits a row).
const char* kNoticeHlsl = R"(
cbuffer P : register(b1) { float4 p[17]; };
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
float4 notice(O i) : SV_Target {
    float4 c = src0.SampleLevel(ps, i.uv, 0);
    float2 q = (i.uv * p[1].xy - p[0].xy) / p[0].z;
    float n = p[0].w;
    if (q.x < -2 || q.y < -2 || q.x >= n * 6 + 1 || q.y >= 9) return c;
    float3 col = lerp(c.rgb, float3(0.05, 0.05, 0.05), 0.75);
    int ci = (int)floor(q.x / 6), gx = (int)floor(q.x) - ci * 6, gy = (int)floor(q.y);
    if (q.x >= 0 && q.y >= 0 && gy < 7 && ci < (int)n && gx < 5) {
        float4 v = p[2 + ci / 2];
        float2 g = (ci & 1) ? v.zw : v.xy;
        uint bits = (uint)(gy < 4 ? g.x : g.y), row = (uint)(gy < 4 ? gy : gy - 4);
        if ((bits >> (row * 5 + 4 - gx)) & 1) col = float3(1, 1, 1);
    }
    return float4(col, c.a);
}
)";

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
// p12 = (march steps, march metres, cap, glow blur axis); p13 = (glow, wisp strength, wisp size m, desaturate)
const char* kFogHlsl = R"(
cbuffer P : register(b1) { float4 p0; float4 p1; float4 p2; float4 p3; float4 p4; float4 p5; float4 p6; float4 p7; float4 p8; float4 p9; float4 p10; float4 p11; float4 p12; float4 p13; };
Texture2D dep  : register(t0);   // the W-buffer depth (0..1 in .r)
Texture2D colr : register(t1);   // the scene colour (the glow's blurred texture in the glow passes)
Texture2D ring : register(t2);   // the horizon colour per azimuth, 64 x 1
Texture2D glow : register(t3);   // the blurred bright parts, an eighth of the scene
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
    float x = (atan2(rd.y, rd.x) / 6.2831853 + 0.5) * 64.0 - 0.5;   // bin centres at integers
    int i0 = (int)floor(x);
    float fx = x - i0;
    float4 dc = 0, mc = 0;
    [unroll] for (int k = -7; k <= 8; k++) {   // a tent 8 bins wide, sliding with the ray: no steps
        float4 r = ring.Load(int3((i0 + k + 128) % 64, 0, 0));
        dc += float4(r.rgb * r.a, r.a) * max(0.0, 8.0 - abs(k - fx)) * 0.25;
    }
    [unroll] for (int j = 0; j < 16; j++) {
        float4 r = ring.Load(int3(j * 4, 0, 0));
        mc += float4(r.rgb * r.a, r.a);
    }
    // fades in with how much sky this direction and the ring have seen: a hard threshold made seams
    float3 ratio = clamp((dc.rgb / max(dc.a, 1e-3)) / max(mc.rgb / max(mc.a, 1e-3), 0.02), 0.7, 1.4);
    float3 tint = lerp(1.0, ratio, saturate(dc.a / 4.0) * saturate(mc.a / 2.0));
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
// wisps: fine drifting mist near the camera, world-anchored so it moves past as the player walks;
// 4 slices along the ray, skipped past the pixel's distance (sky: none skipped)
float Wisps(float3 cam, float3 rd, float dist) {
    const float ds[4] = {2.5, 5.0, 8.5, 13.0};
    const float ls[4] = {2.5, 3.0, 3.5, 4.5};
    float tau = 0;
    [unroll] for (int s = 0; s < 4; s++) {
        if (ds[s] > dist) continue;
        float3 q = cam + rd * ds[s];
        float dens = p1.x * (p1.y + (1.0 - p1.y) * exp(-(q.z - p1.w) / p1.z))
                     + p9.x * saturate((p9.y - q.z) / p9.z);
        float3 nq = q / p13.z + p9.w * float3(0.12, 0.07, 0.03);
        float n = 0.65 * VN3(nq) + 0.35 * VN3(nq * 2.13 + 5.2);
        tau += p13.y * dens * saturate((n - 0.45) * 3.0) * ls[s] * 3.0;
    }
    return tau;
}
float LayerMean(float3 a, float3 b) {   // the closed form with the mean density
    float dh = b.z - a.z;
    float avg = abs(dh) < 1e-3 ? saturate((p9.y - a.z) / p9.z) : (G(b.z) - G(a.z)) / dh;
    return p9.x * length(b - a) * avg;
}
// glowdown: the scene's bright parts into an eighth: 4 bilinear taps at the 8 x 8 block's quadrant
// centres, each weighted by how bright it is, so only the bright sky and the sun get through
float4 glowdown(O i) : SV_Target {
    float2 c = i.uv;   // the block centre: the same normalised position in the scene
    float2 o = 2.0 * p0.xy;
    float3 acc = 0;
    [unroll] for (int j = 0; j < 4; j++) {
        float2 uv = c + float2((j & 1) * 2.0 - 1.0, (j >> 1) * 2.0 - 1.0) * o;
        float3 col = colr.SampleLevel(sr, uv, 0).rgb;
        acc += col * smoothstep(0.55, 1.0, dot(col, float3(0.299, 0.587, 0.114)));
    }
    return float4(acc * 0.25, 1.0);   // the mean over the block: a dim tap adds little
}
// glowblur: a 9-tap Gaussian, sigma 2.5 texels, across the glow; p12.w 0: across, 1: down
float4 glowblur(O i) : SV_Target {
    float2 st = (p12.w > 0.5 ? float2(1.0, 0.0) : float2(0.0, 1.0)) * 8.0 * p0.xy;   // one glow texel
    float3 acc = 0;
    float wsum = 0;
    [unroll] for (int k = -4; k <= 4; k++) {
        float wgt = exp(-k * k * 0.08);
        acc += colr.SampleLevel(sr, i.uv + k * st, 0).rgb * wgt;
        wsum += wgt;
    }
    return float4(acc / wsum, 1.0);
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
    if (p7.z > 1.5 && p7.z < 2.5) {   // the debug (mode 2; mode 3 is the volume)
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
        float fl = Amount(Layer(cam, cam + rd * p10.z, jit) + Wisps(cam, rd, p10.z));
        float amount = 1.0 - (1.0 - f) * (1.0 - fl);
        fc += glow.SampleLevel(sr, uv, 0).rgb * p13.x * amount;   // bright sky and the sun, scattered in the fog
        return float4(lerp(c.rgb, fc, amount), c.a);
    }
    float3 Pvm = dir * (d * 16777215.0 / p3.w);   // metres
    float3 Pw = World(Pvm);
    // the height fog's optical depth, integrated along the ray in closed form
    float k = (Pw.z - cam.z) / p1.z;
    float g = abs(k) < 1e-4 ? eu : eu * (1.0 - exp(-k)) / k;
    float tau = p1.x * length(Pvm) * (p1.y + (1.0 - p1.y) * g) + Layer(cam, Pw, jit) + Wisps(cam, rd, length(Pvm));
    float amount = Amount(tau);
    c.rgb = lerp(c.rgb, dot(c.rgb, float3(0.299, 0.587, 0.114)).xxx, p13.w * amount);   // far: no colour left
    fc += glow.SampleLevel(sr, uv, 0).rgb * p13.x * amount;
    return float4(lerp(c.rgb, fc, amount), c.a);
}
)";

// Volume fog (RECOMP_FCI_XBR_FOG=3): a 128x72x64 froxel volume. "inject" writes one slice's (L, sigma)
// with temporal accumulation; "integ" integrates the accumulated volume; "volfog" composites it.
// Cbuffer layout (register b1, float4 p[24]):
//   inject:  p[0]=(tx,ty,tanx,tany) p[1..3]=view rows p[4]=(s0,thick,wpm,zsign) p[5]=(sdir,sstr)
//            p[6]=(scol,tint) p[7]=(dk,dxk,volw,volh) p[8]=(vold,time,near,far)
//            p[9]=(sh_gw,sh_gh,sh_hw,sh_hh) p[10..13]=shadow mtx p[14..17]=last view rows
//            p[18]=(last_tanx,last_tany,last_zsign,have_hist) p[19]=(h0,H,floor,ceiling)
//            p[20]=(layer_s,top,soft,0) p[21]=(nscale,noise,glow,0) p[22]=(fcol,0)
//            p[23]=(frame, jitter xyz)
//   integ:   p[0]=(k,near,step,0)
//   volfog:  p[0]=(tx,ty,tanx,tany) p[1..3]=view rows p[4]=(wpm,0,0,0) p[7]=(near,far,vold,0)
//            p[8]=(glow_w,sky only,0,desat) p[12]=(zsign,0,0,0)
//   aoapply: p[0]=(ao mode,0,0,0) p[4]=(wpm,0,0,0) p[7]=(near,far,vold,0)
const char* kVolHlsl = R"(
cbuffer P : register(b1) { float4 p[24]; };
Texture2D<float> t0 : register(t0);    // shadow map (inject) / depth (volfog)
Texture3D<float4> vol3 : register(t1);  // volume (inject: history; integ: current; volfog: integrated)
Texture2D<float4> t2 : register(t2);    // ring (inject) / scene colour (volfog)
Texture2D<float4> t3 : register(t3);    // glow (volfog)
SamplerState smp : register(s0);
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
// World from view: R rows p[1..3].xyz, translation p[1..3].w
float3 World(float3 Pvm) {
    float3 dv = Pvm - float3(p[1].w, p[2].w, p[3].w);
    return float3(dot(dv, p[1].xyz), dot(dv, p[2].xyz), dot(dv, p[3].xyz));
}
// World to last frame's view, the inverse of World(): Pv' = R'^T * Pw + t'
float3 ToViewLast(float3 Pw) {
    return float3(Pw.x * p[14].x + Pw.y * p[15].x + Pw.z * p[16].x + p[14].w,
                  Pw.x * p[14].y + Pw.y * p[15].y + Pw.z * p[16].y + p[15].w,
                  Pw.x * p[14].z + Pw.y * p[15].z + Pw.z * p[16].z + p[16].w);
}
float H3(float3 q) { return frac(sin(dot(q, float3(127.1, 311.7, 74.7))) * 43758.5453); }
float VN3(float3 x) {
    float3 i = floor(x), f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(H3(i), H3(i + float3(1,0,0)), f.x), lerp(H3(i + float3(0,1,0)), H3(i + float3(1,1,0)), f.x), f.y),
                lerp(lerp(H3(i + float3(0,0,1)), H3(i + float3(1,0,1)), f.x), lerp(H3(i + float3(0,1,1)), H3(i + 1.0), f.x), f.y), f.z);
}
float Mist(float3 q) {
    // a slow drift (a quarter of mode 1's): faster mist read as a river against the 90% history
    float3 n = q / float3(p[21].x, p[21].x, p[21].x * 0.4) + p[8].y * float3(0.008, 0.004, 0.001);
    float v = 0.65 * VN3(n) + 0.35 * VN3(n * 2.13 + 5.2);
    return lerp(1.0, 2.0 * saturate(0.5 + (v - 0.5) * 2.5), p[21].y);
}
// the layer's profile integrated over height (as mode 1's G): 1 below top - soft, a linear fade to 0 at top
float GL(float z) {
    float b = p[20].y - p[20].z, a = clamp(z - b, 0.0, p[20].z);
    return min(z, b) + a - a * a / (2.0 * p[20].z);
}
// The slice's mean density between a and b in closed form, so a thick far slice is not one noisy point
// sample; the layer's noise at q (a jittered point) only modulates it, less where the slice is thick
float Sigma(float3 a, float3 b, float3 q, float len) {
    float h = p[19].x, H = p[19].y, fl = p[19].z;
    // below h0 the density stops growing at 2 scale heights: froxels under the terrain overflowed half floats
    float za = max(a.z, h - 2.0 * H), zb = max(b.z, h - 2.0 * H), dz = zb - za;
    float e = abs(dz) < 1e-3 ? exp(-(za - h) / H) : H * (exp(-(za - h) / H) - exp(-(zb - h) / H)) / dz;
    // the floor (distance fog) ends at the ceiling p[19].w, so a sky ray leaves it: the sky above stays clear
    float lo = min(a.z, b.z), hi = max(a.z, b.z);
    float below = hi - lo < 1e-3 ? (lo < p[19].w ? 1.0 : 0.0) : saturate((p[19].w - lo) / (hi - lo));
    float s = p[4].x * (fl * below + (1.0 - fl) * e);
    if (p[20].x > 0.0) {
        float lz = b.z - a.z;
        float lay = abs(lz) < 1e-3 ? saturate((p[20].y - a.z) / p[20].z) : (GL(b.z) - GL(a.z)) / lz;
        s += p[20].x * lay * lerp(1.0, Mist(q), saturate(p[21].x / (2.0 * len)));
    }
    return p[4].y * s;
}
// the sun's forward lobe, as the post pass's glow, so modes 1 and 3 agree where nothing is shadowed
float Lobe(float mu) { mu = saturate(mu); return 0.6 * pow(mu, 8.0) + 0.4 * pow(mu, 64.0); }
// the fog's colour along a world ray: the game's fog colour tinted by the sky's horizon per azimuth
float3 FogCol(float3 rd) {
    float x = (atan2(rd.y, rd.x) / 6.2831853 + 0.5) * 64.0 - 0.5;
    int i0 = (int)floor(x);
    float fx = x - i0;
    float4 dc = 0, mc = 0;
    [unroll] for (int k = -7; k <= 8; k++) {
        float4 r = t2.Load(int3((i0 + k + 128) % 64, 0, 0));
        dc += float4(r.rgb * r.a, r.a) * max(0.0, 8.0 - abs(k - fx)) * 0.25;
    }
    [unroll] for (int j = 0; j < 16; j++) {
        float4 r = t2.Load(int3(j * 4, 0, 0));
        mc += float4(r.rgb * r.a, r.a);
    }
    float3 ratio = clamp((dc.rgb / max(dc.a, 1e-3)) / max(mc.rgb / max(mc.a, 1e-3), 0.02), 0.7, 1.4);
    float3 tint = lerp(1.0, ratio, saturate(dc.a / 4.0) * saturate(mc.a / 2.0));
    float3 col = p[22].xyz * lerp(1.0, tint, p[6].w * (1.0 - smoothstep(0.25, 0.7, abs(rd.z))));
    return col;   // the sun's lobe is added per froxel, shadowed
}
// shadow: 2x2 Load-and-compare, the game's convention (uv = tc/size - 0.5/host, depth = tc.z/65535)
float Shad(float2 tc_xy, float tc_z) {
    float2 suv = tc_xy / p[9].xy - 0.5 / p[9].zw;
    if (suv.x < 0 || suv.x > 1 || suv.y < 0 || suv.y > 1) return 1.0;
    float sz = clamp(tc_z, 0.0, 65535.0) / 65535.0;
    int2 base = (int2)floor(suv * p[9].zw);
    float sum = 0;
    [unroll] for (int y = 0; y < 2; y++)
        [unroll] for (int x = 0; x < 2; x++)
            sum += (t0.Load(int3(base + int2(x, y) - 1, 0)).r <= sz) ? 1.0 : 0.0;
    return sum * 0.25;
}
float4 inject(O i) : SV_Target {
    // one sub-froxel offset for all froxels per frame (Halton 2, 3, 5 over 16 frames), so the history
    // averages the froxel's whole cell; the centre while there is no history to average into
    float3 jit = p[18].w > 0.5 ? p[23].yzw : 0.5;
    float2 uv = (floor(i.uv * p[7].zw) + jit.xy) / p[7].zw;
    float d = p[7].x + jit.z * p[7].y;
    float2 ndc = uv * float2(2, -2) + float2(-1, 1);
    float3 dir = float3(ndc * p[0].zw, p[4].w);   // dir.z = zsign: view forward
    float3 Pw = World(dir * d);
    float3 cam = World(float3(0,0,0));
    float3 rd = normalize(Pw - cam);
    float sig = Sigma(World(dir * p[7].x), World(dir * (p[7].x + p[7].y)), Pw, p[7].y * length(dir));
    // the bright sky and sun scattered in the fog, as mode 1 adds it (last frame's glow, same screen uv)
    float3 fc = FogCol(rd) + t3.SampleLevel(smp, uv, 0).rgb * p[21].z;
    float mu = dot(rd, p[5].xyz);
    float4 s4 = float4(dot(p[10].xyz, Pw) + p[10].w, dot(p[11].xyz, Pw) + p[11].w,
                       dot(p[12].xyz, Pw) + p[12].w, dot(p[13].xyz, Pw) + p[13].w);
    // the map covers only the player's surroundings: fade its shadow out over the outer tenth
    float2 sx = s4.xy / p[9].xy;
    float edge = saturate(min(min(sx.x, 1.0 - sx.x), min(sx.y, 1.0 - sx.y)) * 10.0);
    float sun = (s4.w > 0 && edge > 0) ? lerp(1.0, Shad(s4.xy, s4.z), edge) : 1.0;
    float3 L = sig * (fc + p[6].xyz * p[5].w * sun * Lobe(mu));
    float4 cur = float4(L, sig);
    // temporal: reproject through last frame's view (world -> last view space -> NDC -> uv, depth)
    float3 lPv = ToViewLast(Pw);
    float2 lndc = lPv.xy / (lPv.z * p[18].z * float2(p[18].x, p[18].y));
    float2 luv = lndc * float2(0.5, -0.5) + 0.5;
    float ld = lPv.z * p[18].z;   // view depth in metres
    float lkf = p[8].x * log(max(ld, p[8].z) / p[8].z) / log(p[8].w / p[8].z);
    bool ok = p[18].w > 0.5 && ld > 0 && luv.x >= 0 && luv.x <= 1 && luv.y >= 0 && luv.y <= 1 && lkf >= 0 && lkf <= p[8].x;
    float4 hist = ok ? vol3.Sample(smp, float3(luv, lkf / p[8].x)) : cur;
    return lerp(hist, cur, 0.1);
}
float4 integ(O i) : SV_Target {
    float2 uv = i.uv;
    int k = (int)p[0].x;
    float near_m = p[0].y, step = p[0].z;
    float3 S = 0;
    float T = 1.0;
    [loop] for (int j = 0; j <= k; j++) {
        // four taps half a texel out: a 1-2-1 tent across, so froxel edges do not show as blocks
        float w = (float)(j + 0.5) / 64.0;
        float2 o = 0.5 / float2(128.0, 72.0);
        float4 v = 0.25 * (vol3.SampleLevel(smp, float3(uv + o, w), 0) + vol3.SampleLevel(smp, float3(uv - o, w), 0)
                           + vol3.SampleLevel(smp, float3(uv + float2(o.x, -o.y), w), 0)
                           + vol3.SampleLevel(smp, float3(uv + float2(-o.x, o.y), w), 0));
        if (any(!isfinite(v))) v = 0;   // one bad froxel must not poison the rest of the column
        float sig = v.a;
        float dx = near_m * pow(step, (float)j) * (step - 1.0);
        float t = exp(-sig * dx);
        S += (sig > 1e-8 ? v.rgb / sig * (1.0 - t) : v.rgb * dx) * T;   // the second: the limit as sigma -> 0
        T *= t;
    }
    return float4(S, T);
}
// the slice position of a view depth in metres
float VolK(float dm) { return p[7].z * log(max(dm, p[7].x) / p[7].x) / log(p[7].y / p[7].x); }
// the AO after in-shader fog: occlude the surface's share only, out = S + (c - S) * ao, so the fog's own
// light is not darkened and fogged creases do not show as dark seams
float4 aoapply(O i) : SV_Target {
    float2 uv = i.uv;
    float4 c = t2.SampleLevel(smp, uv, 0);
    float a = t3.SampleLevel(smp, uv, 0).r;
    if (p[0].x > 1.5 && p[0].x < 2.5) return float4(a, a, a, c.a);
    if (p[0].x > 2.5 && uv.x < 0.5) return c;
    float d = t0.SampleLevel(smp, uv, 0).r;
    if (d <= 0.0 || d >= 0.9999) return c;
    float w = (VolK(d * 16777215.0 / p[4].x) - 0.5) / p[7].z;
    float3 S = vol3.SampleLevel(smp, float3(uv, saturate(w)), 0).rgb;
    return float4(S + max(c.rgb - S, 0.0) * a, c.a);
}
float4 volfog(O i) : SV_Target {
    float2 uv = i.uv;
    float d = t0.SampleLevel(smp, uv, 0).r;
    float4 c = t2.SampleLevel(smp, uv, 0);
    bool sky = d <= 0.0 || d >= 0.9999;
    if (!sky && p[8].y > 0.5) return c;   // sky only: the surfaces fogged themselves
    // the sky takes the volume's far end, as geometry there does: far shapes melt into the sky behind them
    float w = sky ? 1.0 : (VolK(d * 16777215.0 / p[4].x) - 0.5) / p[7].z;
    float4 v = vol3.SampleLevel(smp, float3(uv, saturate(w)), 0);
    if (sky) return float4(c.rgb * v.a + v.rgb, c.a);
    // the surface loses colour with distance, then the fog's light is added: the fog keeps its own colour
    float3 cs = lerp(c.rgb, dot(c.rgb, float3(0.299, 0.587, 0.114)).xxx, p[8].w * (1.0 - v.a));
    return float4(cs * v.a + v.rgb, c.a);
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
        fog_mode_ = fog_mode_ < 0 ? 0 : (fog_mode_ > 3 ? 3 : fog_mode_);
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
        fog_glow_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_GLOW", 0.5f);
        fog_wisp_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_WISPS", 0.6f);
        fog_wisp_size_ = std::max(0.5f, fci_gfx::Number("RECOMP_FCI_XBR_FOG_WISP_SIZE", 4.0f));
        fog_desat_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_DESAT", 0.35f);
        fog_ceil_ = std::max(1.0f, fci_gfx::Number("RECOMP_FCI_XBR_FOG_CEIL", 150.0f));
        fog_lookup_ = fci_gfx::Number("RECOMP_FCI_XBR_FOG_LOOKUP", 1) > 0.5f;   // 1: in-shader, 0: post-pass
        return remake;   // the shadow map is made again at its new size
    }

    void Reset(Context&) override
    {
        for (auto& r : ao_)
            r.Reset();
        post_.Reset();
        notice_.Reset();
        scene_rt_ = scene_ds_ = nullptr;
        refl_rt_ = nullptr;
        done_ = fog_done_ = false;
        fog_captured_ = refl_captured_ = false;
        fog_gate_ = 0;
    }

    // the sun shadow map: a square depth target drawn with a colour target of its size (1024 x 1024 in
    // Training); it takes the larger render scale so it stays square, other targets keep the render scale.
    // A whole number: the game moves the map in whole texels, which a fractional scale makes edges crawl.
    void MakeTarget(const gpu::Surface&, bool, uint32_t gw, uint32_t gh, float& sx, float& sy,
                    DXGI_FORMAT&) override
    {
        if (gw != gh || gw < kShadowMin)
            return;
        sx = sy = std::max(1.0f, std::round(shadow_scale_ > 0 ? shadow_scale_ : std::max(sx, sy)));
    }

    const gpu::ShaderHooks* Shaders() const override { return &hooks_; }

    void Decoded(const gpu::TextureDesc& d, const gpu::FormatInfo& fi, uint32_t, uint32_t level,
                 std::vector<uint8_t>& px, uint32_t pitch, uint8_t&) override
    { fci_glyphs::Patch(d, fi, level, px, pitch); }

    void Constants(float (*)[4], float* ext, float* vol, float* volsize, const Target* rt, uint32_t src,
                   uint32_t dst) override
    {
        ext[0] = float(shadow_filter_);
        ext[1] = fog_gate_;   // the per-draw fog kill: the scene weight is 1 (no game fog) on gated draws
        ext[2] = ext[3] = 0;
        if (fog_gate_ > 0 && fog_mode_ == 3 && fog_lookup_ && vol_ready_ && !vol_splitscreen_) {
            // each draw is fogged before it blends with the fogged colour behind it, so what it adds
            // depends on the blend (docs/rendering.md): 1 col*T + S, 2 col*T (dest ONE), 3 col*T + a*S
            // (premultiplied), 4 and 5 multiply blends fade to their neutral (1, 0.5 for 2x)
            ext[2] = dst == kBlendOne                                        ? 2.0f
                     : src == kBlendOne && dst == kBlendInvSrcAlpha          ? 3.0f
                     : src == kBlendDestColor && dst == kBlendSrcColor       ? 5.0f
                     : (src == kBlendDestColor && dst == kBlendZero)
                             || (src == kBlendZero && dst == kBlendSrcColor) ? 4.0f
                                                                             : 1.0f;
            ext[3] = fog_desat_;
            vol[0] = kNearM;
            vol[1] = 1.0f / logf(kFarM / kNearM);
            vol[2] = float(kVZ);
            vol[3] = 1.0f / fog_wpm_frame_;
            if (rt) {
                volsize[0] = float(rt->w);
                volsize[1] = float(rt->h);
            }
        } else if (fog_mode_ == 3 && rt && rt == refl_rt_ && fog_end_w_ > 0) {
            // the reflection: a uniform fog at the density near the water, the sky taken as kFarM away
            float s0 = 3.0f * fog_wpm_frame_ / fog_end_w_;
            float layer = fog_layer_ > 0 && fog_layer_vis_ > 0 ? 3.0f / fog_layer_vis_ : 0.0f;
            ext[1] = 1.0f;   // the game's own fog off: this replaces it
            ext[2] = dst == kBlendOne ? 7.0f : 6.0f;
            vol[0] = fog_thick_ * (s0 + 0.5f * layer);
            vol[1] = kFarM;
            vol[3] = 1.0f / (refl_wpm_ > 0 ? refl_wpm_ : fog_wpm_frame_);
            volsize[0] = fog_col_[0];
            volsize[1] = fog_col_[1];
            volsize[2] = fog_col_[2];
        }
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
            vol_ready_ = false;
            vol_splitscreen_ = false;
        }
        if (!rt)
            return false;
        uint32_t gw, gh;
        c.GuestSize(gw, gh);
        bool world = ds && !pretransformed && s.rs[kRsZEnable] == 2;   // W-buffered: FCI's world
        bool main = rt->gw == gw && rt->gh == gh && world && ds->w == rt->w && ds->h == rt->h;
        // kill the game fog on the draws our pass covers; in mode 3 the volume lookup covers all main draws
        // once built (splitscreen: post-pass route, gate until the trigger as in mode 1)
        // mode 3 also fogs fog-off draws of the game's vertex programs (the water popped out of the fog);
        // fixed function fog-off draws are the editor's cursor and lines, kept clear
        bool fog_on = s.rs[kRsFogEnable] != 0;
        bool vol = fog_mode_ == 3 && !vol_splitscreen_ && (fog_on || (s.vs_handle & 1));
        fog_gate_ = main && (vol ? vol_ready_ : fog_mode_ && fog_on && !fog_done_) ? 1.0f : 0.0f;
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
                if (fog_mode_) {
                    CaptureSun(s);
                    // the shadow matrix (c161-c164), world to shadow clip
                    for (int i = 0; i < 4; i++)
                        for (int j = 0; j < 4; j++)
                            shadow_mtx_[i * 4 + j] = s.vsc[161 + i][j];
                }
                // mode 3 with in-shader lookup: build the volume on the first main fog-enabled draw;
                // splitscreen (viewport not the whole target) falls back to the post-pass route
                if (fog_mode_ == 3 && fog_lookup_ && s.rs[kRsFogEnable] != 0 && !vol_ready_) {
                    // the viewport is in guest pixels: a splitscreen window is smaller than the target
                    bool splitscreen = s.viewport.x != 0 || s.viewport.y != 0 || s.viewport.w != gw
                                       || s.viewport.h != gh;
                    if (splitscreen) {
                        vol_splitscreen_ = true;
                        if (!vol_glow_logged_) {
                            vol_glow_logged_ = true;
                            LOGI("fci volfog: splitscreen; in-shader lookup off, post-pass route");
                        }
                    } else {
                        WaterHeight();
                        Volume(c, s);
                        vol_ready_ = true;
                        fog_gate_ = 1.0f;   // this draw too: it was gated before the volume existed
                        if (!vol_glow_logged_) {
                            vol_glow_logged_ = true;
                            LOGI("fci volfog: in-shader lookup");
                        }
                        return true;   // rebind: the draw proceeds with the volume lookup
                    }
                }
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
        if (fog_mode_ && !refl_captured_ && world && s.rs[kRsFogEnable] != 0) {
            CaptureRefl(s);
            refl_rt_ = rt;   // kept across frames: its sky is drawn before its first W-buffered draw
            if (fog_mode_ == 3 && !refl_logged_) {
                refl_logged_ = true;
                LOGI("fci volfog: reflection %ux%u, w per metre %.0f", rt->w, rt->h, refl_wpm_);
            }
        }
        // the world is done: the first non-main draw after the opaque world; the AO (ao_at_ 0) and the
        // fog run here, on the world scene target, the fog after the AO
        if (opaque_ >= kOpaqueDraws && scene_rt_) {
            if (ao_mode_ && !done_ && ao_at_ != 1) {
                done_ = true;
                Occlude(c, *scene_rt_, *scene_ds_);
            }
            if (fog_mode_ && !fog_done_ && fog_captured_) {
                if (fog_mode_ == 3) {
                    // in-shader lookup: the volume was built on the first main draw, the sky is fogged
                    // here; post-pass lookup (RECOMP_FCI_XBR_FOG_LOOKUP=0): build it now, fog everything
                    if (!vol_ready_) {
                        WaterHeight();
                        Volume(c, s);
                    }
                    return Fog(c, *scene_rt_, *scene_ds_, true);
                }
                return Fog(c, *scene_rt_, *scene_ds_);
            }
        }
        return false;
    }

    ID3D12Resource* Present(Context& c, Target& t, DXGI_FORMAT& fmt) override
    {
        ID3D12Resource* shown = Antialias(c, t, fmt);
        std::string msg = fci_quicksave::Notice();
        if (msg.empty() || t.depth || !EnsureTexture(c, notice_, notice_state_, t.w, t.h, t.fmt, kNoticeSlot))
            return shown;
        if (!shown)
            c.Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        c.Transition(notice_.Get(), notice_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        size_t n = std::min<size_t>(msg.size(), 30);
        float p[68] = {};
        float cell = std::max(2.0f, std::floor(t.h / 240.0f));   // 2 screen pixels per font pixel at 480 lines
        p[0] = std::floor(t.w * 0.5f - (n * 6 - 1) * cell * 0.5f);
        p[1] = std::floor(t.h / 5.0f);   // below the compass
        p[2] = cell;
        p[3] = float(n);
        p[4] = float(t.w);
        p[5] = float(t.h);
        for (size_t i = 0; i < n; i++) {
            uint8_t r[7] = {};
            fci_quicksave::GlyphRows(msg[i], r);   // a character the font lacks stays blank
            uint32_t lo = 0, hi = 0;
            for (int k = 0; k < 4; k++)
                lo |= uint32_t(r[k] & 31) << (k * 5);
            for (int k = 0; k < 3; k++)
                hi |= uint32_t(r[4 + k] & 31) << (k * 5);
            p[8 + i * 2] = float(lo);   // 20 and 15 bits: exact in a float
            p[9 + i * 2] = float(hi);
        }
        ID3D12Resource* src[4] = {shown ? shown : t.res.Get(), nullptr, nullptr, nullptr};
        DXGI_FORMAT sf[4] = {t.fmt, kBgra, kBgra, kBgra};
        c.Pass(kNoticeHlsl, "notice", t.fmt, Rtv(kNoticeSlot), t.w, t.h, src, sf, p, 8 + n * 2, -1);
        c.Unbind();
        Target out;   // F3 / RECOMP_FCI_XBR_CAPTURE: the picture as shown, as the anti-aliasing saves it
        out.res = notice_;
        out.w = out.gw = t.w;
        out.h = out.gh = t.h;
        out.fmt = t.fmt;
        out.state = notice_state_;
        c.Capture(out, "shown", 1);
        notice_state_ = out.state;
        c.Transition(notice_.Get(), notice_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        fmt = t.fmt;
        return notice_.Get();
    }

    // edge anti-aliasing into post_ (left in the pixel shader resource state), or null when off
    ID3D12Resource* Antialias(Context& c, Target& t, DXGI_FORMAT& fmt)
    {
        if (!aa_mode_ || t.depth || !EnsureTexture(c, post_, post_state_, t.w, t.h, t.fmt, 2))
            return nullptr;
        c.Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        c.Transition(post_.Get(), post_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12Resource* src[4] = {t.res.Get(), nullptr, nullptr, nullptr};
        DXGI_FORMAT sf[4] = {t.fmt, kBgra, kBgra, kBgra};
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

    ID3D12Resource* Volume() override
    {
        if (fog_mode_ == 3 && fog_lookup_ && vol_ready_ && !vol_splitscreen_ && vol_int_
            && vol_int_state_ == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            return vol_int_.Get();
        return nullptr;
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
        ID3D12Resource* src[4] = {ds.res.Get(), nullptr, nullptr, nullptr};
        DXGI_FORMAT fmt[4] = {kDepthRead, kBgra, kBgra, kBgra};
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
            ID3D12Resource* bs[4] = {ao_[from].Get(), nullptr, nullptr, nullptr};
            DXGI_FORMAT bf[4] = {kAoFormat, kBgra, kBgra, kBgra};
            float bp[8] = {axis == 0 ? 1.0f / w : 0.0f, axis == 1 ? 1.0f / h : 0.0f};
            c.Pass(kAoHlsl, "blur", kAoFormat, Rtv(to), w, h, bs, bf, bp, 8, -1);
        }
        c.Transition(ao_[0].Get(), ao_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* fb = c.Feedback(rt);
        if (!fb)
            return true;
        c.Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (Volume()) {   // mode 3's surfaces are already fogged: occlude their share, not the fog's light
            ID3D12Resource* vs[4] = {ds.res.Get(), vol_int_.Get(), fb, ao_[0].Get()};
            DXGI_FORMAT vf[4] = {kDepthRead, DXGI_FORMAT_R16G16B16A16_FLOAT, rt.fmt, kAoFormat};
            float vp[32] = {float(ao_mode_)};
            vp[16] = fog_wpm_frame_;
            vp[28] = kNearM;
            vp[29] = kFarM;
            vp[30] = float(kVZ);
            c.Pass(kVolHlsl, "aoapply", rt.fmt, c.View(rt), w, h, vs, vf, vp, 32, -1);
            c.Unbind();
            return true;
        }
        ID3D12Resource* as[4] = {fb, ao_[0].Get(), nullptr, nullptr};
        DXGI_FORMAT af[4] = {rt.fmt, kAoFormat, kBgra, kBgra};
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
        const float* vm = s.transforms[kTsView].data();
        Camera(vm, cam_refl_);
        refl_captured_ = true;
        // its own w units per metre (Projection()'s pw), for mode 3's reflection fog
        const auto& r = s.vsc[170];
        float pw = std::abs(vm[2] * r[0] + vm[6] * r[1] + vm[10] * r[2]);
        refl_wpm_ = pw > 1 ? pw : 0;
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
        bool ok = std::abs(px) > 1 && std::abs(py) > 1 && std::abs(pw) > 1
                  && std::abs(m[0][1]) < 1e-3f * std::abs(px) && std::abs(m[1][0]) < 1e-3f * std::abs(py)
                  && std::abs(m[2][0]) < 1e-3f * std::abs(pw);
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

    // the water height: the two cameras' mean, held when the reflection is skipped
    void WaterHeight()
    {
        if (refl_captured_)
            water_h_ = (cam_main_[2] + cam_refl_[2]) * 0.5f, water_seen_ = true;
        else if (!water_seen_)
            water_h_ = cam_main_[2], water_seen_ = true;
    }

    // the post-pass fog: one full-screen pass over the scene, composited in place with c.Feedback
    // vol: mode 3, the ring and glow as here, then the volume's composite (Volfog) in place of the fog
    bool Fog(Context& c, Target& rt, Target& ds, bool vol = false)
    {
        WaterHeight();
        float hw = water_h_;
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
        float p[56] = {1.0f / rt.w,
                       1.0f / rt.h,
                       fog_tan_x_,
                       fog_tan_y_,
                       s0,
                       fog_floor_,
                       fog_height_,
                       h0,
                       sun_dir_[0],
                       sun_dir_[1],
                       sun_dir_[2],
                       sun,
                       fog_col_[0],
                       fog_col_[1],
                       fog_col_[2],
                       fog_wpm_frame_,
                       view_[0],
                       view_[1],
                       view_[2],
                       view_[3],
                       view_[4],
                       view_[5],
                       view_[6],
                       view_[7],
                       view_[8],
                       view_[9],
                       view_[10],
                       view_[11],
                       fog_thick_,
                       fog_sky_,
                       float(fog_mode_),
                       hw,
                       fog_zsign_,
                       0,
                       0,
                       0,
                       layer_s,
                       h0 + fog_layer_,
                       soft,
                       secs,
                       fog_noise_,
                       scale,
                       fog_far_,
                       0,
                       sun_col_[0],
                       sun_col_[1],
                       sun_col_[2],
                       fog_tint_,
                       fog_steps_,
                       fog_march_,
                       fog_cap_,
                       0,
                       fog_glow_,
                       fog_wisp_,
                       fog_wisp_size_,
                       fog_desat_};
        c.Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* fb = c.Feedback(rt);
        fog_done_ = true;
        bool fresh = !ring_;
        if (!fb || !EnsureTexture(c, ring_, ring_state_, 64, 1, kBgra, 3))
            return true;
        uint32_t gw = (rt.w + 7) / 8, gh = (rt.h + 7) / 8;   // the glow, an eighth of the scene
        if (fog_glow_ > 0
            && (!EnsureTexture(c, glow_[0], glow_state_[0], gw, gh, kBgra, 4)
                || !EnsureTexture(c, glow_[1], glow_state_[1], gw, gh, kBgra, 5)))
            return true;
        // the sky's horizon colours: seeded with the fog colour, then a running mean of what is visible
        ID3D12Resource* fs[4] = {ds.res.Get(), fb, nullptr, nullptr};
        DXGI_FORMAT ff[4] = {kDepthRead, rt.fmt, kBgra, kBgra};
        c.Transition(ring_.Get(), ring_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (fresh)
            c.Pass(kFogHlsl, "ringinit", kBgra, Rtv(3), 64, 1, fs, ff, p, 56, -1);
        c.Pass(kFogHlsl, "ringp", kBgra, Rtv(3), 64, 1, fs, ff, p, 56, 0.1f);
        c.Transition(ring_.Get(), ring_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (fog_glow_ > 0) {   // the bright parts, down an eighth and blurred: the light in the fog
            ID3D12Resource* gs[4] = {nullptr, fb, nullptr, nullptr};
            DXGI_FORMAT gf[4] = {kBgra, rt.fmt, kBgra, kBgra};
            c.Transition(glow_[0].Get(), glow_state_[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
            c.Pass(kFogHlsl, "glowdown", kBgra, Rtv(4), gw, gh, gs, gf, p, 56, -1);
            for (int axis = 0; axis < 2; axis++) {
                int from = axis, to = 1 - axis;
                p[48 + 3] = float(axis);   // p12.w: across, then down
                c.Transition(glow_[from].Get(), glow_state_[from],
                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                c.Transition(glow_[to].Get(), glow_state_[to], D3D12_RESOURCE_STATE_RENDER_TARGET);
                ID3D12Resource* bs[4] = {nullptr, glow_[from].Get(), nullptr, nullptr};
                DXGI_FORMAT bf[4] = {kBgra, kBgra, kBgra, kBgra};
                c.Pass(kFogHlsl, "glowblur", kBgra, Rtv(to), gw, gh, bs, bf, p, 56, -1);
            }
            c.Transition(glow_[0].Get(), glow_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        if (vol)
            return Volfog(c, rt, ds, vol_ready_ && fog_lookup_);
        c.Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        fs[2] = ring_.Get();
        if (fog_glow_ > 0)
            fs[3] = glow_[0].Get();
        c.Pass(kFogHlsl, "fog", rt.fmt, c.View(rt), rt.w, rt.h, fs, ff, p, 56, -1);
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

    bool EnsureVolumes(Context& c)
    {
        if (vols_ready_)
            return true;
        if (!EnsureHeap(c))
            return false;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        rd.Width = kVX;
        rd.Height = kVY;
        rd.DepthOrArraySize = kVZ;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;   // each slice is drawn through its RTV
        for (int i = 0; i < 3; i++) {
            ComPtr<ID3D12Resource>& v = i == 0 ? vol_a_ : (i == 1 ? vol_b_ : vol_int_);
            if (FAILED(c.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                           D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                           IID_PPV_ARGS(&v)))) {
                LOGE("fci volfog: the volume could not be made; mode 3 stays off");
                fog_mode_ = 1;
                return false;
            }
            for (int k = 0; k < kVZ; k++) {
                D3D12_RENDER_TARGET_VIEW_DESC rv{};
                rv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
                rv.Texture3D.MipSlice = 0;
                rv.Texture3D.FirstWSlice = k;
                rv.Texture3D.WSize = 1;
                c.Device()->CreateRenderTargetView(v.Get(), &rv, Rtv(6 + i * kVZ + k));
            }
        }
        vol_a_state_ = D3D12_RESOURCE_STATE_COMMON;
        vol_b_state_ = D3D12_RESOURCE_STATE_COMMON;
        vol_int_state_ = D3D12_RESOURCE_STATE_COMMON;
        vols_ready_ = true;
        return true;
    }

    void Volume(Context& c, const State& s)
    {
        if (!EnsureVolumes(c))
            return;
        bool shadow = shadow_target_ && frame_ - shadow_frame_ <= 1;
        // which accumulated volume this frame: ping-pong A/B
        bool use_a = (vol_frame_ >= 0 && vol_frame_ % 2 == 0);
        ComPtr<ID3D12Resource>& cur = use_a ? vol_a_ : vol_b_;
        ComPtr<ID3D12Resource>& hist = use_a ? vol_b_ : vol_a_;
        D3D12_RESOURCE_STATES& cur_st = use_a ? vol_a_state_ : vol_b_state_;
        D3D12_RESOURCE_STATES& hist_st = use_a ? vol_b_state_ : vol_a_state_;
        float p[96] = {};
        // p[0]: tx, ty, tanx, tany
        p[0] = 1.0f / kVX;
        p[1] = 1.0f / kVY;
        p[2] = fog_tan_x_;
        p[3] = fog_tan_y_;
        // p[1..3]: view rows
        for (int i = 0; i < 12; i++)
            p[4 + i] = view_[i];
        // p[4]: s0, thick, wpm, zsign
        float end_m = fog_end_w_ / fog_wpm_frame_;
        float s0 = end_m > 0 ? 3.0f / end_m : 0.0f;
        p[16] = s0;
        p[17] = fog_thick_;
        p[18] = fog_wpm_frame_;
        p[19] = fog_zsign_;
        // p[5]: sdir, sun_str
        p[20] = sun_dir_[0];
        p[21] = sun_dir_[1];
        p[22] = sun_dir_[2];
        p[23] = sun_seen_ ? fog_sun_ : 0.0f;
        // p[6]: scol, tint
        p[24] = sun_col_[0];
        p[25] = sun_col_[1];
        p[26] = sun_col_[2];
        p[27] = fog_tint_;
        // p[7]: dk, dxk, volw, volh (set per-slice below)
        p[30] = kVX;
        p[31] = kVY;
        // p[8]: vold, time, near, far
        float secs = float(GetTickCount64() % 3600000) * 0.001f;
        p[32] = float(kVZ);
        p[33] = secs;
        p[34] = kNearM;
        p[35] = kFarM;
        // p[9]: shadow guest/host size
        if (shadow) {
            p[36] = float(shadow_target_->gw);
            p[37] = float(shadow_target_->gh);
            p[38] = float(shadow_target_->w);
            p[39] = float(shadow_target_->h);
            for (int i = 0; i < 16; i++)
                p[40 + i] = shadow_mtx_[i];
        }
        // p[14..17]: last view rows (for reprojection)
        for (int i = 0; i < 12; i++)
            p[56 + i] = last_view_[i];
        // p[18]: last_tanx, last_tany, last_zsign, have_history
        p[72] = last_tan_x_;
        p[73] = last_tan_y_;
        p[74] = last_zsign_;
        // history only from the frame just before: a frame without the volume (mode 1 or 2, a menu) breaks it
        if (vol_built_frame_ + 1 != frame_)
            have_history_ = false;
        p[75] = have_history_ ? 1.0f : 0.0f;
        // p[19]: h0, H, floor, 0
        float hw = water_h_;
        float h0 = fog_base_ >= 0 ? fog_base_ : hw;
        p[76] = h0;
        p[77] = fog_height_;
        p[78] = fog_floor_;
        p[79] = h0 + fog_ceil_;
        // p[20]: layer_s, top, soft, 0
        float layer_s = fog_layer_ > 0 && fog_layer_vis_ > 0 ? 3.0f / fog_layer_vis_ : 0.0f;
        float soft = std::clamp(fog_layer_soft_, 0.1f, std::max(0.1f, fog_layer_));
        p[80] = layer_s;
        p[81] = h0 + fog_layer_;
        p[82] = soft;
        // p[21]: nscale, noise, 0, 0
        p[84] = std::max(1.0f, fog_noise_scale_);
        p[85] = fog_noise_;
        p[86] = glow_[0] ? fog_glow_ : 0.0f;
        // the jitter: Halton bases 2, 3, 5 over 16 frames
        auto halton = [](int i, int b) {
            float f = 1, r = 0;
            for (; i > 0; i /= b)
                f /= b, r += f * (i % b);
            return r;
        };
        int hi = int(vol_frame_ & 15) + 1;
        p[92] = float(vol_frame_ & 1023);
        p[93] = halton(hi, 2);
        p[94] = halton(hi, 3);
        p[95] = halton(hi, 5);
        // p[22]: fog colour
        p[88] = fog_col_[0];
        p[89] = fog_col_[1];
        p[90] = fog_col_[2];
        // inject per slice
        c.Transition(hist.Get(), hist_st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        hist_st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (shadow)
            c.Transition(shadow_target_->res.Get(), shadow_target_->state,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (ring_)   // made by the first Fog(); until then FogCol reads a null view (no tint)
            c.Transition(ring_.Get(), ring_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (glow_[0])
            c.Transition(glow_[0].Get(), glow_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        float ratio = kFarM / kNearM;
        float step = powf(ratio, 1.0f / kVZ);
        for (int k = 0; k < kVZ; k++) {
            float dk = kNearM * powf(ratio, float(k) / kVZ);
            float dxk = kNearM * powf(ratio, float(k + 1) / kVZ) - dk;
            p[28] = dk;
            p[29] = dxk;
            c.Transition(cur.Get(), cur_st, D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12Resource* is[4]
                    = {shadow ? shadow_target_->res.Get() : nullptr, hist.Get(), ring_.Get(), glow_[0].Get()};
            DXGI_FORMAT ifm[4] = {shadow ? kDepthRead : DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                  kBgra, kBgra};
            c.Pass(kVolHlsl, "inject", DXGI_FORMAT_R16G16B16A16_FLOAT, Rtv(6 + (use_a ? 0 : kVZ) + k), kVX,
                   kVY, is, ifm, p, 96, -1);
            c.Transition(cur.Get(), cur_st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // integrate this slice: read from the current volume (just written), write to vol_int_
            c.Transition(vol_int_.Get(), vol_int_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
            float ip[4] = {float(k), kNearM, step, 0};
            ID3D12Resource* its[4] = {nullptr, cur.Get(), nullptr, nullptr};
            DXGI_FORMAT itfm[4] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN,
                                   DXGI_FORMAT_UNKNOWN};
            c.Pass(kVolHlsl, "integ", DXGI_FORMAT_R16G16B16A16_FLOAT, Rtv(6 + 2 * kVZ + k), kVX, kVY, its,
                   itfm, ip, 4, -1);
        }
        c.Transition(cur.Get(), cur_st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        c.Transition(vol_int_.Get(), vol_int_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        vol_frame_++;
        // keep last frame's view for next frame's temporal; camera jump > 5 m: no history
        float dx = view_[3] - last_view_[3], dy = view_[7] - last_view_[7], dz = view_[11] - last_view_[11];
        bool jump = have_history_ && (dx * dx + dy * dy + dz * dz) > 25.0f;
        for (int i = 0; i < 12; i++)
            last_view_[i] = view_[i];
        last_tan_x_ = fog_tan_x_;
        last_tan_y_ = fog_tan_y_;
        last_zsign_ = fog_zsign_;
        have_history_ = !jump;
        vol_built_frame_ = frame_;
        c.Unbind();
        if (!vol_logged_) {
            vol_logged_ = true;
            LOGI("fci volfog: %dx%dx%d, d %.1f-%.0f m, shadow %d", kVX, kVY, kVZ, kNearM, kFarM, int(shadow));
        }
    }

    // sky_only: the in-shader lookup fogged the surfaces; this pass fogs the sky
    bool Volfog(Context& c, Target& rt, Target& ds, bool sky_only)
    {
        float p[52] = {};
        // p[0]: tx, ty, tanx, tany
        p[0] = 1.0f / rt.w;
        p[1] = 1.0f / rt.h;
        p[2] = fog_tan_x_;
        p[3] = fog_tan_y_;
        // p[1..3]: view rows
        for (int i = 0; i < 12; i++)
            p[4 + i] = view_[i];
        // p[4]: wpm, 0, 0, 0
        p[16] = fog_wpm_frame_;
        // p[7]: near, far, vold, 0
        p[28] = kNearM;
        p[29] = kFarM;
        p[30] = float(kVZ);
        // p[8]: glow_w, 0, 0, desat
        p[32] = fog_glow_;
        p[33] = sky_only ? 1.0f : 0.0f;
        p[35] = fog_desat_;
        // p[12]: zsign, 0, 0, 0
        p[48] = fog_zsign_;
        c.Transition(ds.res.Get(), ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (glow_[0])   // last frame's glow (Fog() makes it after this pass)
            c.Transition(glow_[0].Get(), glow_state_[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12Resource* fb = c.Feedback(rt);
        if (!fb)
            return true;
        c.Transition(rt.res.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12Resource* fs[4] = {ds.res.Get(), vol_int_.Get(), fb, glow_[0].Get()};
        DXGI_FORMAT ff[4] = {kDepthRead, DXGI_FORMAT_R16G16B16A16_FLOAT, rt.fmt, kBgra};
        c.Pass(kVolHlsl, "volfog", rt.fmt, c.View(rt), rt.w, rt.h, fs, ff, p, 52, -1);
        c.Unbind();
        return true;
    }

    void Sampled(const gpu::TextureDesc& d, const Target* t, uint8_t, gpu::TexInfo&) override
    {
        if (!t)
            return;
        if (t->w == t->h && t->w >= kShadowMin && (d.format == 0x2C || d.format == 0x30)) {
            shadow_target_ = const_cast<Target*>(t);
            shadow_frame_ = frame_;
        }
    }

    uint32_t gen_ = 0;
    int ao_mode_ = 0, ao_at_ = 0, aa_mode_ = 0, shadow_filter_ = 0;
    int fog_mode_ = 0;           // 0 off, 1 the fog, 2 the debug, 3 the volume
    float fog_floor_ = 0.35f;    // a: the density floor
    float fog_height_ = 6.0f;    // H: the height scale, metres
    float fog_base_ = -1.0f;     // h0: the reference height, metres; -1 the water height
    float fog_sky_ = 0.2f;       // the sky fog's smoothstep knee
    float fog_wpm_ = 39475.0f;   // s: w-units per metre, when the projection is not found
    float fog_layer_ = 5.0f, fog_layer_vis_ = 40.0f, fog_layer_soft_ = 3.0f;   // the ground layer, metres
    float fog_noise_ = 0.8f, fog_noise_scale_ = 25.0f, fog_far_ = 500.0f;   // its noise; the sky ray's length
    float fog_wpm_frame_ = 39475.0f, fog_tan_x_ = 0.89f, fog_tan_y_ = 0.67f, fog_zsign_ = 1;   // this frame's
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
    ComPtr<ID3D12Resource> notice_;   // the picture with the host notice over it
    D3D12_RESOURCE_STATES notice_state_{};
    float fog_gate_ = 0;           // the per-draw ext[1]: kill the game fog on this draw
    bool fog_captured_ = false;    // the fog register, colour and main view are set
    bool refl_captured_ = false;   // the reflection camera is set
    bool fog_done_ = false;        // the fog ran this frame
    bool fog_logged_ = false;
    float fog_end_w_ = 0;             // the fog register's end, w-units
    float fog_thick_ = 0;             // the fog register's thickness
    float fog_col_[3] = {0, 0, 0};    // the game's fog colour
    float view_[12] = {};             // the main view rows 0..2 (world to view, metres)
    float cam_main_[3] = {0, 0, 0};   // the main camera, world metres
    float cam_refl_[3] = {0, 0, 0};   // the reflection camera, world metres
    float sun_dir_[3] = {0, 0, 1};    // toward the sun, world (Evolution's c186)
    float sun_col_[3] = {0, 0, 0};    // the sun's colour (c189)
    float water_h_ = 0;               // the fog's base: the last water height seen, metres
    bool water_seen_ = false;
    bool sun_seen_ = false;                    // a sun was found in some frame
    float fog_sun_ = 0.6f, fog_tint_ = 1.0f;   // the glow's strength; how much the fog takes the sky's colour
    float fog_steps_ = 16, fog_march_ = 150;   // the layer's march: steps and metres (0 steps: closed form)
    float fog_cap_ = 0;                        // 1: thickness caps the opacity, as the game
    ComPtr<ID3D12Resource> ring_;              // the horizon colour per azimuth, 64 x 1, kept across frames
    D3D12_RESOURCE_STATES ring_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ComPtr<ID3D12Resource> glow_[2];   // the scene's bright parts, blurred: the light in the fog
    D3D12_RESOURCE_STATES glow_state_[2]{};
    float fog_glow_ = 0.5f, fog_wisp_ = 0.6f, fog_wisp_size_ = 4.0f,
          fog_desat_ = 0.35f;                       // the fog's additions
    float fog_ceil_ = 150.0f;
    const Target* refl_rt_ = nullptr;
    float refl_wpm_ = 0;
    bool refl_logged_ = false;   // the reflection's w units per metre; 0: not found, the main view's   // the water reflection's target, fogged by its own path in mode 3   // metres above the base where the distance fog ends (mode 3)
    bool fog_lookup_ = true;                        // true: in-shader lookup, false: post-pass
    static constexpr UINT kRtvSlots = 7 + 3 * 64;   // AO(2), post, ring, glow(2), 3 volume x 64 slices, notice
    static constexpr int kNoticeSlot = 6 + 3 * 64;
                                                    // the glow's two targets
    // volume fog (mode 3)
    ComPtr<ID3D12Resource> vol_a_, vol_b_, vol_int_;
    D3D12_RESOURCE_STATES vol_a_state_{}, vol_b_state_{}, vol_int_state_{};
    bool vols_ready_ = false;
    bool vol_ready_ = false;         // the volume was built this frame (for the in-shader lookup)
    bool vol_splitscreen_ = false;   // splitscreen detected: in-shader lookup off for the frame
    int vol_frame_ = -1;
    float last_view_[12] = {};
    float last_tan_x_ = 0.89f, last_tan_y_ = 0.67f, last_zsign_ = 1;
    bool have_history_ = false;
    uint64_t vol_built_frame_ = ~0ull - 1;   // the frame the volume was last built
    Target* shadow_target_ = nullptr;
    uint64_t shadow_frame_ = 0;
    float shadow_mtx_[16] = {};
    bool vol_logged_ = false;
    bool vol_glow_logged_ = false;   // the in-shader route was logged
    static constexpr int kVX = 128, kVY = 72, kVZ = 64;
    static constexpr float kNearM = 0.5f, kFarM = 500.0f;
};

// before the backend is made (d3d8.cpp makes it at CreateDevice): g_extension is constant initialised
const bool g_registered = [] {
    gpu::SetExtension(std::make_unique<FciExtension>());
    return true;
}();
}   // namespace
