#version 450
#include "common.glsl"
// Volumetric light, at half resolution: march from the camera to the scene depth, adding
// sunlight where the shadow map says the sun reaches (shafts through the open roof and the
// board's gaps) and a warm haze around every point light. Composited additively.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 8) uniform sampler2D sceneDepth;
layout(push_constant) uniform PC { vec4 p; } pc;   // x sun density, y light density, z steps

void main() {
    if (pc.p.x <= 0.0 && pc.p.y <= 0.0) { outColor = vec4(0.0); return; }
    float d = texture(sceneDepth, vUV).r;
    // The world pass draws with a +Y-up viewport, so the top row is NDC y = +1.
    vec4 clip = vec4(vUV.x * 2.0 - 1.0, 1.0 - 2.0 * vUV.y, d, 1.0);
    vec4 wp = u.invViewProj * clip;
    wp /= wp.w;
    vec3 cam = u.cameraPos.xyz;
    vec3 ray = wp.xyz - cam;
    float len = min(length(ray), 60.0);
    vec3 dir = ray / max(length(ray), 1e-4);
    int steps = int(max(pc.p.z, 4.0));
    float stepLen = len / float(steps);
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    vec3 acc = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        float t = (float(i) + jitter) * stepLen;
        vec3 P = cam + dir * t;
        float w = stepLen * exp(-t * 0.025);
        if (pc.p.x > 0.0) {
            float sun = sunShadow(P, vec3(0.0, 1.0, 0.0));
            acc += vec3(1.0, 0.95, 0.85) * (u.sunDir.w * sun * pc.p.x * w);
        }
        if (pc.p.y > 0.0)
            for (int l = 0; l < u.counts.x; ++l) {
                vec3 dl = u.lightPos[l].xyz - P;
                float dist = length(dl);
                float att = clamp(1.0 - dist / max(u.lightPos[l].w, 0.001), 0.0, 1.0);
                att *= att;
                acc += u.lightColor[l].rgb * (u.lightColor[l].w * att * pc.p.y * w);
            }
    }
    outColor = vec4(acc, 1.0);
}
