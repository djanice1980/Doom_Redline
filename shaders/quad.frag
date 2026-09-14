#version 450
#include "common.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec3 vLight;
layout(location = 3) in vec3 vWorldPos;
layout(location = 4) in float vMode;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(atlas, vUV);
    float a = tex.a * vColor.a;
    if (vMode < 0.5 && tex.a < 0.5) discard;   // hard-edged Doom sprites
    vec3 col = tex.rgb * vColor.rgb * vLight;
    if (vMode < 0.5) col = applyFog(col, vWorldPos);
    outColor = vec4(col, a);
}
