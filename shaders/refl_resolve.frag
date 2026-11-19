#version 450
// The floor's reflections, blended over time (full res, right after the world pass).
// Each frame every reflective pixel sends one ray somewhere in its gloss lobe, a
// different spot each frame, so one frame alone is noisy; kept from frame to frame the
// rays add up to the lobe's soft image (cube_frag_body.glsl).
// Last frame's result is looked up where the reflected thing was, not where the floor
// was: a reflection sits behind the mirror, as far behind it as the reflected thing is in
// front, so it is found along this pixel's view ray, that far past the floor. It is then
// held to what this frame's own 3x3 neighbourhood says is possible, and blended in.
// Bright samples are tamed by blending in a compressed space (c / (1 + luminance)): one
// ray that catches a torch's head otherwise lights its pixel for a dozen frames.
// Output 0 is the result (copied to the history for next frame), output 1 adds it to the
// HDR image.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outRefl;
layout(location = 1) out vec4 outAdd;
layout(set = 0, binding = 4) uniform sampler2D reflNow;      // this frame: rgb = what the reflection adds, a = ray length (0 = none)
layout(set = 0, binding = 5) uniform sampler2D sceneDepth;
layout(set = 0, binding = 6) uniform sampler2D reflHistory;  // last frame's result
// reproject: this frame's clip position -> last frame's clip position.
// p: x how much of this frame goes in (1 = no history: the first frame, a cut), y, z the
// projection's depth terms (proj[2][2], proj[3][2]); q: x, y = proj[0][0], proj[1][1].
layout(push_constant) uniform PC { mat4 reproject; vec4 p; vec4 q; } pc;

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
vec3 squash(vec3 c) { return c / (1.0 + luma(c)); }
vec3 unsquash(vec3 c) { return c / max(1.0 - luma(c), 1e-4); }

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 last = textureSize(reflNow, 0) - 1;
    vec4 now = texelFetch(reflNow, px, 0);
    vec3 nowS = squash(now.rgb);
    vec3 result = now.rgb;
    if (pc.p.x < 1.0) {
        vec3 m1 = vec3(0.0), m2 = vec3(0.0);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                vec3 c = squash(texelFetch(reflNow, clamp(px + ivec2(dx, dy), ivec2(0), last), 0).rgb);
                m1 += c;
                m2 += c * c;
            }
        m1 /= 9.0;
        vec3 sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));
        // Where the reflected thing is: along the view ray, the ray's length past the
        // floor. Depth -> view distance z = proj[3][2] / (depth + proj[2][2]); a metre
        // along the ray is 1/L metres of z.
        float d = texelFetch(sceneDepth, px, 0).r;
        vec2 ndc = vec2(vUV.x * 2.0 - 1.0, 1.0 - 2.0 * vUV.y);
        float dv = d;
        if (now.a > 0.0) {
            float z = pc.p.z / (d + pc.p.y);
            float L = length(vec3(ndc.x / pc.q.x, ndc.y / pc.q.y, 1.0));
            dv = pc.p.z / (z + now.a / L) - pc.p.y;
        }
        vec4 prev = pc.reproject * vec4(ndc, dv, 1.0);
        vec2 puv = vec2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
        if (prev.w > 0.0 && all(greaterThanEqual(puv, vec2(0.0))) && all(lessThanEqual(puv, vec2(1.0)))) {
            vec3 history = squash(max(texture(reflHistory, puv).rgb, vec3(0.0)));
            history = clamp(history, m1 - 1.25 * sigma, m1 + 1.25 * sigma);
            result = unsquash(mix(history, nowS, pc.p.x));
        }
    }
    outRefl = vec4(result, 0.0);
    outAdd = vec4(result, 0.0);
}
