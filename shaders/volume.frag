#version 450
#include "common.glsl"
// Volumetric light and ground fog, at half resolution: march from the camera to the
// scene depth.
//  - Air: sunlight where the shadow map says the sun reaches (shafts through the open
//    roof and the board's gaps) and a warm haze around every point light. Additive.
//  - Ground fog: a layer over the floor whose depth, streaks and flow come from the
//    simulated field (binding 9, a shallow-water sheet simulated on the CPU: bodies
//    plough it, blasts throw it, it flows back, eddies curl in a wake). The deeper it is
//    the higher it stands; its surface swells, and two banks of noise at different
//    heights give it layers. Fine wisps (binding 10, a tiling pattern) are dragged along
//    the simulated flow, far smaller than a cell: where the fog is moving they curl and
//    stream, with gaps between them, and their tops rise out of the layer. It scatters
//    the ambient light, the point lights and the sun, and hides what is behind it. It
//    has a march of its own, clipped to the layer, so every sample lands where there
//    can be fog.
// rgb = light added, a = how much of the scene shows through (the composite does c * a + rgb).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 8) uniform sampler2D sceneDepth;
layout(set = 0, binding = 9) uniform sampler2D groundFogField;   // depth / 2, wisps, flow x, flow z (0.5 = still)
layout(set = 0, binding = 10) uniform sampler2D groundFogWisps;  // tiling wisp pattern, with mips
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

const float kMaxFlow = 5.0;     // m/s at a texel of 0 or 255 (GroundFog::kMaxFlow)
const float kWispTile = 2.5;    // metres per repeat of the wisp pattern
const float kWispCycle = 2.2;   // seconds per flow-map phase: long enough to see the drift, short enough not to smear
const float kIdlePace = 0.6;    // how fast the fog's own drifting terms run (GroundFog's kIdlePace: a creep, not a churn)

// Ground fog at P: x = density (0 = clear), y = height within the layer (0 at the
// floor, 1 at its surface). The field gives the fog's depth (about 1 at rest), its
// streaks (dragged along by the flow) and the flow itself. `footprint` is the width of
// a pixel at P, in metres: the wisps are read at a mip that wide, so far fog turns to
// a haze rather than sparkling. (A sample stands for a whole step, often wider than a
// pixel, but the steps move from frame to frame and the frames are blended, so the
// wisps come out at the pixel's width, not the step's.)
// Height is different: the step runs from y0 to y1, and on a ray that grazes the
// layer that can be most of its depth. The fog's height profile (thick at the floor,
// fading out at the top) is averaged over that whole range rather than read at the one
// height the sample happens to sit at; read at a point, a step that crossed the top of
// the layer was all fog or no fog depending on where in the step its sample fell, and
// that came out as grain from pixel to pixel, more than the blend over frames can
// settle without lagging behind moving fog.
vec2 groundFogAt(vec3 P, float footprint, float y0, float y1, vec2 xzA, vec2 xzB) {
    float layer = u.groundFog[1].z;
    float yLo = max(min(y0, y1), 0.0), yHi = max(max(y0, y1), 0.0);
    if (yLo > layer * 2.8) return vec2(0.0);
    // The field's depth averaged along the step too (from xzA to xzB, three reads): at
    // the edge of fog rolling into clear floor, one read per step drew the step
    // boundaries as contour lines.
    vec2 lo = vec2(0.5), hi = u.groundFog[1].xy - 0.5;
    vec4 field = texture(groundFogField, clamp((P.xz - u.groundFog[0].xy) * u.groundFog[0].z, lo, hi) / 256.0);
    float depth = (field.r
                 + texture(groundFogField, clamp((mix(xzA, xzB, 1.0 / 6.0) - u.groundFog[0].xy) * u.groundFog[0].z, lo, hi) / 256.0).r
                 + texture(groundFogField, clamp((mix(xzA, xzB, 5.0 / 6.0) - u.groundFog[0].xy) * u.groundFog[0].z, lo, hi) / 256.0).r) * (2.0 / 3.0);
    if (depth < 0.008) return vec2(0.0);
    vec2 flow = (field.ba * 255.0 - 128.0) / 127.0 * kMaxFlow;
    float t = u.cameraPos.w;
    float tp = t * kIdlePace;   // the clock the fog's own drift runs on
    vec2 xz = P.xz;
    // The wisps: the pattern dragged along the flow (a flow map). Two copies run half a
    // cycle apart, each drifting with the flow from where it was last reset, and the
    // blend hides each one while it jumps back. The cycle starts at a different moment
    // from place to place, so the resets never pulse across the whole floor at once.
    float speed = length(flow);
    float stir = smoothstep(0.08, 0.9, speed);   // 0 lying still .. 1 well stirred
    float lod = log2(max(footprint * 256.0 / kWispTile, 1.0));
    float ph = t / kWispCycle + 2.0 * valueNoise(xz * 0.35);
    float p0 = fract(ph), p1 = fract(ph + 0.5);
    vec2 base = xz / kWispTile;
    // The pattern changes with height too: read as slices a third of the layer apart, each
    // shifted its own way, blended from one to the next. Read the same at every height,
    // every pixel of a screen column (which all look along the same line over the floor)
    // met the same wisps, and the fog stood in vertical curtains. The blend keeps the
    // contrast up between slices (a plain mix of two unrelated patterns is flatter halfway,
    // which would draw the slices as bands).
    float sy = P.y / max(layer * 0.15, 0.05);
    float si = floor(sy), sf = smoothstep(0.0, 1.0, sy - si);
    vec2 s0 = fract(vec2(0.37, 0.61) * si), s1 = fract(vec2(0.37, 0.61) * (si + 1.0));
    float sNorm = 1.0 / sqrt((1.0 - sf) * (1.0 - sf) + sf * sf);
    vec2 ua = base - flow * (p0 * kWispCycle / kWispTile);
    vec2 ub = base + vec2(0.43, 0.27) - flow * (p1 * kWispCycle / kWispTile);
    float wa = 0.5 + (mix(textureLod(groundFogWisps, ua + s0, lod).r, textureLod(groundFogWisps, ua + s1, lod).r, sf) - 0.5) * sNorm;
    float wb = 0.5 + (mix(textureLod(groundFogWisps, ub + s0, lod).r, textureLod(groundFogWisps, ub + s1, lod).r, sf) - 0.5) * sNorm;
    float wisp = smoothstep(0.25, 0.8, mix(wa, wb, abs(1.0 - 2.0 * p0)));
    // The surface: banks and hollows a few metres across roll slowly by; the deeper
    // the simulated fog, the higher it stands; and even when nothing stirs it, it
    // heaves: a few long, slow swells crossing each other, like a calm lake. Stirred
    // up, the thick of each wisp stands proud of the layer and the gaps sink into it.
    float bank = valueNoise(xz * 0.22 + vec2(tp * 0.045, -tp * 0.03)) * 0.65 + valueNoise(xz * 0.5 + vec2(-tp * 0.06, tp * 0.05)) * 0.35;
    float swell = sin(dot(xz, vec2(0.61, 0.79)) * 0.9 + tp * 0.55) * 0.5
                + sin(dot(xz, vec2(-0.83, 0.56)) * 1.4 + tp * 0.8) * 0.3
                + sin(dot(xz, vec2(0.24, -0.97)) * 2.3 + tp * 1.15) * 0.2;
    // Rolling waves through the thick of it: two long, slow wave trains (7 m and 4.5 m
    // from crest to crest, a fifth of a metre a second or so), their fronts bent by noise
    // so they curve and break up rather than marching in straight lines. In a trough the
    // fog is thin and lies low, on a crest it is thick and stands tall, so a deep bank
    // has depth to it instead of being one even slab.
    vec2 bend = vec2(valueNoise(xz * 0.11 + vec2(tp * 0.02, 3.1)), valueNoise(xz * 0.11 + vec2(7.7, -tp * 0.02))) * 5.0 - 2.5;
    vec2 wq = xz + bend;
    float roll = 0.6 * sin(dot(wq, vec2(0.8, 0.6)) * (6.2832 / 7.0) - tp * 0.3)
               + 0.4 * sin(dot(wq, vec2(-0.5, 0.866)) * (6.2832 / 4.5) - tp * 0.26);
    float rollUp = smoothstep(-0.6, 0.6, roll);    // 0 the bottom of a trough .. 1 a crest
    float top = layer * (0.2 + 0.8 * depth) * (0.45 + 1.1 * bank) * (1.0 + 0.15 * swell) * (0.5 + 0.8 * rollUp) * (1.0 + (wisp - 0.5) * (0.25 + 0.6 * stir));
    float topC = max(top, 0.02);
    float h0 = yLo / topC, h1 = yHi / topC;
    float hf = 0.5 * (h0 + h1);
    if (h0 >= 1.3) return vec2(0.0, hf);
    // Inside: streaks low down and streaks up top drift different ways, so the fog
    // has layers rather than one even smoke; the simulated wisps thicken and thin them
    // where the flow has dragged them.
    vec2 q = xz * 0.6;
    float nLow = valueNoise(q + vec2(tp * 0.12, tp * 0.05));
    float nHigh = valueNoise(q * 1.4 + vec2(17.0 - tp * 0.18, -tp * 0.05));
    float streak = smoothstep(0.25, 0.75, mix(nLow, nHigh, clamp(hf, 0.0, 1.0))) * (0.35 + 1.3 * field.g);
    // The fine wisps: lying still they only grain the mist; stirred, the gaps between
    // them open right up and the wisps themselves thicken.
    float fine = mix(0.65 + 0.7 * wisp, 0.12 + 1.9 * wisp, stir);
    // Thickest at the floor, a soft edge at the top: averaged over the step's heights
    // (four points across it; the profile is smooth, so that is plenty).
    float profile = 0.0;
    for (int k = 0; k < 4; ++k) {
        float h = mix(h0, h1, (float(k) + 0.5) * 0.25);
        profile += (1.0 - smoothstep(0.5, 1.3, h)) * (1.2 - 0.6 * clamp(h, 0.0, 1.0));
    }
    profile *= 0.25;
    return vec2(depth * (0.1 + 1.3 * streak) * fine * profile * mix(0.25, 1.4, rollUp), hf);
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
    // How wide a pixel of this (half-res) pass is per metre of distance: the angle to
    // the next pixel down.
    vec2 texel = 2.0 / vec2(textureSize(sceneDepth, 0));
    vec4 wp2 = u.invViewProj * vec4(vUV.x * 2.0 - 1.0, 1.0 - 2.0 * (vUV.y + texel.y), d, 1.0);
    float pixAngle = length(normalize(wp2.xyz / wp2.w - cam) - dir);
    // Where along each step the sample sits: a random offset per pixel, moved on by the
    // golden ratio every frame (pc.p.w counts frames), so over a few frames each pixel
    // samples its steps evenly from end to end. One frame alone is noisy; the resolve
    // that follows blends the frames, which adds them up into a far finer march.
    float jitter = fract(fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) + pc.p.w * 0.61803399);
    const vec3 sunColor = vec3(1.0, 0.95, 0.85);
    vec3 acc = vec3(0.0);

    // Ground fog first, in its own march over the part of the ray inside the layer
    // (below the highest the fog can stand; the ray is already cut at 60 m). Samples bunch up near
    // the start (t grows with the square of the step): the near fog is what shows its
    // wisps, and evenly spaced steps over tens of metres would hit a metre-deep layer
    // once or twice and flatten it into an even smoke.
    float T = 1.0;   // transmittance through the ground fog
    float fogIn = 0.0, fogOut = 0.0;
    if (ground) {
        float yTop = u.groundFog[1].z * 2.8;
        fogIn = cam.y <= yTop ? 0.0 : (dir.y < -1e-4 ? (cam.y - yTop) / -dir.y : len);
        fogOut = dir.y > 1e-4 ? min(len, (yTop - cam.y) / dir.y) : len;
    }
    if (fogOut > fogIn) {
        const int fogSteps = 16;
        vec3 fogAmbient = u.ambient.rgb * vec3(0.92, 0.96, 1.05) * u.groundFog[1].w;
        float span = fogOut - fogIn;
        for (int i = 0; i < fogSteps; ++i) {
            float a = float(i) / float(fogSteps), b = float(i + 1) / float(fogSteps);
            float t0 = fogIn + span * a * a, t1 = fogIn + span * b * b;
            float stepLen = t1 - t0;
            float t = t0 + stepLen * jitter;
            vec3 P = cam + dir * t;
            vec2 gf = groundFogAt(P, t * pixAngle, cam.y + dir.y * t0, cam.y + dir.y * t1, (cam + dir * t0).xz, (cam + dir * t1).xz);
            float g = gf.x;
            if (g <= 0.0) continue;
            float sun = u.groundFog[2].y > 0.0 ? sunShadow(P, vec3(0.0, 1.0, 0.0)) * u.sunDir.w : 0.0;
            vec3 lit = vec3(0.0);   // point light reaching P, with a wider falloff than the air haze
            for (int l = 0; l < u.counts.x; ++l) {
                float dist = length(u.lightPos[l].xyz - P);
                float wide = clamp(1.0 - dist / max(u.lightPos[l].w, 0.001), 0.0, 1.0);
                lit += u.lightColor[l].rgb * u.lightColor[l].w * (wide * wide);
            }
            float stepT = exp(-g * u.groundFog[0].w * stepLen);
            // Mist reads pale against warm light: take the lights' brightness more than their colour.
            vec3 litPale = mix(lit, vec3(dot(lit, vec3(0.3, 0.5, 0.2))), 0.6);
            // The top of the layer catches more light than the fog down at the floor: crests
            // read pale and the hollows between them deeper, which is what shows the swell
            // and the wisps standing out of it.
            float relief = mix(0.5, 1.2, clamp(gf.y, 0.0, 1.0));
            vec3 scatter = (fogAmbient + litPale * u.groundFog[2].x + sunColor * (sun * u.groundFog[2].y)) * relief;
            acc += T * (1.0 - stepT) * scatter;
            T *= stepT;
        }
    }

    // The air: sun shafts and the haze round the lights, over the whole ray. What lies
    // beyond the start of the fog shows through it as much as the fog lets it; the fog is
    // taken as even along its stretch, which is close enough for a glow this faint.
    if (pc.p.x > 0.0 || pc.p.y > 0.0) {
        int steps = int(max(pc.p.z, 4.0));
        float span = max(fogOut - fogIn, 1e-3);
        for (int i = 0; i < steps; ++i) {
            float a = float(i) / float(steps), b = float(i + 1) / float(steps);
            float t0 = len * a * a, t1 = len * b * b;
            float stepLen = t1 - t0;
            float t = t0 + stepLen * jitter;
            vec3 P = cam + dir * t;
            float w = stepLen * exp(-t * 0.025);
            vec3 air = vec3(0.0);
            if (pc.p.x > 0.0) air += sunColor * (sunShadow(P, vec3(0.0, 1.0, 0.0)) * u.sunDir.w * pc.p.x * w);
            if (pc.p.y > 0.0)
                for (int l = 0; l < u.counts.x; ++l) {
                    float dist = length(u.lightPos[l].xyz - P);
                    float att = clamp(1.0 - dist / (0.7 * max(u.lightPos[l].w, 0.001)), 0.0, 1.0);   // tighter than the surface lighting
                    air += u.lightColor[l].rgb * u.lightColor[l].w * (att * att * att * pc.p.y * w);
                }
            acc += air * pow(T, clamp((t - fogIn) / span, 0.0, 1.0));
        }
    }
    outColor = vec4(acc, T);
}
