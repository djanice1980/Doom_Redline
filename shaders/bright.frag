#version 450
// Bloom bright pass: keeps what is brighter than the threshold, with a soft knee.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC { vec4 p; } pc;   // x threshold, y knee width
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 c = texture(src, vUV).rgb;
    float l = max(c.r, max(c.g, c.b));
    float knee = max(pc.p.y, 1e-4);
    float soft = clamp(l - pc.p.x + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float w = max(l - pc.p.x, soft) / max(l, 1e-4);
    outColor = vec4(c * max(w, 0.0), 1.0);
}
