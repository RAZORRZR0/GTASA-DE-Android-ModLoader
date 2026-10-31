#version 450
// PC "thresh", radiosity step 2 (the PS2 combiners): (texture - limit) saturated, doubled, saturated.
layout(push_constant) uniform P { vec4 uvTop; vec4 uvBottom; vec4 p; } pc;
layout(set = 0, binding = 0) uniform sampler2D T;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = vec4(clamp(2.0 * clamp(texture(T, uv).rgb - pc.p.x, 0.0, 1.0), 0.0, 1.0), 1.0); }
