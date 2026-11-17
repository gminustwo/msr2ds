/* Shared between the software PICA pipeline (pica_sw.c) and the Vulkan renderer (pica_vk.c). */
#ifndef PWR_PICA_INT_H
#define PWR_PICA_INT_H
#include "gpu.h"

typedef struct { float x, y, z, w; } V4;
/* clip-space vertex after the vertex shader's output map; the Vulkan vertex layout uses these offsets */
typedef struct { V4 pos; V4 col; float uv[3][2]; float tw; V4 quat; float view[3]; } Vtx;

extern uint32_t pica_regs[0x300];
/* the fragment shader's SC_TEXEN (pica_vk.c, pica_gl.c): 0x080's texture units and unit 2's coordinates, and for each
   unit on, whether it clamps to its border colour on s (bit 16 + 2 * unit) and t (17 + 2 * unit) (wrap mode 1): a unit
   without that samples with no range check or parameter read (shaders/pica.frag sample_unit) */
static inline uint32_t pica_sc_texen(void) {
    static const uint16_t par[3] = { 0x083, 0x093, 0x09B };
    uint32_t en = pica_regs[0x080], sc = en & 0x2007;
    for (unsigned u = 0; u < 3; u++) {
        if (!(en >> u & 1)) continue;
        uint32_t p = pica_regs[par[u]];
        if ((p >> 12 & 7) == 1) sc |= 1u << (16 + 2 * u);
        if ((p >> 8 & 7) == 1) sc |= 2u << (16 + 2 * u);
    }
    return sc;
}
/* The fragment shader's lighting constants SC_LC0..SC_LCFG (pica_vk.c, pica_gl.c; shaders/pica.frag fragment_light),
   given that lighting is on: the lighting setup of a material, folded into its pipeline as Azahar builds a shader for
   each (#37: on phones the lighting read its whole setup from the uniforms every pixel, the largest per-pixel cost of
   the scene's passes). Each word keeps only the bits the shader reads for this setup, so setups that light the same
   share a pipeline: out[0] 0x1C3, [1] 0x1C4, [2] 0x1C2's light count - 1, [3] 0x1D9's light ids, [4] 0x1D0 (absolute
   inputs), [5] 0x1D1 (input selectors), [6] 0x1D2 (scales), [7] each light's 0x149 bits 0-3 at nibble id. */
static inline void pica_sc_light(uint32_t out[8]) {
    static const uint8_t ok[9] = { 0x41, 0x49, 0x43, 0x4a, 0, 0x73, 0x79, 0x4b, 0x7b };   /* LUTs each 0x1C3 configuration has (pica.frag sampler_ok) */
    static const uint8_t dis_bit[7] = { 16, 17, 18, 19, 22, 21, 20 };                    /* 0x1C4: each LUT's off bit */
    const uint32_t c0 = pica_regs[0x1C3], c1 = pica_regs[0x1C4], cfg = c0 >> 4 & 15, n = (pica_regs[0x1C2] & 7) + 1;
    uint32_t have = 0, nib = 0, perm = 0, lcfg = 0, c1m = 0;
    for (unsigned s = 0; s < 7; s++) {
        if (s == 2 || cfg >= 9 || !(ok[cfg] >> s & 1)) continue;   /* a LUT this configuration lacks: its bits unread */
        c1m |= 1u << dis_bit[s];   /* (read either way: set, it turns the LUT off) */
        if (!(c1 >> dis_bit[s] & 1)) { have |= 1u << s; nib |= 0xFu << 4 * s; }
    }
    for (unsigned li = 0; li < n; li++) {
        const uint32_t id = pica_regs[0x1D9] >> 4 * li & 7;
        perm |= id << 4 * li;
        lcfg |= (pica_regs[0x149 + 0x10 * id] & 15) << 4 * id;
        c1m |= 1u << (8 + id) | 1u << (24 + id);
        if (!(c1 >> (8 + id) & 1)) nib |= 0xFu << 8;   /* the spot LUT's input (nibble 2) */
    }
    uint32_t c0m = 0x080000FC;   /* Fresnel alpha (2, 3), the configuration (4-7), clamp highlights (27) */
    if (c0 >> 28 & 3) c0m |= 0x70C00000;   /* bump: its mode (28, 29), unit (22, 23) and renormalization (30) */
    if (!(have >> 3 & 1)) c0m &= ~0xCu;   /* no Fresnel LUT: its alpha bits unused */
    out[0] = c0 & c0m; out[1] = c1 & c1m; out[2] = n - 1; out[3] = perm;
    out[4] = pica_regs[0x1D0] & nib & 0x2222222; out[5] = pica_regs[0x1D1] & nib & 0x7777777; out[6] = pica_regs[0x1D2] & nib & 0x7777777;
    out[7] = lcfg;
}
extern uint32_t light_lut[24][256];    /* 0-6 D0 D1 - FR RB RG RR, 8-15 spot per light, 16-23 distance per light */
extern uint32_t light_lut_version;      /* bumped on every LUT write (lighting or fog) */
extern uint32_t fog_lut[128];           /* 0x0E8-0x0EF: bits 13-23 value, 0-12 signed difference (both /2047), index 0x0E6 */

float f24(uint32_t v);
float pica_f16(uint32_t v);
float pica_f20(uint32_t v);
/* texel at memory coordinates (x, y) of a tiled texture, RGBA8 packed r | g<<8 | b<<16 | a<<24 */
uint32_t pica_texel(uint32_t addr, int fmt, int w, int x, int y);
void pica_decode_level(uint32_t addr, int fmt, uint32_t w, uint32_t h, uint32_t *out);   /* a level as pica_texel, rows bottom-up */
extern const int pica_tex_bpp[14];
extern uint32_t pica_vs_prog[4096 + 128], pica_vs_version;   /* vertex shader program + swizzles, bumped on upload */
extern V4 pica_vs_cf[96];
int pica_fetch_attrs(uint32_t index, V4 *attr);
void pica_fetch_attrs_n(const uint32_t *index, uint32_t count, V4 *out);   /* pica_num_attrs() vec4 per vertex */
/* one attribute read of the array-mode vertex loaders: n components of type (0 s8, 1 u8, 2 s16, 3 f32) at
   addr + index * stride into attribute dst; loader = which of the 12 loaders */
typedef struct { uint32_t addr, stride; uint8_t type, n, dst, loader; } FetchOp;
int pica_fetch_ops(FetchOp *op, V4 *def);   /* ops of the current draw (at most 48) and every attribute's default; -1 in immediate mode */
int pica_num_attrs(void);                          /* attribute count of the current draw (array or immediate mode) */   /* attributes of one vertex (before the input permutation); returns count */

/* pica_vk.c */
int  pica_vk_enabled(void);             /* hardware renderer active (PWR_RENDERER != sw and Vulkan 1.3 present) */
int  pica_vk_gpu_vs(void);              /* vertex shaders run on the GPU (default; PWR_VK_CPU_VS=1 shades on the CPU) */
int  pica_vk_vs_ready(void);            /* ...and this draw's can now (phones: not yet translated, it is shaded on the CPU) */
/* one draw: the nvlist distinct vertices (guest index vlist_index[i], cache slot vlist_slot[i]) and the cache slot of
 * each of the n vertices in draw order; vcache = CPU-shaded vertices by slot, or NULL to shade on the GPU */
void pica_vk_draw(const Vtx *vcache, const uint32_t *vlist_index, const uint32_t *vlist_slot, uint32_t nvlist,
                  const uint32_t *slots, uint32_t n, int topo);
void pica_vk_flush(void);
void pica_vk_filled(uint32_t start, uint32_t end);   /* gsp: a MemoryFill wrote [start, end) */               /* end of a command list: finish GPU work, write render targets back */
#endif
