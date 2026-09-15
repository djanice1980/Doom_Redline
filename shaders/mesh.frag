#version 450
#include "common.glsl"

layout(push_constant) uniform PC {
    mat4 model;
    vec4 color;
    vec4 emissive;
} pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    // Soften the axis-aligned normals towards the viewer so voxel monsters read
    // as brightly as the camera-facing sprites they replace.
    vec3 toCam = normalize(u.cameraPos.xyz - vWorldPos);
    vec3 N = normalize(normalize(vNormal) + 0.6 * toCam);
    vec3 lit = shade(vWorldPos, N, vColor.rgb, 0.85, 0.0);
    lit += pc.emissive.rgb * pc.emissive.w;
    lit = applyFog(lit, vWorldPos);
    outColor = vec4(lit, vColor.a);
}
