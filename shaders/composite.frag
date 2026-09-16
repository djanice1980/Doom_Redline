#version 450
// HDR scene + three bloom levels, exposure, and a soft-knee tone curve that leaves
// everything below the knee untouched (so the look stays the same) and rolls the
// highlights off instead of clipping them.
layout(set = 0, binding = 0) uniform sampler2D hdr;
layout(set = 0, binding = 1) uniform sampler2D bloom0;   // half res
layout(set = 0, binding = 2) uniform sampler2D bloom1;   // quarter
layout(set = 0, binding = 3) uniform sampler2D bloom2;   // eighth
layout(push_constant) uniform PC { vec4 p; } pc;   // x exposure, y bloom strength, z knee, w colour grade amount
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
vec3 knee(vec3 x, float k) {
    vec3 hi = k + (1.0 - k) * (1.0 - exp(-(x - k) / (1.0 - k)));
    return mix(x, hi, step(k, x));
}
void main() {
    vec3 c = texture(hdr, vUV).rgb;
    if (pc.p.y > 0.0) {
        vec3 b = texture(bloom0, vUV).rgb * 0.35 + texture(bloom1, vUV).rgb * 0.6 + texture(bloom2, vUV).rgb * 1.0;
        c += b * pc.p.y;
    }
    c = max(c * pc.p.x, 0.0);
    c = knee(c, clamp(pc.p.z, 0.1, 0.99));
    // Colour grade: a warm Doom palette lean (reds and browns up, blues down), a touch more
    // saturation and a mild S-curve. pc.p.w blends it in.
    if (pc.p.w > 0.0) {
        vec3 warm = c * vec3(1.06, 0.97, 0.86);
        float l = dot(warm, vec3(0.299, 0.587, 0.114));
        vec3 g = mix(vec3(l), warm, 1.12);
        g = mix(g, g * g * (3.0 - 2.0 * g), 0.35);
        c = mix(c, clamp(g, 0.0, 1.0), pc.p.w);
    }
    outColor = vec4(c, 1.0);
}
