#version 450
// PWR_VK_DBG=simplefs (pica_vk.c): every PICA draw's fragment shader is this one, which passes the vertex color on. The
// picture is wrong, but the GPU's time is then what the pixels cost without pica.frag's shading: rasterizing, blending
// and the targets' loads and stores. Against a normal run and PWR_VK_DBG=scissor (no pixels at all), it says how much of
// a phone's GPU time is the shading (#37).
layout(location = 0) in vec4 v_col;
layout(location = 0) out vec4 o_col;
void main() { o_col = v_col; }
