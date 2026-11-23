#version 450
// Samus Returns' single-screen compositor (msr/compositor.cpp): one quad, a region of the bottom screen or a fill.
// Also OpenGL's (present.vert).
#ifndef VARYING
#define VARYING(n) layout(location = n)
#endif
#ifdef VULKAN
layout(push_constant) uniform P {
#else
layout(std140) uniform P {
#endif
    vec4 dst;      // x0, y0, x1, y1 in NDC
    vec4 src;      // the bottom screen's u0, v0, u1, v1 (screen-oriented)
    vec4 style;    // opacity, key low (mode 3: gain), key high, mode
    ivec4 flags;   // x = feather | hard edges << 8, y = circle radius (percent), z = src turned by quarter turns (MsrQuad.turn)
    vec4 color;    // fill color (mode 2)
} p;
VARYING(0) out vec2 uv;
VARYING(1) out vec2 local;
void main() {
    vec2 c = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));   // triangle strip: (0,0) (1,0) (0,1) (1,1)
    // a picture turned in its quad (clockwise on the screen, y down): the point of it at each corner
    int turn = p.flags.z & 3;
    vec2 t = turn == 1 ? vec2(c.y, 1.0 - c.x) : turn == 2 ? vec2(1.0) - c : turn == 3 ? vec2(1.0 - c.y, c.x) : c;
    uv = mix(p.src.xy, p.src.zw, t);
    local = c;
    gl_Position = vec4(mix(p.dst.xy, p.dst.zw, c), 0.0, 1.0);
}
