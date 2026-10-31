#version 450
// PC "vsDrop": positions in pixels; p.zw = 1 / size. Vulkan clip space has +y down.
layout(push_constant) uniform P { vec4 uvTop; vec4 uvBottom; vec4 p; } pc;
layout(location = 0) in vec2 xy;
layout(location = 1) in vec4 col;
layout(location = 2) in vec4 uvIn;
layout(location = 0) out vec4 c;
layout(location = 1) out vec4 uv;
void main() {
    gl_Position = vec4(xy.x * pc.p.z * 2.0 - 1.0, xy.y * pc.p.w * 2.0 - 1.0, 0.0, 1.0);
    c = col;
    uv = uvIn;
}
