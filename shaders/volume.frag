#version 450
#include "common.glsl"
// Volumetric light and ground fog, at half resolution: march from the camera to the
// scene depth.
//  - Air: sunlight where the shadow map says the sun reaches (shafts through the open
//    roof and the board's gaps) and a warm haze around every point light. Additive.
//  - Ground fog: a layer over the floor whose depth and streaks come from the simulated
//    field (binding 9, a shallow-water sheet simulated on the CPU: bodies plough it,
//    blasts throw it, it flows back). The deeper it is the higher it stands; its surface
//    swells, and two banks of noise at different heights give it layers. It scatters the
//    ambient light, the point lights and the sun, and hides what is behind it.
// rgb = light added, a = how much of the scene shows through (the composite does c * a + rgb).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 8) uniform sampler2D sceneDepth;
layout(set = 0, binding = 9) uniform sampler2D groundFogField;
layout(push_constant) uniform PC { vec4 p; } pc;   // x sun density, y light density, z steps

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float valueNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), f.x), mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Ground fog at P: x = density (0 = clear), y = height within the layer (0 at the
// floor, 1 at its surface). The field gives the fog's depth (about 1 at rest) and its
// wisps (streaks dragged along by the flow).
vec2 groundFogAt(vec3 P) {
    float layer = u.groundFog[1].z;
    if (P.y > layer * 2.8) return vec2(0.0);
    vec2 c = clamp((P.xz - u.groundFog[0].xy) * u.groundFog[0].z, vec2(0.5), u.groundFog[1].xy - 0.5);
    vec2 field = texture(groundFogField, c / 256.0).rg;
    float depth = field.r * 2.0;
    if (depth < 0.008) return vec2(0.0);
    float t = u.cameraPos.w;
    vec2 xz = P.xz;
    // The surface: banks and hollows a few metres across roll slowly by; the deeper
    // the simulated fog, the higher it stands; and even when nothing stirs it, it
    // heaves: a few long, slow swells crossing each other, like a calm lake.
    float bank = valueNoise(xz * 0.22 + vec2(t * 0.045, -t * 0.03)) * 0.65 + valueNoise(xz * 0.5 + vec2(-t * 0.06, t * 0.05)) * 0.35;
    float swell = sin(dot(xz, vec2(0.61, 0.79)) * 0.9 + t * 0.55) * 0.5
                + sin(dot(xz, vec2(-0.83, 0.56)) * 1.4 + t * 0.8) * 0.3
                + sin(dot(xz, vec2(0.24, -0.97)) * 2.3 + t * 1.15) * 0.2;
    float top = layer * (0.2 + 0.8 * depth) * (0.45 + 1.1 * bank) * (1.0 + 0.15 * swell);
    float hf = max(P.y, 0.0) / max(top, 0.02);
    if (hf >= 1.3) return vec2(0.0, hf);
    // Inside: streaks low down and streaks up top drift different ways, so the fog
    // has layers rather than one even smoke; the simulated wisps thicken and thin them
    // where the flow has dragged them.
    vec2 q = xz * 0.6;
    float nLow = valueNoise(q + vec2(t * 0.12, t * 0.05));
    float nHigh = valueNoise(q * 1.4 + vec2(17.0 - t * 0.18, -t * 0.05));
    float streak = smoothstep(0.25, 0.75, mix(nLow, nHigh, clamp(hf, 0.0, 1.0))) * (0.35 + 1.3 * field.g);
    // Thickest at the floor, a soft edge at the top.
    float profile = (1.0 - smoothstep(0.5, 1.3, hf)) * (1.2 - 0.6 * clamp(hf, 0.0, 1.0));
    return vec2(depth * (0.1 + 1.3 * streak) * profile, hf);
}

void main() {
    bool ground = u.groundFog[0].w > 0.0;
    if (pc.p.x <= 0.0 && pc.p.y <= 0.0 && !ground) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    float d = texture(sceneDepth, vUV).r;
    // The world pass draws with a +Y-up viewport, so the top row is NDC y = +1.
    vec4 clip = vec4(vUV.x * 2.0 - 1.0, 1.0 - 2.0 * vUV.y, d, 1.0);
    vec4 wp = u.invViewProj * clip;
    wp /= wp.w;
    vec3 cam = u.cameraPos.xyz;
    vec3 ray = wp.xyz - cam;
    float len = min(length(ray), 60.0);
    vec3 dir = ray / max(length(ray), 1e-4);
    // Samples bunch up near the camera and spread out with distance (t grows with the
    // square of the step): the ground fog is a metre deep, and evenly spaced steps over
    // tens of metres would hit it once or twice and flatten it into an even smoke.
    int steps = int(max(pc.p.z, 4.0)) + (ground ? 10 : 0);
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    const vec3 sunColor = vec3(1.0, 0.95, 0.85);
    vec3 fogAmbient = u.ambient.rgb * vec3(0.92, 0.96, 1.05) * u.groundFog[1].w;
    vec3 acc = vec3(0.0);
    float T = 1.0;   // transmittance through the ground fog so far
    for (int i = 0; i < steps; ++i) {
        float a = float(i) / float(steps), b = float(i + 1) / float(steps);
        float t0 = len * a * a, t1 = len * b * b;
        float stepLen = t1 - t0;
        float t = t0 + stepLen * jitter;
        vec3 P = cam + dir * t;
        float w = stepLen * exp(-t * 0.025);
        vec2 gf = ground ? groundFogAt(P) : vec2(0.0);
        float g = gf.x;
        vec3 air = vec3(0.0);
        float sun = 0.0;
        if (pc.p.x > 0.0 || (g > 0.0 && u.groundFog[2].y > 0.0)) sun = sunShadow(P, vec3(0.0, 1.0, 0.0)) * u.sunDir.w;
        air += sunColor * (sun * pc.p.x * w);
        vec3 lit = vec3(0.0);   // point light reaching P, with a wider falloff than the air haze
        if (pc.p.y > 0.0 || g > 0.0)
            for (int l = 0; l < u.counts.x; ++l) {
                vec3 dl = u.lightPos[l].xyz - P;
                float dist = length(dl);
                float r = max(u.lightPos[l].w, 0.001);
                float att = clamp(1.0 - dist / (0.7 * r), 0.0, 1.0);   // tighter than the surface lighting
                att *= att * att;
                float wide = clamp(1.0 - dist / r, 0.0, 1.0);
                vec3 lc = u.lightColor[l].rgb * u.lightColor[l].w;
                air += lc * (att * pc.p.y * w);
                lit += lc * (wide * wide);
            }
        acc += T * air;
        if (g > 0.0) {
            float stepT = exp(-g * u.groundFog[0].w * stepLen);
            // Mist reads pale against warm light: take the lights' brightness more than their colour.
            vec3 litPale = mix(lit, vec3(dot(lit, vec3(0.3, 0.5, 0.2))), 0.6);
            // The top of the layer catches more light than the fog down at the floor: crests
            // read pale and the hollows between them deeper, which is what shows the swell.
            float relief = mix(0.5, 1.2, clamp(gf.y, 0.0, 1.0));
            vec3 scatter = (fogAmbient + litPale * u.groundFog[2].x + sunColor * (sun * u.groundFog[2].y)) * relief;
            acc += T * (1.0 - stepT) * scatter;
            T *= stepT;
        }
    }
    outColor = vec4(acc, T);
}
