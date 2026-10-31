#version 450
// PC "psDrop": uv.xy = drop mask (procedural), uv.zw = frame window (the drop acts as a lens).
layout(set = 0, binding = 0) uniform sampler2D T;
layout(location = 0) in vec4 c;
layout(location = 1) in vec4 uv;
layout(location = 0) out vec4 o;
void main() {
    vec2 d = uv.xy * 2.0 - 1.0;
    float r = dot(d, d);
    o = vec4(texture(T, uv.zw).rgb * c.rgb * mix(1.0, 0.75, r), c.a * clamp((1.0 - r) * 3.0, 0.0, 1.0));
}
