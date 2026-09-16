#include "common.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec4 vColor;
layout(location = 4) in vec4 vEmissive;
layout(location = 5) in vec4 vParams;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(atlas, vUV);
    vec3 albedo = tex.rgb * vColor.rgb;
    vec3 N = normalize(vNormal);
    vec3 lit = shade(vWorldPos, N, albedo, vParams.x, vParams.y);
    // flag bit 1: pulse emissive with time as well
    float e = vEmissive.w;
    if ((int(vParams.w) & 1) != 0) e *= 0.75 + 0.25 * sin(u.cameraPos.w * 6.0 + vParams.z);
    lit += vEmissive.rgb * e;
    lit = applyFog(lit, vWorldPos);
    outColor = vec4(lit, vColor.a * tex.a);
}
