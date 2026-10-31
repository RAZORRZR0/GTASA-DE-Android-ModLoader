#version 450
// PC "ps": the source at alpha p.x.
layout(push_constant) uniform P { vec4 uvTop; vec4 uvBottom; vec4 p; } pc;
layout(set = 0, binding = 0) uniform sampler2D T;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(texture(T, uv).rgb, pc.p.x); }
