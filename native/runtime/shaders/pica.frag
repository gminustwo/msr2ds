#version 450
#ifdef VULKAN
#extension GL_EXT_control_flow_attributes : require
#define UNROLL [[unroll]]
#endif
// PICA200 fragment pipeline: texture units 0-2, fragment lighting, 6 TEV stages + combiner buffer, alpha test, fog,
// depth from the PICA depth map. Blending, depth test and colour mask are fixed-function state.
// Mirrors pica_sw.c (the reference); u.r holds PICA registers 0x040-0x1DF of this draw.
// Also OpenGL's (pica_gl.c, GLSL 3.30): its first lines define VARYING and UNROLL empty and the SC_ constants below, the
// uniforms are a uniform buffer and the LUTs a texture buffer there.
#ifndef VARYING
#define VARYING(n) layout(location = n)
#endif
VARYING(0) in vec4 v_col;
VARYING(1) in vec4 v_uv01;
VARYING(2) in vec3 v_uv2tw;
VARYING(3) in vec4 v_quat;
VARYING(4) in vec3 v_view;
layout(location = 0) out vec4 o_col;

#ifdef VULKAN
layout(set = 0, binding = 0, std140) uniform Draw {
#else
layout(std140) uniform Draw {
#endif
    uvec4 r[104];      // registers 0x040..0x1DF
    vec4 lpos[8];      // light position (decoded f16)
    vec4 lspot[8];     // spot direction (decoded s1.11)
    vec4 latt[8];      // x = distance attenuation bias, y = scale (decoded f20)
    vec4 misc;         // x = depth scale, y = depth offset
} u;
#ifdef VULKAN
layout(set = 0, binding = 1, std430) readonly buffer Lut { uint lut[]; };   // 24 x 256 lighting LUT entries, then 128 fog LUT entries
#define LUT(i) lut[i]
layout(set = 0, binding = 2) uniform sampler2D tex0;
layout(set = 0, binding = 3) uniform sampler2D tex1;
layout(set = 0, binding = 4) uniform sampler2D tex2;
#else
uniform usamplerBuffer lut;   // the same entries, as R32UI texels
#define LUT(i) texelFetch(lut, int(i)).r
uniform sampler2D tex0;
uniform sampler2D tex1;
uniform sampler2D tex2;
#endif

uint R(uint i) { i -= 0x40u; return u.r[i >> 2][i & 3u]; }

// Specialisation constants (pica_vk.c puts them in the pipeline key): the register words that decide control flow, masked to
// the bits used, so each pipeline is compiled with its TEV / combiner-buffer / alpha-test / texture / lighting / fog / depth
// configuration folded in instead of branching on uniform reads per fragment. SC_ON = 0 (PWR_VK_NOSPEC=1) reads the registers.
#ifdef VULKAN
layout(constant_id = 0) const uint SC_ON = 0u;
layout(constant_id = 1) const uint SC_SRC0 = 0u;  layout(constant_id = 2) const uint SC_SRC1 = 0u;  layout(constant_id = 3) const uint SC_SRC2 = 0u;
layout(constant_id = 4) const uint SC_SRC3 = 0u;  layout(constant_id = 5) const uint SC_SRC4 = 0u;  layout(constant_id = 6) const uint SC_SRC5 = 0u;
layout(constant_id = 7) const uint SC_OPS0 = 0u;  layout(constant_id = 8) const uint SC_OPS1 = 0u;  layout(constant_id = 9) const uint SC_OPS2 = 0u;
layout(constant_id = 10) const uint SC_OPS3 = 0u; layout(constant_id = 11) const uint SC_OPS4 = 0u; layout(constant_id = 12) const uint SC_OPS5 = 0u;
layout(constant_id = 13) const uint SC_CMB0 = 0u; layout(constant_id = 14) const uint SC_CMB1 = 0u; layout(constant_id = 15) const uint SC_CMB2 = 0u;
layout(constant_id = 16) const uint SC_CMB3 = 0u; layout(constant_id = 17) const uint SC_CMB4 = 0u; layout(constant_id = 18) const uint SC_CMB5 = 0u;
layout(constant_id = 19) const uint SC_SCL0 = 0u; layout(constant_id = 20) const uint SC_SCL1 = 0u; layout(constant_id = 21) const uint SC_SCL2 = 0u;
layout(constant_id = 22) const uint SC_SCL3 = 0u; layout(constant_id = 23) const uint SC_SCL4 = 0u; layout(constant_id = 24) const uint SC_SCL5 = 0u;
layout(constant_id = 25) const uint SC_BUFCFG = 0u;   // 0x0E0: fog mode, combiner-buffer update masks, fog flip
layout(constant_id = 26) const uint SC_ALPHA = 0u;    // 0x104: alpha test enable and function (the reference is read from the uniforms)
layout(constant_id = 27) const uint SC_TEXEN = 0u;    // 0x080: texture units, unit 2 coordinates; 16-21: units' border wrap
layout(constant_id = 28) const uint SC_LIGHT = 0u;    // fragment lighting on
layout(constant_id = 29) const uint SC_ZBUF = 0u;     // 0x06D bit 0
// the lighting setup, when SC_LIGHT (pica_int.h pica_sc_light: only the bits read for it): 0x1C3, 0x1C4, the light count - 1,
// the lights' ids (0x1D9), 0x1D0-0x1D2 (LUT inputs: absolute, selector, scale), each light's 0x149 bits 0-3 at nibble id
layout(constant_id = 30) const uint SC_LC0 = 0u;  layout(constant_id = 31) const uint SC_LC1 = 0u;
layout(constant_id = 32) const uint SC_LNUM = 0u; layout(constant_id = 33) const uint SC_LPERM = 0u;
layout(constant_id = 34) const uint SC_LABS = 0u; layout(constant_id = 35) const uint SC_LIN = 0u;
layout(constant_id = 36) const uint SC_LSCL = 0u; layout(constant_id = 37) const uint SC_LCFG = 0u;
#endif
// per-stage selectors (constant arrays of specialisation constants are not allowed); the TEV loop is unrolled, so i is a constant
uint sc6(uint i, uint a, uint b, uint c, uint d, uint e, uint f) { return i == 0u ? a : i == 1u ? b : i == 2u ? c : i == 3u ? d : i == 4u ? e : f; }
vec4 rgba8(uint v) { return vec4(float(v & 255u), float((v >> 8) & 255u), float((v >> 16) & 255u), float(v >> 24)) / 255.0; }

vec4 sample_unit(int unit, vec2 uv) {
    // clamp to border (wrap mode 1) on s and t: specialised, SC_TEXEN bits 16 + 2 * unit (s) and 17 + 2 * unit (t), so
    // that a unit without it is a bare sample (as Azahar folds it per configuration); else from the unit's parameters
    bool bs, bt;
    if (SC_ON != 0u) { uint b = SC_TEXEN >> (16u + 2u * uint(unit)); bs = (b & 1u) != 0u; bt = (b & 2u) != 0u; }
    else { uint par = R(unit == 0 ? 0x083u : unit == 1 ? 0x093u : 0x09Bu); bs = ((par >> 12) & 7u) == 1u; bt = ((par >> 8) & 7u) == 1u; }
    if ((bs && (uv.x < 0.0 || uv.x > 1.0)) || (bt && (uv.y < 0.0 || uv.y > 1.0)))
        return rgba8(R(unit == 0 ? 0x081u : unit == 1 ? 0x091u : 0x099u));
    return unit == 0 ? texture(tex0, uv) : unit == 1 ? texture(tex1, uv) : texture(tex2, uv);
}

// ---- lighting ----
float lut_lookup(uint l, uint idx, float d) {
    uint e = LUT(l * 256u + (idx & 255u));
    int diff = int((e >> 12) & 0xfffu); if (diff >= 0x800) diff -= 0x1000;
    return float(e & 0xfffu) / 4095.0 + float(diff) / 2047.0 * d;
}
vec3 light_color(uint v) { return vec3(float((v >> 20) & 255u), float((v >> 10) & 255u), float(v & 255u)) / 255.0; }
bool sampler_ok(uint cfg, uint smp) {
    const uint ok[9] = uint[9](0x41u, 0x49u, 0x43u, 0x4au, 0u, 0x73u, 0x79u, 0x4bu, 0x7bu);
    return cfg < 9u && ((ok[cfg] >> smp) & 1u) != 0u;
}
#ifdef GENERIC_SELECTS
// Built so on Macs (CMake: MoltenVK): the generic pipeline (SC_ON = 0: each draw whose own pipeline is still compiling)
// with no array indexed at run time, its selections as selects and its pass-through stages left out. Apple's GPUs ran it
// about 17 times as long as the specialised pipelines (2026-11-18: an M3 Max at 7x, PWR_VK_NOSPEC=1, 69 ms of GPU a frame
// against about 4; 52.8 with this, the pictures the same, specialised and generic). The combiner's selections were most of
// it (its six stages without lighting 54 ms, one stage 7.6), the light loop about 15 ms. Elsewhere the shader is as it
// was (its SPIR-V byte for byte): untried there, and Adreno's compiler has crashed on changes to the light loop (#37, #58).
uint sampler_bits(uint cfg) {   // sampler_ok's table as a bit per LUT
    return cfg == 0u ? 0x41u : cfg == 1u ? 0x49u : cfg == 2u ? 0x43u : cfg == 3u ? 0x4au : cfg == 5u ? 0x73u :
           cfg == 6u ? 0x79u : cfg == 7u ? 0x4bu : cfg == 8u ? 0x7bu : 0u;
}
#define HAVE(s) ((have & (1u << s)) != 0u)
#else
#define HAVE(s) have[s]
#endif
float lut_scale(uint s) { return s == 0u ? 1.0 : s == 1u ? 2.0 : s == 2u ? 4.0 : s == 3u ? 8.0 : s == 6u ? 0.25 : s == 7u ? 0.5 : 0.0; }
vec3 qrot(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + v * q.w); }

// The lighting setup, specialized (a pipeline per material's setup, as Azahar's shader per setup: with the light count and
// ids constants the light loop unrolls and every register read in it is at a fixed place; #37), or from the registers:
// SC_ON = 0, or SC_LNUM's bit 31 (pica_vk.c leaves it to the registers on Adreno GPUs, whose shader compiler crashed on
// the specialized setup in 2.0.9: #37, #58; there the shader is 2.0.8's)
bool L_SPEC() { return SC_ON != 0u && SC_LNUM < 0x80000000u; }
uint L_C0() { return L_SPEC() ? SC_LC0 : R(0x1C3u); }
uint L_C1() { return L_SPEC() ? SC_LC1 : R(0x1C4u); }
uint L_N() { return L_SPEC() ? SC_LNUM + 1u : (R(0x1C2u) & 7u) + 1u; }
uint L_ID(uint li) { return ((L_SPEC() ? SC_LPERM : R(0x1D9u)) >> (4u * li)) & 7u; }
uint L_CFG(uint id) { return L_SPEC() ? (SC_LCFG >> (4u * id)) & 15u : R(0x149u + 0x10u * id) & 15u; }

float lut_value(uint nib, uint lut_id, vec3 normal, vec3 nview, vec3 lv, vec3 half_v, vec3 tangent, uint lid, bool two_sided) {
    uint inr = L_SPEC() ? SC_LIN : R(0x1D1u), absr = L_SPEC() ? SC_LABS : R(0x1D0u), scr = L_SPEC() ? SC_LSCL : R(0x1D2u);
    uint input_sel = (inr >> (4u * nib)) & 7u;
    bool use_abs = ((absr >> (1u + 4u * nib)) & 1u) == 0u;
    float r;
    if (input_sel == 0u) r = dot(normal, normalize(half_v));
    else if (input_sel == 1u) r = dot(nview, normalize(half_v));
    else if (input_sel == 2u) r = dot(normal, nview);
    else if (input_sel == 3u) r = dot(lv, normal);
    else if (input_sel == 4u) r = dot(lv, u.lspot[lid].xyz);
    else if (input_sel == 5u) { vec3 hp = half_v - normal * dot(normal, half_v); r = dot(normalize(hp), tangent); }
    else r = 0.0;
    uint idx; float dl;
    if (use_abs) { r = two_sided ? abs(r) : max(r, 0.0); float fl = clamp(floor(r * 256.0), 0.0, 255.0); idx = uint(fl); dl = r * 256.0 - fl; }
    else { float fl = clamp(floor(r * 128.0), -128.0, 127.0); idx = uint(int(fl)) & 255u; dl = r * 128.0 - fl; }
    return lut_lookup(lut_id, idx, dl) * lut_scale((scr >> (4u * nib)) & 7u);
}

void fragment_light(vec4 tex[3], out vec4 prim, out vec4 sec) {
    uint c0 = L_C0(), c1 = L_C1();
    uint cfg = (c0 >> 4) & 15u, bump = (c0 >> 28) & 3u, bsel = (c0 >> 22) & 3u;
    vec3 surf_n = vec3(0, 0, 1), surf_t = vec3(1, 0, 0);
    if (bump != 0u && bsel < 3u) {
#ifdef GENERIC_SELECTS
        vec3 m = (bsel == 0u ? tex[0].rgb : bsel == 1u ? tex[1].rgb : tex[2].rgb) * 2.0 - 1.0;
#else
        vec3 m = tex[bsel].rgb * 2.0 - 1.0;
#endif
        if (bump == 1u) { surf_n = m; if (((c0 >> 30) & 1u) == 0u) surf_n.z = sqrt(max(1.0 - m.x * m.x - m.y * m.y, 0.0)); }
        else surf_t = m;
    }
    vec4 q = v_quat; float ql = length(q); q = ql > 0.0 ? q / ql : vec4(0, 0, 0, 1);
    vec3 normal = qrot(q, surf_n), tangent = qrot(q, surf_t);
    vec3 view = v_view, nview = normalize(view);
    vec3 dsum = vec3(0), ssum = vec3(0);
    float dalpha = 1.0, salpha = 1.0;
    uint nlights = L_N();
#ifdef GENERIC_SELECTS
    // the LUTs sampled, the same for every light: s unless c1 disables it (s 0-3: bits 16-19, 4: 22, 5: 21, 6: 20) or the
    // configuration lacks it, never 2 (the spotlight's, per light below)
    uint d16 = c1 >> 16, have = sampler_bits(cfg) & ~((d16 & 15u) | ((d16 >> 6) & 1u) << 4 | ((d16 >> 5) & 1u) << 5 | ((d16 >> 4) & 1u) << 6) & ~4u;
#else
    const uint dis_bit[7] = uint[7](16u, 17u, 18u, 19u, 22u, 21u, 20u);
#endif
    // The loop as 2.0.8 wrote it, its reads of the setup through L_*(): specialized, the light count is a constant and the
    // driver unrolls it, each light's id, register reads and LUTs constants. (2.0.9 moved its body into a function taking
    // the LUT array, with unroll hints on this loop and the one over the LUTs; Adreno 830's compiler crashed on that, #37,
    // #58, so no hints or functions here.)
    for (uint li = 0u; li < nlights; li++) {
        uint id = L_ID(li), b = 0x140u + 0x10u * id;
        uint lcfg = L_CFG(id);
        bool directional = (lcfg & 1u) != 0u, two_sided = ((lcfg >> 1) & 1u) != 0u;
        vec3 pos = u.lpos[id].xyz;
        vec3 lv = normalize(directional ? pos : pos + view);
        vec3 half_v = nview + lv;
#ifndef GENERIC_SELECTS
        bool have[7];
        for (uint s = 0u; s < 7u; s++) have[s] = s != 2u && ((c1 >> dis_bit[s]) & 1u) == 0u && sampler_ok(cfg, s);
#endif
        float dist_att = 1.0, spot_att = 1.0;
        if (((c1 >> (24u + id)) & 1u) == 0u) {
            float d = length(view + pos);
            float sm = clamp(u.latt[id].y * d + u.latt[id].x, 0.0, 1.0);
            float fl = min(floor(sm * 256.0), 255.0);
            dist_att = lut_lookup(16u + id, uint(fl), sm * 256.0 - fl);
        }
        if (((c1 >> (8u + id)) & 1u) == 0u) spot_att = lut_value(2u, 8u + id, normal, nview, lv, half_v, tangent, id, two_sided);
        float d0 = 1.0, d1 = 1.0; vec3 refl;
        if (HAVE(0)) d0 = lut_value(0u, 0u, normal, nview, lv, half_v, tangent, id, two_sided);
        refl.x = HAVE(6) ? lut_value(6u, 6u, normal, nview, lv, half_v, tangent, id, two_sided) : 1.0;
        refl.y = HAVE(5) ? lut_value(5u, 5u, normal, nview, lv, half_v, tangent, id, two_sided) : refl.x;
        refl.z = HAVE(4) ? lut_value(4u, 4u, normal, nview, lv, half_v, tangent, id, two_sided) : refl.x;
        if (HAVE(1)) d1 = lut_value(1u, 1u, normal, nview, lv, half_v, tangent, id, two_sided);
        if (HAVE(3)) {
            float fr = lut_value(3u, 3u, normal, nview, lv, half_v, tangent, id, two_sided);
            if (((c0 >> 2) & 1u) != 0u) dalpha = fr;
            if (((c0 >> 3) & 1u) != 0u) salpha = fr;
        }
        vec3 spec0 = light_color(R(b)) * d0;
        vec3 spec1 = light_color(R(b + 1u)) * refl * d1;
        float ndl = dot(lv, normal); ndl = two_sided ? abs(ndl) : max(ndl, 0.0);
        float clamp_h = ((c0 >> 27) & 1u) != 0u ? (ndl == 0.0 ? 0.0 : 1.0) : 1.0;
        if (((lcfg >> 2) & 3u) != 0u) {
            float g = dot(half_v, half_v); g = g == 0.0 ? 0.0 : min(ndl / g, 1.0);
            if (((lcfg >> 2) & 1u) != 0u) spec0 *= g;
            if (((lcfg >> 3) & 1u) != 0u) spec1 *= g;
        }
        float att = dist_att * spot_att;
        dsum += (light_color(R(b + 2u)) * ndl + light_color(R(b + 3u))) * att;
        ssum += (spec0 + spec1) * clamp_h * att;
    }
    dsum += light_color(R(0x1C0u));
    prim = clamp(vec4(dsum, dalpha), 0.0, 1.0);
    sec = clamp(vec4(ssum, salpha), 0.0, 1.0);
}

// ---- TEV ----
#ifdef GENERIC_SELECTS
// selects, not branches: the same values (the first match of each chain below; the sources and ops are distinct)
vec4 tev_src(uint src, vec4 tex[3], vec4 prev, vec4 buf, vec4 konst, vec4 lp, vec4 ls) {
    vec4 r = vec4(0);
    r = mix(r, v_col, bvec4(src == 0u)); r = mix(r, lp, bvec4(src == 1u)); r = mix(r, ls, bvec4(src == 2u));
    r = mix(r, tex[0], bvec4(src == 3u)); r = mix(r, tex[1], bvec4(src == 4u)); r = mix(r, tex[2], bvec4(src == 5u));
    r = mix(r, buf, bvec4(src == 0xDu)); r = mix(r, konst, bvec4(src == 0xEu)); r = mix(r, prev, bvec4(src == 0xFu));
    return r;
}
vec3 oprgb(uint op, vec4 c) {
    vec3 v = c.rgb;
    v = mix(v, vec3(c.a), bvec3(op == 2u || op == 3u)); v = mix(v, vec3(c.r), bvec3(op == 4u || op == 5u));
    v = mix(v, vec3(c.g), bvec3(op == 8u || op == 9u)); v = mix(v, vec3(c.b), bvec3(op == 12u || op == 13u));
    return mix(v, 1.0 - v, bvec3(op == 1u || op == 3u || op == 5u || op == 9u || op == 13u));
}
float opa(uint op, vec4 c) {
    uint ch = op >> 1;
    float v = mix(mix(mix(c.b, c.g, ch == 2u), c.r, ch == 1u), c.a, ch == 0u);
    return mix(v, 1.0 - v, (op & 1u) != 0u);
}
vec3 comb3(uint op, vec3 a, vec3 b, vec3 c) {
    vec3 r = a;
    r = mix(r, a * b, bvec3(op == 1u)); r = mix(r, a + b, bvec3(op == 2u)); r = mix(r, a + b - 0.5, bvec3(op == 3u));
    r = mix(r, a * c + b * (1.0 - c), bvec3(op == 4u)); r = mix(r, a - b, bvec3(op == 5u));
    r = mix(r, a * b + c, bvec3(op == 8u)); r = mix(r, (a + b) * c, bvec3(op == 9u));
    return r;
}
float comb1(uint op, float a, float b, float c) {
    float r = a;
    r = mix(r, a * b, op == 1u); r = mix(r, a + b, op == 2u); r = mix(r, a + b - 0.5, op == 3u);
    r = mix(r, a * c + b * (1.0 - c), op == 4u); r = mix(r, a - b, op == 5u);
    r = mix(r, a * b + c, op == 8u); r = mix(r, (a + b) * c, op == 9u);
    return r;
}
#else
vec4 tev_src(uint src, vec4 tex[3], vec4 prev, vec4 buf, vec4 konst, vec4 lp, vec4 ls) {
    if (src == 0u) return v_col;
    if (src == 1u) return lp;
    if (src == 2u) return ls;
    if (src == 3u) return tex[0];
    if (src == 4u) return tex[1];
    if (src == 5u) return tex[2];
    if (src == 0xDu) return buf;
    if (src == 0xEu) return konst;
    if (src == 0xFu) return prev;
    return vec4(0);
}
vec3 oprgb(uint op, vec4 c) {
    if (op == 0u) return c.rgb; if (op == 1u) return 1.0 - c.rgb;
    if (op == 2u) return vec3(c.a); if (op == 3u) return vec3(1.0 - c.a);
    if (op == 4u) return vec3(c.r); if (op == 5u) return vec3(1.0 - c.r);
    if (op == 8u) return vec3(c.g); if (op == 9u) return vec3(1.0 - c.g);
    if (op == 12u) return vec3(c.b); if (op == 13u) return vec3(1.0 - c.b);
    return c.rgb;
}
float opa(uint op, vec4 c) {
    if (op == 0u) return c.a; if (op == 1u) return 1.0 - c.a; if (op == 2u) return c.r; if (op == 3u) return 1.0 - c.r;
    if (op == 4u) return c.g; if (op == 5u) return 1.0 - c.g; if (op == 6u) return c.b; return 1.0 - c.b;
}
vec3 comb3(uint op, vec3 a, vec3 b, vec3 c) {
    if (op == 0u) return a; if (op == 1u) return a * b; if (op == 2u) return a + b; if (op == 3u) return a + b - 0.5;
    if (op == 4u) return a * c + b * (1.0 - c); if (op == 5u) return a - b; if (op == 8u) return a * b + c; if (op == 9u) return (a + b) * c;
    return a;
}
float comb1(uint op, float a, float b, float c) {
    if (op == 0u) return a; if (op == 1u) return a * b; if (op == 2u) return a + b; if (op == 3u) return a + b - 0.5;
    if (op == 4u) return a * c + b * (1.0 - c); if (op == 5u) return a - b; if (op == 8u) return a * b + c; if (op == 9u) return (a + b) * c;
    return a;
}
#endif
bool cmp_func(uint f, float a, float b) {
    if (f == 0u) return false; if (f == 1u) return true; if (f == 2u) return a == b; if (f == 3u) return a != b;
    if (f == 4u) return a < b; if (f == 5u) return a <= b; if (f == 6u) return a > b; return a >= b;
}

void main() {
    uint tex_en = SC_ON != 0u ? SC_TEXEN : R(0x080u);
    vec4 tex[3] = vec4[3](vec4(0, 0, 0, 1), vec4(0, 0, 0, 1), vec4(0, 0, 0, 1));
    if ((tex_en & 1u) != 0u) tex[0] = sample_unit(0, v_uv01.xy);
    if ((tex_en & 2u) != 0u) tex[1] = sample_unit(1, v_uv01.zw);
    if ((tex_en & 4u) != 0u) tex[2] = sample_unit(2, ((tex_en >> 13) & 1u) != 0u ? v_uv01.zw : v_uv2tw.xy);
    vec4 lp = vec4(0), ls = vec4(0);
    if (SC_ON != 0u ? SC_LIGHT != 0u : (R(0x08Fu) & 1u) != 0u && (R(0x1C6u) & 1u) == 0u) fragment_light(tex, lp, ls);

    const uint tev_base[6] = uint[6](0x0C0u, 0x0C8u, 0x0D0u, 0x0D8u, 0x0F0u, 0x0F8u);
    // the combiner buffer lags a stage (as Citra models the hardware): stage 0 reads 0, stage 1 the buffer colour
    // stage 0's Previous (source 15) is that stage's third source, and the output before stage 0 is 0 (as Azahar's
    // generator, from the hardware's behaviour; pica_sw.c run_tev the same)
    vec4 prev = vec4(0.0), buf = vec4(0.0), next_buf = rgba8(R(0x0FDu));
    uint bufcfg = SC_ON != 0u ? SC_BUFCFG : R(0x0E0u);
    UNROLL for (uint i = 0u; i < 6u; i++) {
        uint b = tev_base[i];
        uint srcs = SC_ON != 0u ? sc6(i, SC_SRC0, SC_SRC1, SC_SRC2, SC_SRC3, SC_SRC4, SC_SRC5) : R(b);
        uint ops = SC_ON != 0u ? sc6(i, SC_OPS0, SC_OPS1, SC_OPS2, SC_OPS3, SC_OPS4, SC_OPS5) : R(b + 1u);
        uint cmb = SC_ON != 0u ? sc6(i, SC_CMB0, SC_CMB1, SC_CMB2, SC_CMB3, SC_CMB4, SC_CMB5) : R(b + 2u);
        uint scl = SC_ON != 0u ? sc6(i, SC_SCL0, SC_SCL1, SC_SCL2, SC_SCL3, SC_SCL4, SC_SCL5) : R(b + 4u);
#ifdef GENERIC_SELECTS
        // a stage that passes Previous through (replace; source 0 Previous, as is; scale 1) gives it as it is (clamped by
        // the stage before): its work left out (most draws use 4-6 of the 6 stages, the rest such)
        vec4 o = prev;
        if (i == 0u || (cmb & 0xF000Fu) != 0u || (srcs & 0xF000Fu) != 0xF000Fu || (ops & 0x700Fu) != 0u || (scl & 0x30003u) != 0u) {
#else
        vec4 o;
        {
#endif
        vec4 konst = rgba8(R(b + 3u));
        vec3 in_rgb[3]; float in_a[3];
        UNROLL for (uint k = 0u; k < 3u; k++) {
            uint sc = (srcs >> (4u * k)) & 15u, sa = (srcs >> (16u + 4u * k)) & 15u;
            if (i == 0u && sc == 15u) sc = (srcs >> 8u) & 15u;
            if (i == 0u && sa == 15u) sa = (srcs >> 24u) & 15u;
            in_rgb[k] = oprgb((ops >> (4u * k)) & 15u, tev_src(sc, tex, prev, buf, konst, lp, ls));
            in_a[k] = opa((ops >> (12u + 4u * k)) & 7u, tev_src(sa, tex, prev, buf, konst, lp, ls));
        }
        uint crgb = cmb & 15u, ca = (cmb >> 16) & 15u;
        if (crgb == 6u || crgb == 7u) o.rgb = vec3(4.0 * dot(in_rgb[0] - 0.5, in_rgb[1] - 0.5));
        else o.rgb = comb3(crgb, in_rgb[0], in_rgb[1], in_rgb[2]);
        o.a = crgb == 7u ? o.r : comb1(ca, in_a[0], in_a[1], in_a[2]);
        o.rgb = clamp(o.rgb * float(1u << (scl & 3u)), 0.0, 1.0);
        o.a = clamp(o.a * float(1u << ((scl >> 16) & 3u)), 0.0, 1.0);
        }
        buf = next_buf;
        if (i < 4u) {
            if (((bufcfg >> (8u + i)) & 1u) != 0u) next_buf.rgb = o.rgb;
            if (((bufcfg >> (12u + i)) & 1u) != 0u) next_buf.a = o.a;
        }
        prev = o;
    }
    // the reference value (bits 8-15) stays a uniform: games animate it (fades, dissolves), and every value would be a new pipeline
    uint at = SC_ON != 0u ? SC_ALPHA | (R(0x104u) & 0xFF00u) : R(0x104u);
    // on the 8-bit alpha, as integers (as Azahar; unrounded floats missed EQUAL and moved GREATER's edge)
    if ((at & 1u) != 0u && !cmp_func((at >> 4) & 7u, floor(prev.a * 255.0 + 0.5), float((at >> 8) & 255u))) discard;
    // depth = z/w * scale + offset (gl_FragCoord.z = -z/w, see the vertex shaders); 0x06D bit 0 clear = W-buffer: times w
    // (gl_FragCoord.w = 1/w). The world in play is W-buffered (scales like -0.0002, offset 0: depth linear in z), which
    // is not linear across the screen as the rasterizer's depth is, so only this write gives it (as in Azahar).
    bool zbuf = SC_ON != 0u ? SC_ZBUF != 0u : (R(0x06Du) & 1u) != 0u;
    float fd = clamp((zbuf ? 1.0 : 1.0 / gl_FragCoord.w) * (u.misc.y - gl_FragCoord.z * u.misc.x), 0.0, 1.0);
    if ((bufcfg & 7u) == 5u) {
        // fog: factor from the fog LUT (lut[6144 + i]: bits 13-23 value, 0-12 signed difference, both /2047) at the hardware's
        // depth * 128 (0x0E0 bit 16: 1 - depth)
        float fi = ((bufcfg >> 16) & 1u) != 0u ? (1.0 - fd) * 128.0 : fd * 128.0, i = clamp(floor(fi), 0.0, 127.0);
        uint e = LUT(6144u + uint(i));
        int diff = int(e & 0x1fffu); if (diff >= 0x1000) diff -= 0x2000;
        float f = clamp(float((e >> 13) & 0x7ffu) / 2047.0 + float(diff) / 2047.0 * (fi - i), 0.0, 1.0);
        prev.rgb = f * prev.rgb + (1.0 - f) * rgba8(R(0x0E1u)).rgb;
    }
    o_col = prev;
#ifndef NO_FRAG_DEPTH
    gl_FragDepth = fd;   // NO_FRAG_DEPTH build (pica_vk.c, Z-buffer with scale -1 and offset 0): the rasterizer's depth already is fd,
#endif                   // and not writing it lets the GPU test depth before running this shader
}
