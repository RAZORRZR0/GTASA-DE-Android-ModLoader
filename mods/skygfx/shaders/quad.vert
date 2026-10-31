#version 450
// Full-screen strip quad (TL, TR, BL, BR) with per-corner UVs: PC postfx.cpp "vs". Vulkan clip space has +y down.
layout(push_constant) uniform P { vec4 uvTop; vec4 uvBottom; vec4 p; } pc;
layout(location = 0) out vec2 uv;
void main() {
    vec2 c = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    gl_Position = vec4(c.x * 2.0 - 1.0, c.y * 2.0 - 1.0, 0.0, 1.0);
    vec4 row = c.y > 0.5 ? pc.uvBottom : pc.uvTop;
    uv = c.x > 0.5 ? row.zw : row.xy;
}
