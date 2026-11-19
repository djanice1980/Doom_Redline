#version 450
// The fog, blended over time (half resolution, between the fog pass and the composite).
// Each frame the fog pass marches its few samples at a different place along every ray,
// so one frame alone is noisy; kept from frame to frame, the samples add up to a fine
// march. Last frame's result is looked up where this pixel's ray ended last frame (the
// surface it hits, through last frame's camera), held to what this frame's own
// neighbourhood says is possible (so a demon walking past or a view that just opened up
// does not drag old fog along), and blended in with this frame's.
// rgb = light added, a = how much of the scene shows through (as the fog pass writes it).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 4) uniform sampler2D fogNow;      // this frame's fog pass
layout(set = 0, binding = 5) uniform sampler2D sceneDepth;  // full res
layout(set = 0, binding = 6) uniform sampler2D fogHistory;  // last frame's result
// reproject: this frame's clip position -> last frame's clip position.
// p: x how much of this frame goes in (1 = no history: the first frame, a cut).
layout(push_constant) uniform PC { mat4 reproject; vec4 p; } pc;

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 last = textureSize(fogNow, 0) - 1;
    vec4 now = texelFetch(fogNow, px, 0);
    if (pc.p.x >= 1.0) { outColor = now; return; }
    // What this frame allows: the spread of its 3x3 pixels (mean +- 1.25 standard
    // deviations). Generous enough that the noise of one frame does not hold the
    // history to it, tight enough that fog from something that has moved is let go.
    vec4 m1 = vec4(0.0), m2 = vec4(0.0);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            vec4 c = texelFetch(fogNow, clamp(px + ivec2(dx, dy), ivec2(0), last), 0);
            m1 += c;
            m2 += c * c;
        }
    m1 /= 9.0;
    vec4 sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec4(0.0)));
    // Where this pixel's ray ended, as last frame saw it. The world pass draws with a
    // +Y-up viewport, so the top row is NDC y = +1 (as in the fog pass).
    float d = texture(sceneDepth, vUV).r;
    vec4 prev = pc.reproject * vec4(vUV.x * 2.0 - 1.0, 1.0 - 2.0 * vUV.y, d, 1.0);
    vec2 puv = vec2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
    if (prev.w <= 0.0 || any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) { outColor = now; return; }
    vec4 history = clamp(texture(fogHistory, puv), m1 - 1.25 * sigma, m1 + 1.25 * sigma);
    outColor = mix(history, now, pc.p.x);
}
