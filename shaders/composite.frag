#version 450
// HDR scene + three bloom levels, exposure, and a soft-knee tone curve that leaves
// everything below the knee untouched (so the look stays the same) and rolls the
// highlights off instead of clipping them.
layout(set = 0, binding = 0) uniform sampler2D hdr;
layout(set = 0, binding = 1) uniform sampler2D bloom0;   // half res
layout(set = 0, binding = 2) uniform sampler2D bloom1;   // quarter
layout(set = 0, binding = 3) uniform sampler2D bloom2;   // eighth
layout(set = 0, binding = 4) uniform sampler2D fog;      // volumetric light (rgb) and what of the scene shows through the ground fog (a), half res
layout(set = 0, binding = 5) uniform sampler2D sceneDepth;
// p: x exposure, y bloom strength, z knee, w colour grade amount; q: xy the projection's
// depth terms (distance = q.y / (depth + q.x)).
layout(push_constant) uniform PC { vec4 p; vec4 q; } pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
vec3 knee(vec3 x, float k) {
    vec3 hi = k + (1.0 - k) * (1.0 - exp(-(x - k) / (1.0 - k)));
    return mix(x, hi, step(k, x));
}
float distanceAt(float d) { return pc.q.y / min(d + pc.q.x, -1e-7); }

// The fog pass runs at half resolution, and each of its pixels marches only a handful
// of samples, offset along the ray at random per pixel: thin fog, and the top edge of
// the layer, come out with a little noise from pixel to pixel. Read it back through a
// small filter over its 3x3 pixels round this one, each weighed by how close it is and
// by whether it looked at the same depth as this pixel (the depth it marched to is the
// scene depth at its centre, read the same way). That smooths the noise into fog, and
// fog in front of a demon's leg is not smeared over the floor behind it or the other
// way round.
vec4 fogAt(vec2 uv) {
    vec2 size = vec2(textureSize(fog, 0));
    vec2 hp = uv * size - 0.5;   // in fog pixels, centres on whole numbers
    ivec2 base = ivec2(floor(hp + 0.5));
    vec2 off = hp - vec2(base);
    float z = distanceAt(texture(sceneDepth, uv).r);
    float tol = 0.03 * z + 0.05;
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            ivec2 tc = clamp(base + ivec2(dx, dy), ivec2(0), ivec2(size) - 1);
            float zt = distanceAt(texture(sceneDepth, (vec2(tc) + 0.5) / size).r);
            vec2 dd = vec2(dx, dy) - off;
            float rz = (zt - z) / tol;
            float w = exp(-0.7 * dot(dd, dd)) / (1.0 + rz * rz);
            sum += texelFetch(fog, tc, 0) * w;
            wsum += w;
        }
    return wsum > 1e-5 ? sum / wsum : texture(fog, uv);
}

void main() {
    vec3 c = texture(hdr, vUV).rgb;
    if (pc.p.y > 0.0) {
        vec3 b = texture(bloom0, vUV).rgb * 0.35 + texture(bloom1, vUV).rgb * 0.6 + texture(bloom2, vUV).rgb * 1.0;
        c += b * pc.p.y;
    }
    vec4 f = fogAt(vUV);
    c = c * f.a + f.rgb;
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
