#version 450
// PC "grain": white, alpha MODULATE2X; blended DST_COLOR / SRC_ALPHA = dst * (1 + 2a).
layout(push_constant) uniform P { vec4 uvTop; vec4 uvBottom; vec4 p; } pc;
layout(set = 0, binding = 0) uniform sampler2D T;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(1.0, 1.0, 1.0, 2.0 * texture(T, uv).r * pc.p.x); }
